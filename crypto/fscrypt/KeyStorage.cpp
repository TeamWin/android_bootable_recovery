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

#include "KeyStorage.h"

#include "Checkpoint.h"
#include "Keymaster.h"
#include "ScryptParameters.h"
#include "Utils.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>

#include <cutils/properties.h>

extern "C" {

#include "crypto_scrypt.h"
}

const KeyAuthentication kEmptyAuthentication{""};

static constexpr size_t AES_KEY_BYTES = 32;
static constexpr size_t GCM_NONCE_BYTES = 12;
static constexpr size_t GCM_MAC_BYTES = 16;
static constexpr size_t SECDISCARDABLE_BYTES = 1 << 14;

static const std::string kPkmBlob("pKMblob\x00", 8);

static const char* kCurrentVersion = "1";
static const char* kRmPath = "/system/bin/rm";
static const char* kSecdiscardPath = "/system/bin/secdiscard";
static const char* kStretch_none = "none";
static const char* kStretch_nopassword = "nopassword";
static const char* kHashPrefix_secdiscardable = "Android secdiscardable SHA512";
static const char* kHashPrefix_keygen = "Android key wrapping key generation SHA512";
static const char* kFn_encrypted_key = "encrypted_key";
static const char* kFn_keymaster_key_blob = "keymaster_key_blob";
static const char* kFn_keymaster_key_blob_upgraded = "keymaster_key_blob_upgraded";
static const char* kFn_secdiscardable = "secdiscardable";
static const char* kFn_stretching = "stretching";
static const char* kFn_version = "version";

// Storage binding info for ensuring key encryption keys include a
// platform-provided seed in their derivation.
struct StorageBindingInfo {
    enum class State {
        UNINITIALIZED,
        IN_USE,    // key storage keys are bound to seed
        NOT_USED,  // key storage keys are NOT bound to seed
    };

    // Binding seed mixed into all key storage keys.
    std::vector<uint8_t> seed;

    // State tracker for the key storage key binding.
    State state = State::UNINITIALIZED;

    std::mutex guard;
};

// Never freed as the dtor is non-trivial.
StorageBindingInfo& storage_binding_info = *new StorageBindingInfo;

static bool checkSize(const std::string& kind, size_t actual, size_t expected) {
    if (actual != expected) {
        LOG(ERROR) << "Wrong number of bytes in " << kind << ", expected " << expected << " got "
                   << actual;
        return false;
    }
    return true;
}

static void hashWithPrefix(char const* prefix, const std::string& tohash, std::string* res) {
    SHA512_CTX c;

    SHA512_Init(&c);
    // Personalise the hashing by introducing a fixed prefix.
    // Hashing applications should use personalization except when there is a
    // specific reason not to; see section 4.11 of https://www.schneier.com/skein1.3.pdf
    std::string hashingPrefix = prefix;
    hashingPrefix.resize(SHA512_CBLOCK);
    SHA512_Update(&c, hashingPrefix.data(), hashingPrefix.size());
    SHA512_Update(&c, tohash.data(), tohash.size());
    res->assign(SHA512_DIGEST_LENGTH, '\0');
    SHA512_Final(reinterpret_cast<uint8_t*>(&(*res)[0]), &c);
}

// Generates a keymaster key, using rollback resistance if supported.
static bool generateKeymasterKey(Keymaster& keymaster,
                                 const km::AuthorizationSetBuilder& paramBuilder,
                                 std::string* key) {
    auto paramsWithRollback = paramBuilder;
    paramsWithRollback.Authorization(km::TAG_ROLLBACK_RESISTANCE);

    if (!keymaster.generateKey(paramsWithRollback, key)) {
        LOG(WARNING) << "Failed to generate rollback-resistant key.  This is expected if keymaster "
                        "doesn't support rollback resistance.  Falling back to "
                        "non-rollback-resistant key.";
        if (!keymaster.generateKey(paramBuilder, key)) return false;
    }
    return true;
}

static bool generateKeyStorageKey(Keymaster& keymaster, const std::string& appId,
                                  std::string* key) {
    auto paramBuilder = km::AuthorizationSetBuilder()
                                .AesEncryptionKey(AES_KEY_BYTES * 8)
                                .GcmModeMinMacLen(GCM_MAC_BYTES * 8)
                                .Authorization(km::TAG_APPLICATION_ID, appId)
                                .Authorization(km::TAG_NO_AUTH_REQUIRED);
    LOG(DEBUG) << "Generating \"key storage\" key";
    return generateKeymasterKey(keymaster, paramBuilder, key);
}

bool generateWrappedStorageKey(KeyBuffer* key) {
    Keymaster keymaster;
    if (!keymaster) return false;
    std::string key_temp;
    auto paramBuilder = km::AuthorizationSetBuilder().AesEncryptionKey(AES_KEY_BYTES * 8);
    paramBuilder.Authorization(km::TAG_STORAGE_KEY);
    if (!generateKeymasterKey(keymaster, paramBuilder, &key_temp)) return false;
    *key = KeyBuffer(key_temp.size());
    memcpy(reinterpret_cast<void*>(key->data()), key_temp.c_str(), key->size());
    return true;
}

bool exportWrappedStorageKey(const KeyBuffer& kmKey, KeyBuffer* key) {
    Keymaster keymaster;
    if (!keymaster) return false;
    std::string key_temp;

    if (!keymaster.exportKey(kmKey, &key_temp)) return false;
    *key = KeyBuffer(key_temp.size());
    memcpy(reinterpret_cast<void*>(key->data()), key_temp.c_str(), key->size());
    return true;
}

static km::AuthorizationSet beginParams(const std::string& appId) {
    return km::AuthorizationSetBuilder()
            .GcmModeMacLen(GCM_MAC_BYTES * 8)
            .Authorization(km::TAG_APPLICATION_ID, appId);
}

static bool readFileToString(const std::string& filename, std::string* result) {
    if (!android::base::ReadFileToString(filename, result)) {
        PLOG(ERROR) << "Failed to read from " << filename;
        return false;
    }
    return true;
}

static bool readRandomBytesOrLog(size_t count, std::string* out) {
    auto status = ReadRandomBytes(count, *out);
    if (status != android::OK) {
        LOG(ERROR) << "Random read failed with status: " << status;
        return false;
    }
    return true;
}

bool createSecdiscardable(const std::string& filename, std::string* hash) {
    std::string secdiscardable;
    if (!readRandomBytesOrLog(SECDISCARDABLE_BYTES, &secdiscardable)) return false;
    if (!writeStringToFile(secdiscardable, filename)) return false;
    hashWithPrefix(kHashPrefix_secdiscardable, secdiscardable, hash);
    return true;
}

bool readSecdiscardable(const std::string& filename, std::string* hash) {
    std::string secdiscardable;
    if (!readFileToString(filename, &secdiscardable)) return false;
    hashWithPrefix(kHashPrefix_secdiscardable, secdiscardable, hash);
    return true;
}

static std::mutex key_upgrade_lock;

// List of key directories that have had their Keymaster key upgraded during
// this boot and written to "keymaster_key_blob_upgraded", but replacing the old
// key was delayed due to an active checkpoint.  Protected by key_upgrade_lock.
static std::vector<std::string> key_dirs_to_commit;

// Replaces |dir|/keymaster_key_blob with |dir|/keymaster_key_blob_upgraded and
// deletes the old key from Keymaster.
static bool CommitUpgradedKey(Keymaster& keymaster, const std::string& dir) {
    auto blob_file = dir + "/" + kFn_keymaster_key_blob;
    auto upgraded_blob_file = dir + "/" + kFn_keymaster_key_blob_upgraded;

    std::string blob;
    if (!readFileToString(blob_file, &blob)) return false;

    if (rename(upgraded_blob_file.c_str(), blob_file.c_str()) != 0) {
        PLOG(ERROR) << "Failed to rename " << upgraded_blob_file << " to " << blob_file;
        return false;
    }
    // Ensure that the rename is persisted before deleting the Keymaster key.
    if (!FsyncDirectory(dir)) return false;

    if (!keymaster || !keymaster.deleteKey(blob)) {
        LOG(WARNING) << "Failed to delete old key " << blob_file
                     << " from Keymaster; continuing anyway";
        // Continue on, but the space in Keymaster used by the old key won't be freed.
    }
    return true;
}

static void DeferredCommitKeys() {
    android::base::WaitForProperty("vold.checkpoint_committed", "1");
    LOG(INFO) << "Committing upgraded keys";
    Keymaster keymaster;
    if (!keymaster) {
        LOG(ERROR) << "Failed to open Keymaster; old keys won't be deleted from Keymaster";
        // Continue on, but the space in Keymaster used by the old keys won't be freed.
    }
    std::lock_guard<std::mutex> lock(key_upgrade_lock);
    for (auto& dir : key_dirs_to_commit) {
        LOG(INFO) << "Committing upgraded key " << dir;
        CommitUpgradedKey(keymaster, dir);
    }
    key_dirs_to_commit.clear();
}

// Returns true if the Keymaster key in |dir| has already been upgraded and is
// pending being committed.  Assumes that key_upgrade_lock is held.
static bool IsKeyCommitPending(const std::string& dir) {
    for (const auto& dir_to_commit : key_dirs_to_commit) {
        if (IsSameFile(dir, dir_to_commit)) return true;
    }
    return false;
}

// Schedules the upgraded Keymaster key in |dir| to be committed later.
// Assumes that key_upgrade_lock is held.
static void ScheduleKeyCommit(const std::string& dir) {
    if (key_dirs_to_commit.empty()) std::thread(DeferredCommitKeys).detach();
    key_dirs_to_commit.push_back(dir);
}

static void CancelPendingKeyCommit(const std::string& dir) {
    std::lock_guard<std::mutex> lock(key_upgrade_lock);
    for (auto it = key_dirs_to_commit.begin(); it != key_dirs_to_commit.end(); it++) {
        if (IsSameFile(*it, dir)) {
            LOG(DEBUG) << "Cancelling pending commit of upgraded key " << dir
                       << " because it is being destroyed";
            key_dirs_to_commit.erase(it);
            break;
        }
    }
}

// Renames a key directory. Also updates the deferred commit vector
// (key_dirs_to_commit) appropriately.
//
// However, @old_name must be the path to the directory that was used to put that
// directory into the deferred commit list in the first place (since this function
// directly compares paths instead of using IsSameFile()).
static bool RenameKeyDir(const std::string& old_name, const std::string& new_name) {
    std::lock_guard<std::mutex> lock(key_upgrade_lock);

    if (rename(old_name.c_str(), new_name.c_str()) != 0) return false;

    // IsSameFile() doesn't work here since we just renamed @old_name.
    for (auto it = key_dirs_to_commit.begin(); it != key_dirs_to_commit.end(); it++) {
        if (*it == old_name) *it = new_name;
    }
    return true;
}

// Deletes a leftover upgraded key, if present.  An upgraded key can be left
// over if an update failed, or if we rebooted before committing the key in a
// freak accident.  Either way, we can re-upgrade the key if we need to.
static void DeleteUpgradedKey(Keymaster& keymaster, const std::string& path) {
    if (pathExists(path)) {
        LOG(DEBUG) << "Deleting leftover upgraded key " << path;
        std::string blob;
        if (!android::base::ReadFileToString(path, &blob)) {
            LOG(WARNING) << "Failed to read leftover upgraded key " << path
                         << "; continuing anyway";
        } else if (!keymaster.deleteKey(blob)) {
            LOG(WARNING) << "Failed to delete leftover upgraded key " << path
                         << " from Keymaster; continuing anyway";
        }
        if (unlink(path.c_str()) != 0) {
            LOG(WARNING) << "Failed to unlink leftover upgraded key " << path
                         << "; continuing anyway";
        }
    }
}

// Begins a Keymaster operation using the key stored in |dir|.
static KeymasterOperation BeginKeymasterOp(Keymaster& keymaster, const std::string& dir,
                                           const km::AuthorizationSet& keyParams,
                                           const km::AuthorizationSet& opParams,
                                           km::AuthorizationSet* outParams) {
    km::AuthorizationSet inParams(keyParams);
    inParams.append(opParams.begin(), opParams.end());

    auto blob_file = dir + "/" + kFn_keymaster_key_blob;
    auto upgraded_blob_file = dir + "/" + kFn_keymaster_key_blob_upgraded;

    std::lock_guard<std::mutex> lock(key_upgrade_lock);

    std::string blob;
    bool already_upgraded = IsKeyCommitPending(dir);
    if (already_upgraded) {
        LOG(DEBUG)
                << blob_file
                << " was already upgraded and is waiting to be committed; using the upgraded blob";
        if (!readFileToString(upgraded_blob_file, &blob)) return KeymasterOperation();
    } else {
        DeleteUpgradedKey(keymaster, upgraded_blob_file);
        if (!readFileToString(blob_file, &blob)) return KeymasterOperation();
    }

    auto opHandle = keymaster.begin(blob, inParams, outParams);
    if (!opHandle) return opHandle;

    // If key blob wasn't upgraded, nothing left to do.
    if (!opHandle.getUpgradedBlob()) return opHandle;

    if (already_upgraded) {
        LOG(ERROR) << "Unexpected case; already-upgraded key " << upgraded_blob_file
                   << " still requires upgrade";
        return KeymasterOperation();
    }
    LOG(INFO) << "Upgrading key: " << blob_file;
    if (!writeStringToFile(*opHandle.getUpgradedBlob(), upgraded_blob_file))
        return KeymasterOperation();
    if (cp_needsCheckpoint()) {
        LOG(INFO) << "Wrote upgraded key to " << upgraded_blob_file
                  << "; delaying commit due to checkpoint";
        ScheduleKeyCommit(dir);
    } else {
        if (!CommitUpgradedKey(keymaster, dir)) return KeymasterOperation();
        LOG(INFO) << "Key upgraded: " << blob_file;
    }
    return opHandle;
}

static bool encryptWithKeymasterKey(Keymaster& keymaster, const std::string& dir,
                                    const km::AuthorizationSet& keyParams,
                                    const KeyBuffer& message, std::string* ciphertext) {
    km::AuthorizationSet opParams =
            km::AuthorizationSetBuilder()
                    .Authorization(km::TAG_ROLLBACK_RESISTANCE)
                    .Authorization(km::TAG_PURPOSE, km::KeyPurpose::ENCRYPT);
    km::AuthorizationSet outParams;
    auto opHandle = BeginKeymasterOp(keymaster, dir, keyParams, opParams, &outParams);
    if (!opHandle) return false;
    auto nonceBlob = outParams.GetTagValue(km::TAG_NONCE);
    if (!nonceBlob) {
        LOG(ERROR) << "GCM encryption but no nonce generated";
        return false;
    }
    // nonceBlob here is just a pointer into existing data, must not be freed
    std::string nonce(nonceBlob.value().get().begin(), nonceBlob.value().get().end());
    if (!checkSize("nonce", nonce.size(), GCM_NONCE_BYTES)) return false;
    std::string body;
    if (!opHandle.updateCompletely(message, &body)) return false;

    std::string mac;
    if (!opHandle.finish(&mac)) return false;
    if (!checkSize("mac", mac.size(), GCM_MAC_BYTES)) return false;
    *ciphertext = nonce + body + mac;
    return true;
}

static bool decryptWithKeymasterKey(Keymaster& keymaster, const std::string& dir,
                                    const km::AuthorizationSet& keyParams,
                                    const std::string& ciphertext, KeyBuffer* message,
                                    bool keepOld) {
    const std::string nonce = ciphertext.substr(0, GCM_NONCE_BYTES);
    auto bodyAndMac = ciphertext.substr(GCM_NONCE_BYTES);
    auto opParams = km::AuthorizationSetBuilder()
                            .Authorization(km::TAG_NONCE, nonce)
                            .Authorization(km::TAG_ROLLBACK_RESISTANCE)
                            .Authorization(km::TAG_PURPOSE, km::KeyPurpose::DECRYPT);
    auto opHandle = BeginKeymasterOp(keymaster, dir, keyParams, opParams, nullptr);
    if (!opHandle) return false;
    if (!opHandle.updateCompletely(bodyAndMac, message)) return false;
    if (!opHandle.finish(nullptr)) return false;
    return true;
}

static std::string getStretching(const KeyAuthentication& auth) {
    if (auth.usesKeymaster()) {
        return kStretch_nopassword;
    } else {
        return kStretch_none;
    }
}

static bool stretchSecret(const std::string& stretching, const std::string& secret,
                          std::string* stretched) {
    if (stretching == kStretch_nopassword) {
        if (!secret.empty()) {
            LOG(WARNING) << "Password present but stretching is nopassword";
            // Continue anyway
        }
        stretched->clear();
    } else if (stretching == kStretch_none) {
        *stretched = secret;
    } else {
        LOG(ERROR) << "Unknown stretching type: " << stretching;
        return false;
    }
    return true;
}

static bool generateAppId(const KeyAuthentication& auth, const std::string& stretching,
                          const std::string& secdiscardable_hash, std::string* appId) {
    std::string stretched;
    if (!stretchSecret(stretching, auth.secret, &stretched)) return false;
    *appId = secdiscardable_hash + stretched;

    const std::lock_guard<std::mutex> scope_lock(storage_binding_info.guard);
    switch (storage_binding_info.state) {
        case StorageBindingInfo::State::UNINITIALIZED:
            storage_binding_info.state = StorageBindingInfo::State::NOT_USED;
            break;
        case StorageBindingInfo::State::IN_USE:
            appId->append(storage_binding_info.seed.begin(), storage_binding_info.seed.end());
            break;
        case StorageBindingInfo::State::NOT_USED:
            // noop
            break;
    }

    return true;
}

static void logOpensslError() {
    LOG(ERROR) << "Openssl error: " << ERR_get_error();
}

static bool encryptWithoutKeymaster(const std::string& preKey, const KeyBuffer& plaintext,
                                    std::string* ciphertext) {
    std::string key;
    hashWithPrefix(kHashPrefix_keygen, preKey, &key);
    key.resize(AES_KEY_BYTES);
    if (!readRandomBytesOrLog(GCM_NONCE_BYTES, ciphertext)) return false;
    auto ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)>(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) {
        logOpensslError();
        return false;
    }
    if (1 != EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), NULL,
                                reinterpret_cast<const uint8_t*>(key.data()),
                                reinterpret_cast<const uint8_t*>(ciphertext->data()))) {
        logOpensslError();
        return false;
    }
    ciphertext->resize(GCM_NONCE_BYTES + plaintext.size() + GCM_MAC_BYTES);
    int outlen;
    if (1 != EVP_EncryptUpdate(
                 ctx.get(), reinterpret_cast<uint8_t*>(&(*ciphertext)[0] + GCM_NONCE_BYTES),
                 &outlen, reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size())) {
        logOpensslError();
        return false;
    }
    if (outlen != static_cast<int>(plaintext.size())) {
        LOG(ERROR) << "GCM ciphertext length should be " << plaintext.size() << " was " << outlen;
        return false;
    }
    if (1 != EVP_EncryptFinal_ex(
                 ctx.get(),
                 reinterpret_cast<uint8_t*>(&(*ciphertext)[0] + GCM_NONCE_BYTES + plaintext.size()),
                 &outlen)) {
        logOpensslError();
        return false;
    }
    if (outlen != 0) {
        LOG(ERROR) << "GCM EncryptFinal should be 0, was " << outlen;
        return false;
    }
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, GCM_MAC_BYTES,
                                 reinterpret_cast<uint8_t*>(&(*ciphertext)[0] + GCM_NONCE_BYTES +
                                                            plaintext.size()))) {
        logOpensslError();
        return false;
    }
    return true;
}

static bool decryptWithoutKeymaster(const std::string& preKey, const std::string& ciphertext,
                                    KeyBuffer* plaintext) {
    if (ciphertext.size() < GCM_NONCE_BYTES + GCM_MAC_BYTES) {
        LOG(ERROR) << "GCM ciphertext too small: " << ciphertext.size();
        return false;
    }
    std::string key;
    hashWithPrefix(kHashPrefix_keygen, preKey, &key);
    key.resize(AES_KEY_BYTES);
    auto ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)>(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) {
        logOpensslError();
        return false;
    }
    if (1 != EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), NULL,
                                reinterpret_cast<const uint8_t*>(key.data()),
                                reinterpret_cast<const uint8_t*>(ciphertext.data()))) {
        logOpensslError();
        return false;
    }
    *plaintext = KeyBuffer(ciphertext.size() - GCM_NONCE_BYTES - GCM_MAC_BYTES);
    int outlen;
    if (1 != EVP_DecryptUpdate(ctx.get(), reinterpret_cast<uint8_t*>(&(*plaintext)[0]), &outlen,
                               reinterpret_cast<const uint8_t*>(ciphertext.data() + GCM_NONCE_BYTES),
                               plaintext->size())) {
        logOpensslError();
        return false;
    }
    if (outlen != static_cast<int>(plaintext->size())) {
        LOG(ERROR) << "GCM plaintext length should be " << plaintext->size() << " was " << outlen;
        return false;
    }
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, GCM_MAC_BYTES,
                                 const_cast<void*>(reinterpret_cast<const void*>(
                                     ciphertext.data() + GCM_NONCE_BYTES + plaintext->size())))) {
        logOpensslError();
        return false;
    }
    if (1 != EVP_DecryptFinal_ex(ctx.get(),
                                 reinterpret_cast<uint8_t*>(&(*plaintext)[0] + plaintext->size()),
                                 &outlen)) {
        logOpensslError();
        return false;
    }
    if (outlen != 0) {
        LOG(ERROR) << "GCM EncryptFinal should be 0, was " << outlen;
        return false;
    }
    return true;
}

bool storeKey(const std::string& dir, const KeyAuthentication& auth, const KeyBuffer& key) {
    if (TEMP_FAILURE_RETRY(mkdir(dir.c_str(), 0700)) == -1) {
        PLOG(ERROR) << "key mkdir " << dir;
        return false;
    }
    if (!writeStringToFile(kCurrentVersion, dir + "/" + kFn_version)) return false;
    std::string secdiscardable_hash;
    if (!createSecdiscardable(dir + "/" + kFn_secdiscardable, &secdiscardable_hash)) return false;
    std::string stretching = getStretching(auth);
    if (!writeStringToFile(stretching, dir + "/" + kFn_stretching)) return false;
    std::string appId;
    if (!generateAppId(auth, stretching, secdiscardable_hash, &appId)) return false;
    std::string encryptedKey;
    if (auth.usesKeymaster()) {
        Keymaster keymaster;
        if (!keymaster) return false;
        std::string kmKey;
        if (!generateKeyStorageKey(keymaster, appId, &kmKey)) return false;
        if (!writeStringToFile(kmKey, dir + "/" + kFn_keymaster_key_blob)) return false;
        km::AuthorizationSet keyParams = beginParams(appId);
        if (!encryptWithKeymasterKey(keymaster, dir, keyParams, key, &encryptedKey)) return false;
    } else {
        if (!encryptWithoutKeymaster(appId, key, &encryptedKey)) return false;
    }
    if (!writeStringToFile(encryptedKey, dir + "/" + kFn_encrypted_key)) return false;
    if (!FsyncDirectory(dir)) return false;
    return true;
}

bool storeKeyAtomically(const std::string& key_path, const std::string& tmp_path,
                        const KeyAuthentication& auth, const KeyBuffer& key) {
    if (pathExists(key_path)) {
        LOG(ERROR) << "Already exists, cannot create key at: " << key_path;
        return false;
    }
    if (pathExists(tmp_path)) {
        LOG(DEBUG) << "Already exists, destroying: " << tmp_path;
        destroyKey(tmp_path);  // May be partially created so ignore errors
    }
    if (!storeKey(tmp_path, auth, key)) return false;

    if (!RenameKeyDir(tmp_path, key_path)) {
        PLOG(ERROR) << "Unable to move new key to location: " << key_path;
        return false;
    }
    if (!FsyncParentDirectory(key_path)) return false;
    LOG(DEBUG) << "Created key: " << key_path;
    return true;
}

bool retrieveKey(const std::string& dir, const KeyAuthentication& auth, KeyBuffer* key,
                 bool keepOld) {    
    std::string version;
    if (!readFileToString(dir + "/" + kFn_version, &version)) return false;
    if (version != kCurrentVersion) {
        LOG(ERROR) << "Version mismatch, expected " << kCurrentVersion << " got " << version;
        return false;
    }
    std::string secdiscardable_hash;
    if (!readSecdiscardable(dir + "/" + kFn_secdiscardable, &secdiscardable_hash)) return false;
    std::string stretching;
    if (!readFileToString(dir + "/" + kFn_stretching, &stretching)) return false;
    std::string appId;
    if (!generateAppId(auth, stretching, secdiscardable_hash, &appId)) return false;
    std::string encryptedMessage;
    if (!readFileToString(dir + "/" + kFn_encrypted_key, &encryptedMessage)) return false;
    if (auth.usesKeymaster()) {
        Keymaster keymaster;
        if (!keymaster) return false;
        km::AuthorizationSet keyParams = beginParams(appId);
        if (!decryptWithKeymasterKey(keymaster, dir, keyParams, encryptedMessage, key,
                                             keepOld))
            return false;
    } else {
        if (!decryptWithoutKeymaster(appId, encryptedMessage, key)) return false;
    }
    return true;
}

static bool DeleteKeymasterKey(const std::string& blob_file) {
    std::string blob;
    if (!readFileToString(blob_file, &blob)) return false;
    Keymaster keymaster;
    if (!keymaster) return false;
    LOG(DEBUG) << "Deleting key " << blob_file << " from Keymaster";
    if (!keymaster.deleteKey(blob)) return false;
    return true;
}

bool runSecdiscardSingle(const std::string& file) {
    if (ForkExecvp(std::vector<std::string>{kSecdiscardPath, "--", file}) != 0) {
        LOG(ERROR) << "secdiscard failed";
        return false;
    }
    return true;
}

static bool recursiveDeleteKey(const std::string& dir) {
    if (ForkExecvp(std::vector<std::string>{kRmPath, "-rf", dir}) != 0) {
        LOG(ERROR) << "recursive delete failed";
        return false;
    }
    return true;
}

bool destroyKey(const std::string& dir) {
    bool success = true;

    CancelPendingKeyCommit(dir);

    auto secdiscard_cmd = std::vector<std::string>{
        kSecdiscardPath,
        "--",
        dir + "/" + kFn_encrypted_key,
        dir + "/" + kFn_secdiscardable,
    };
    // Try each thing, even if previous things failed.

    for (auto& fn : {kFn_keymaster_key_blob, kFn_keymaster_key_blob_upgraded}) {
        auto blob_file = dir + "/" + fn;
        if (pathExists(blob_file)) {
            success &= DeleteKeymasterKey(blob_file);
            secdiscard_cmd.push_back(blob_file);
        }
    }
    if (ForkExecvp(secdiscard_cmd) != 0) {
        LOG(ERROR) << "secdiscard failed";
        success = false;
    }
    success &= recursiveDeleteKey(dir);
    return success;
}

bool setKeyStorageBindingSeed(const std::vector<uint8_t>& seed) {
    const std::lock_guard<std::mutex> scope_lock(storage_binding_info.guard);
    switch (storage_binding_info.state) {
        case StorageBindingInfo::State::UNINITIALIZED:
            storage_binding_info.state = StorageBindingInfo::State::IN_USE;
            storage_binding_info.seed = seed;
            return true;
        case StorageBindingInfo::State::IN_USE:
            LOG(ERROR) << "key storage binding seed already set";
            return false;
        case StorageBindingInfo::State::NOT_USED:
            LOG(ERROR) << "key storage already in use without binding";
            return false;
    }
    return false;
}
