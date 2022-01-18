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

#include <string>

#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/fs.h>
#include <fs_mgr.h>
#include <libdm/dm.h>
#include <libgsi/libgsi.h>

#include "Checkpoint.h"
#include "CryptoType.h"
#include "EncryptInplace.h"
#include "KeyStorage.h"
#include "KeyUtil.h"
#include "Keymaster.h"
#include "Utils.h"
#include "VoldUtil.h"
#include <fs/Ext4.h>
#include <fs/F2fs.h>


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
    // fs_mgr_do_mount runs fsck. Use setexeccon to run trusted
    // partitions in the fsck domain.
    if (setexeccon(sFsckContext)) {
        PLOG(ERROR) << "Failed to setexeccon";
        return false;
    }
    auto mount_rc = fs_mgr_do_mount(&fstab_default, const_cast<char*>(mount_point),
                                    const_cast<char*>(blk_device), nullptr,
                                    cp_needsCheckpoint(), true);
    if (setexeccon(nullptr)) {
        PLOG(ERROR) << "Failed to clear setexeccon";
        return false;
    }
    if (mount_rc != 0) {
        LOG(ERROR) << "fs_mgr_do_mount failed with rc " << mount_rc;
        return false;
    }
    LOG(DEBUG) << "Mounted " << mount_point;
    return true;
}

static bool read_key(const std::string& metadata_key_dir, const KeyGeneration& gen,
                     KeyBuffer* key) {
    if (metadata_key_dir.empty()) {
        LOG(ERROR) << "Failed to get metadata_key_dir";
        return false;
    }
    std::string sKey;
    auto dir = metadata_key_dir + "/key";
    LOG(DEBUG) << "metadata_key_dir/key: " << dir;
    if (!MkdirsSync(dir, 0700)) return false;
    if (!pathExists(dir)) {
        auto delete_all = android::base::GetBoolProperty(
                "ro.crypto.metadata_init_delete_all_keys.enabled", false);
        if (delete_all) {
            LOG(INFO) << "Metadata key does not exist, calling deleteAllKeys";
            Keymaster::deleteAllKeys();
        } else {
            LOG(DEBUG) << "Metadata key does not exist but "
                          "ro.crypto.metadata_init_delete_all_keys.enabled is false";
        }
    }
    auto temp = metadata_key_dir + "/tmp";
    return retrieveOrGenerateKey(dir, temp, kEmptyAuthentication, gen, key);
}

static bool get_number_of_sectors(const std::string& real_blkdev, uint64_t* nr_sec) {
    if (GetBlockDev512Sectors(real_blkdev, nr_sec) != android::OK) {
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
    if (StrToHex(module_key, hex_key_buffer) != android::OK) {
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
    if (!dm.CreateDevice(dm_name, table, crypto_blkdev, std::chrono::seconds(5))) {
        PLOG(ERROR) << "Could not create default-key device " << dm_name;
        return false;
    }
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
                                      bool needs_encrypt, bool should_format,
                                      const std::string& fs_type) {
    LOG(DEBUG) << "fscrypt_mount_metadata_encrypted: " << mount_point
               << " encrypt: " << needs_encrypt << " format: " << should_format << " with "
               << fs_type;
    auto encrypted_state = android::base::GetProperty("ro.crypto.state", "");
    if (encrypted_state != "" && encrypted_state != "encrypted") {
        LOG(DEBUG) << "fscrypt_enable_crypto got unexpected starting state: " << encrypted_state;
        return false;
    }

    auto data_rec = GetEntryForMountPoint(&fstab_default, mount_point);
    if (!data_rec) {
        LOG(ERROR) << "Failed to get data_rec for " << mount_point;
        return false;
    }

    unsigned int options_format_version = android::base::GetUintProperty<unsigned int>(
            "ro.crypto.dm_default_key.options_format.version",
            (GetFirstApiLevel() <= __ANDROID_API_Q__ ? 1 : 2));

    CryptoOptions options;
    if (options_format_version == 1) {
        if (!data_rec->metadata_encryption.empty()) {
            LOG(ERROR) << "metadata_encryption options cannot be set in legacy mode";
            return false;
        }
        options.cipher = legacy_aes_256_xts;
        options.use_legacy_options_format = true;
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

    if (needs_encrypt) {
        if (should_format) {
            android::status_t error;

            if (fs_type == "ext4") {
                error = android::vold::ext4::Format(crypto_blkdev, 0, mount_point);
            } else if (fs_type == "f2fs") {
                error = android::vold::f2fs::Format(crypto_blkdev);
            } else {
                LOG(ERROR) << "Unknown filesystem type: " << fs_type;
                return false;
            }
            LOG(DEBUG) << "Format (err=" << error << ") " << crypto_blkdev << " on " << mount_point;
            if (error != 0) return false;
        } else {
            if (!encrypt_inplace(crypto_blkdev, blk_device, nr_sec, false)) return false;
        }
    }

    LOG(DEBUG) << "Mounting metadata-encrypted filesystem:" << mount_point;
    mount_via_fs_mgr(mount_point.c_str(), crypto_blkdev.c_str());

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
    LOG(DEBUG) << "defaultkey_setup_ext_volume: " << label << " " << blk_device;

    CryptoOptions options;
    if (!get_volume_options(&options)) return false;
    uint64_t nr_sec;
    return create_crypto_blk_dev(label, blk_device, key, options, out_crypto_blkdev, &nr_sec);
}

bool destroy_dsu_metadata_key(const std::string& dsu_slot) {
    LOG(DEBUG) << "destroy_dsu_metadata_key: " << dsu_slot;

    const auto dsu_metadata_key_dir = android::gsi::GetDsuMetadataKeyDir(dsu_slot);
    if (!pathExists(dsu_metadata_key_dir)) {
        LOG(DEBUG) << "DSU metadata_key_dir doesn't exist, nothing to remove: "
                   << dsu_metadata_key_dir;
        return true;
    }

    // Ensure that the DSU key directory is different from the host OS'.
    // Under normal circumstances, this should never happen, but handle it just in case.
    if (auto data_rec = GetEntryForMountPoint(&fstab_default, "/data")) {
        if (dsu_metadata_key_dir == data_rec->metadata_key_dir) {
            LOG(ERROR) << "DSU metadata_key_dir is same as host OS: " << dsu_metadata_key_dir;
            return false;
        }
    }

    bool ok = true;
    for (auto suffix : {"/key", "/tmp"}) {
        const auto key_path = dsu_metadata_key_dir + suffix;
        if (pathExists(key_path)) {
            LOG(DEBUG) << "Destroy key: " << key_path;
            if (!destroyKey(key_path)) {
                LOG(ERROR) << "Failed to destroyKey(): " << key_path;
                ok = false;
            }
        }
    }
    if (!ok) {
        return false;
    }

    LOG(DEBUG) << "Remove DSU metadata_key_dir: " << dsu_metadata_key_dir;
    // DeleteDirContentsAndDir() already logged any error, so don't log repeatedly.
    return DeleteDirContentsAndDir(dsu_metadata_key_dir) == android::OK;
}
