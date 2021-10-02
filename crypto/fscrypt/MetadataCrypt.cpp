/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MetadataCrypt.h"
#include "KeyBuffer.h"

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/fs.h>
#include <fs_mgr.h>
#include <libdm/dm.h>

#include "Checkpoint.h"
#include "CryptoType.h"
#include "EncryptInplace.h"
#include "FsCrypt.h"
#include "KeyStorage.h"
#include "KeyUtil.h"
#include "Keymaster.h"
#include "Utils.h"
#include "VoldUtil.h"

#define TABLE_LOAD_RETRIES 10


using android::fs_mgr::FstabEntry;
using android::fs_mgr::GetEntryForMountPoint;
using ::KeyBuffer;
using namespace android::dm;

// Parsed from metadata options
struct CryptoOptions {
    struct CryptoType cipher = invalid_crypto_type;
    bool use_legacy_options_format = false;
    bool set_dun = true;  // Non-legacy driver always sets DUN
    bool use_hw_wrapped_key = false;
};

static const std::string kDmNameUserdata = "userdata";

static const char* kFn_keymaster_key_blob = "keymaster_key_blob";
static const char* kFn_keymaster_key_blob_upgraded = "keymaster_key_blob_upgraded";

// The first entry in this table is the default crypto type.
constexpr CryptoType supported_crypto_types[] = {aes_256_xts, adiantum};

static_assert(validateSupportedCryptoTypes(64, supported_crypto_types,
                                           array_length(supported_crypto_types)),
              "We have a CryptoType which was incompletely constructed.");

constexpr CryptoType legacy_aes_256_xts =
        CryptoType().set_config_name("aes-256-xts").set_kernel_name("AES-256-XTS").set_keysize(64);

static_assert(isValidCryptoType(64, legacy_aes_256_xts),
              "We have a CryptoType which was incompletely constructed.");

// Returns KeyGeneration suitable for key as described in CryptoOptions
const KeyGeneration makeGen(const CryptoOptions& options) {
    return KeyGeneration{options.cipher.get_keysize(), true, options.use_hw_wrapped_key};
}

static bool mount_via_fs_mgr(const char* mount_point, const char* blk_device) {
    // We're about to mount data not verified by verified boot.  Tell Keymaster instances that early
    // boot has ended.
    ::Keymaster::earlyBootEnded();

    // fs_mgr_do_mount runs fsck. Use setexeccon to run trusted
    // partitions in the fsck domain.
    if (setexeccon(::sFsckContext)) {
        PLOG(ERROR) << "Failed to setexeccon";
        return false;
    }

    if (fstab_default.empty()) {
        if (!ReadDefaultFstab(&fstab_default)) {
            PLOG(ERROR) << "Failed to open default fstab";
            return -1;
        }
    }
    auto mount_rc = fs_mgr_do_mount(&fstab_default, const_cast<char*>(mount_point),
                                    const_cast<char*>(blk_device), nullptr,
                                    ::cp_needsCheckpoint(), true);
    if (setexeccon(nullptr)) {
        PLOG(ERROR) << "Failed to clear setexeccon";
        return false;
    }
    if (mount_rc != 0) {
        LOG(ERROR) << "fs_mgr_do_mount failed with rc " << mount_rc;
        return false;
    }
    LOG(INFO) << "mount_via_fs_mgr::Mounted " << mount_point;
    return true;
}

// Note: It is possible to orphan a key if it is removed before deleting
// Update this once keymaster APIs change, and we have a proper commit.
static void commit_key(const std::string& dir) {
    while (!android::base::WaitForProperty("vold.checkpoint_committed", "1")) {
        LOG(ERROR) << "Wait for boot timed out";
    }
    Keymaster keymaster;
    auto keyPath = dir + "/" + kFn_keymaster_key_blob;
    auto newKeyPath = dir + "/" + kFn_keymaster_key_blob_upgraded;
    std::string key;

    if (!android::base::ReadFileToString(keyPath, &key)) {
        LOG(ERROR) << "Failed to read old key: " << dir;
        return;
    }
    if (rename(newKeyPath.c_str(), keyPath.c_str()) != 0) {
        PLOG(ERROR) << "Unable to move upgraded key to location: " << keyPath;
        return;
    }
    if (!keymaster.deleteKey(key)) {
        LOG(ERROR) << "Key deletion failed during upgrade, continuing anyway: " << dir;
    }
    LOG(INFO) << "Old Key deleted: " << dir;
}

static bool read_key(const std::string& metadata_key_dir, const KeyGeneration& gen,
                     KeyBuffer* key) {
    if (metadata_key_dir.empty()) {
        LOG(ERROR) << "Failed to get metadata_key_dir";
        return false;
    }
    std::string sKey;
    auto dir = metadata_key_dir + "/key";
    LOG(INFO) << "metadata_key_dir/key: " << dir;
    if (fs_mkdirs(dir.c_str(), 0700)) {
        PLOG(ERROR) << "Creating directories: " << dir;
        return false;
    }
    auto temp = metadata_key_dir + "/tmp";
    auto newKeyPath = dir + "/" + kFn_keymaster_key_blob_upgraded;
    /* If we have a leftover upgraded key, delete it.
     * We either failed an update and must return to the old key,
     * or we rebooted before commiting the keys in a freak accident.
     * Either way, we can re-upgrade the key if we need to.
     */

    Keymaster keymaster;
    if (pathExists(newKeyPath)) {
        if (!android::base::ReadFileToString(newKeyPath, &sKey))
            LOG(ERROR) << "Failed to read incomplete key: " << dir;
        else if (!keymaster.deleteKey(sKey))
            LOG(ERROR) << "Incomplete key deletion failed, continuing anyway: " << dir;
        else
            unlink(newKeyPath.c_str());
    }
    bool needs_cp = cp_needsCheckpoint();
    if (!retrieveOrGenerateKey(dir, temp, kEmptyAuthentication, gen, key, true)) return false;
    if (needs_cp && pathExists(newKeyPath)) std::thread(commit_key, dir).detach();
    return true;
}

static bool get_number_of_sectors(const std::string& real_blkdev, uint64_t* nr_sec) {
    if (::GetBlockDev512Sectors(real_blkdev, nr_sec) != android::OK) {
        PLOG(ERROR) << "Unable to measure size of " << real_blkdev;
        return false;
    }
    return true;
}

static bool create_crypto_blk_dev(const std::string& dm_name, const std::string& blk_device,
                                  const KeyBuffer& key, const CryptoOptions& options,
                                  std::string* crypto_blkdev, uint64_t* nr_sec) {
    if (!get_number_of_sectors(blk_device, nr_sec)) return false;
    // TODO(paulcrowley): don't hardcode that DmTargetDefaultKey uses 4096-byte
    // sectors
    *nr_sec &= ~7;

    KeyBuffer module_key;
    if (options.use_hw_wrapped_key) {
        if (!exportWrappedStorageKey(key, &module_key)) {
            LOG(ERROR) << "Failed to get ephemeral wrapped key";
            return false;
        }
    } else {
        module_key = key;
    }

    KeyBuffer hex_key_buffer;
    if (::StrToHex(module_key, hex_key_buffer) != android::OK) {
        LOG(ERROR) << "Failed to turn key to hex";
        return false;
    }
    std::string hex_key(hex_key_buffer.data(), hex_key_buffer.size());

    auto target = std::make_unique<DmTargetDefaultKey>(0, *nr_sec, options.cipher.get_kernel_name(),
                                                       hex_key, blk_device, 0);
    if (options.use_legacy_options_format) target->SetUseLegacyOptionsFormat();
    if (options.set_dun) target->SetSetDun();
    if (options.use_hw_wrapped_key) target->SetWrappedKeyV0();

    DmTable table;
    table.AddTarget(std::move(target));

    auto& dm = DeviceMapper::Instance();
    for (int i = 0;; i++) {
        if (dm.CreateDevice(dm_name, table)) {
            break;
        }
        if (i + 1 >= TABLE_LOAD_RETRIES) {
            PLOG(ERROR) << "Could not create default-key device " << dm_name;
            return false;
        }
        PLOG(INFO) << "Could not create default-key device, retrying";
        usleep(500000);
    }

    if (!dm.GetDmDevicePathByName(dm_name, crypto_blkdev)) {
        LOG(ERROR) << "Cannot retrieve default-key device status " << dm_name;
        return false;
    }
    std::stringstream ss;
    ss << *crypto_blkdev;
    LOG(INFO) << "Created device: " << ss.str();
    return true;
}

static const CryptoType& lookup_cipher(const std::string& cipher_name) {
    if (cipher_name.empty()) return supported_crypto_types[0];
    for (size_t i = 0; i < array_length(supported_crypto_types); i++) {
        if (cipher_name == supported_crypto_types[i].get_config_name()) {
            return supported_crypto_types[i];
        }
    }
    return invalid_crypto_type;
}

static bool parse_options(const std::string& options_string, CryptoOptions* options) {
    auto parts = android::base::Split(options_string, ":");
    if (parts.size() < 1 || parts.size() > 2) {
        LOG(ERROR) << "Invalid metadata encryption option: " << options_string;
        return false;
    }
    std::string cipher_name = parts[0];
    options->cipher = lookup_cipher(cipher_name);
    if (options->cipher.get_kernel_name() == nullptr) {
        LOG(ERROR) << "No metadata cipher named " << cipher_name << " found";
        return false;
    }

    if (parts.size() == 2) {
        if (parts[1] == "wrappedkey_v0") {
            options->use_hw_wrapped_key = true;
        } else {
            LOG(ERROR) << "Invalid metadata encryption flag: " << parts[1];
            return false;
        }
    }
    return true;
}

bool fscrypt_mount_metadata_encrypted(const std::string& blk_device, const std::string& mount_point,
                                      bool needs_encrypt) {
    LOG(INFO) << "fscrypt_mount_metadata_encrypted: " << mount_point << " " << needs_encrypt;
    auto encrypted_state = android::base::GetProperty("ro.crypto.state", "");
    if (encrypted_state != "" && encrypted_state != "encrypted") {
        LOG(ERROR) << "fscrypt_enable_crypto got unexpected starting state: " << encrypted_state;
        return false;
    }
    if (fstab_default.empty()) {
        if (!ReadDefaultFstab(&fstab_default)) {
            PLOG(ERROR) << "Failed to open default fstab";
            return -1;
        }
    }
    auto data_rec = GetEntryForMountPoint(&fstab_default, mount_point);
    if (!data_rec) {
        LOG(ERROR) << "Failed to get data_rec for " << mount_point;
        return false;
    }

    constexpr unsigned int pre_gki_level = 29;
    unsigned int options_format_version = android::base::GetUintProperty<unsigned int>(
            "ro.crypto.dm_default_key.options_format.version",
            (GetFirstApiLevel() <= pre_gki_level ? 1 : 2));

    CryptoOptions options;
    if (options_format_version == 1) {
        if (!data_rec->metadata_encryption.empty()) {
            LOG(ERROR) << "metadata_encryption options cannot be set in legacy mode";
            return false;
        }
        options.cipher = legacy_aes_256_xts;
        options.use_legacy_options_format = true;
        if (is_metadata_wrapped_key_supported()) {
            options.use_hw_wrapped_key = true;
            LOG(INFO) << "metadata_wrapped_key_true";
        }
        options.set_dun = android::base::GetBoolProperty("ro.crypto.set_dun", false);
        if (!options.set_dun && data_rec->fs_mgr_flags.checkpoint_blk) {
            LOG(ERROR)
                    << "Block checkpoints and metadata encryption require ro.crypto.set_dun option";
            return false;
        }
    } else if (options_format_version == 2) {
        if (!parse_options(data_rec->metadata_encryption, &options)) return false;
    } else {
        LOG(ERROR) << "Unknown options_format_version: " << options_format_version;
        return false;
    }
    auto gen = needs_encrypt ? makeGen(options) : neverGen();
    KeyBuffer key;
    if (!read_key(data_rec->metadata_key_dir, gen, &key)) return false;

    std::string crypto_blkdev;
    uint64_t nr_sec;
    if (!create_crypto_blk_dev(kDmNameUserdata, blk_device, key, options, &crypto_blkdev, &nr_sec))
        return false;

    // FIXME handle the corrupt case
    if (needs_encrypt) {
        LOG(INFO) << "Beginning inplace encryption, nr_sec: " << nr_sec;
        off64_t size_already_done = 0;
        auto rc = cryptfs_enable_inplace(crypto_blkdev.data(), blk_device.data(), nr_sec,
                                         &size_already_done, nr_sec, 0, false);
        if (rc != 0) {
            LOG(ERROR) << "Inplace crypto failed with code: " << rc;
            return false;
        }
        if (static_cast<uint64_t>(size_already_done) != nr_sec) {
            LOG(ERROR) << "Inplace crypto only got up to sector: " << size_already_done;
            return false;
        }
        LOG(INFO) << "Inplace encryption complete";
    }

    LOG(INFO) << "Mounting metadata-encrypted filesystem:" << mount_point;
    mount_via_fs_mgr(mount_point.c_str(), crypto_blkdev.c_str());
    android::base::SetProperty("ro.crypto.fs_crypto_blkdev", crypto_blkdev);

    // Record that there's at least one fstab entry with metadata encryption
    if (!android::base::SetProperty("ro.crypto.metadata.enabled", "true")) {
        LOG(WARNING) << "failed to set ro.crypto.metadata.enabled";  // This isn't fatal
    }
    return true;
}

static bool get_volume_options(CryptoOptions* options) {
    return parse_options(android::base::GetProperty("ro.crypto.volume.metadata.encryption", ""),
                         options);
}

bool defaultkey_volume_keygen(KeyGeneration* gen) {
    CryptoOptions options;
    if (!get_volume_options(&options)) return false;
    *gen = makeGen(options);
    return true;
}

bool defaultkey_setup_ext_volume(const std::string& label, const std::string& blk_device,
                                 const KeyBuffer& key, std::string* out_crypto_blkdev) {
    LOG(ERROR) << "defaultkey_setup_ext_volume: " << label << " " << blk_device;

    CryptoOptions options;
    if (!get_volume_options(&options)) return false;
    uint64_t nr_sec;
    return create_crypto_blk_dev(label, blk_device, key, options, out_crypto_blkdev, &nr_sec);
}
