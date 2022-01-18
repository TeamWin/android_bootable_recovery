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
// TODO: Maybe "Keymaster" should be replaced with Keystore2 everywhere?
#ifndef ANDROID_VOLD_KEYMASTER_H
#define ANDROID_VOLD_KEYMASTER_H

#include "KeyBuffer.h"

#include <memory>
#include <string>
#include <utility>

#include <android-base/macros.h>
#include <keymint_support/authorization_set.h>
#include <keymint_support/keymint_tags.h>

#include <aidl/android/hardware/security/keymint/ErrorCode.h>
#include <aidl/android/system/keystore2/IKeystoreService.h>
#include <android/binder_manager.h>

namespace ks2 = ::aidl::android::system::keystore2;
namespace km = ::aidl::android::hardware::security::keymint;

// C++ wrappers to the Keystore2 AIDL interface.
// This is tailored to the needs of KeyStorage, but could be extended to be
// a more general interface.

// Wrapper for a Keystore2 operation handle representing an
// ongoing Keystore2 operation.  Aborts the operation
// in the destructor if it is unfinished. Methods log failures
// to LOG(ERROR).
class KeymasterOperation {
  public:
    ~KeymasterOperation();
    // Is this instance valid? This is false if creation fails, and becomes
    // false on finish or if an update fails.
    explicit operator bool() const { return (bool)ks2Operation; }
    km::ErrorCode getErrorCode() const { return errorCode; }
    std::optional<std::string> getUpgradedBlob() const { return upgradedBlob; }
    // Call "update" repeatedly until all of the input is consumed, and
    // concatenate the output. Return true on success.
    template <class TI, class TO>
    bool updateCompletely(TI& input, TO* output) {
        if (output) output->clear();
        return updateCompletely(input.data(), input.size(), [&](const char* b, size_t n) {
            if (output) std::copy(b, b + n, std::back_inserter(*output));
        });
    }

    // Finish and write the output to this string, unless pointer is null.
    bool finish(std::string* output);
    // Move constructor
    KeymasterOperation(KeymasterOperation&& rhs) { *this = std::move(rhs); }
    // Construct an object in an error state for error returns
    KeymasterOperation() { errorCode = km::ErrorCode::UNKNOWN_ERROR; }
    // Move Assignment
    KeymasterOperation& operator=(KeymasterOperation&& rhs) {
        ks2Operation = rhs.ks2Operation;
        rhs.ks2Operation = nullptr;

        upgradedBlob = rhs.upgradedBlob;
        rhs.upgradedBlob = std::nullopt;

        errorCode = rhs.errorCode;
        rhs.errorCode = km::ErrorCode::UNKNOWN_ERROR;

        return *this;
    }

  private:
    KeymasterOperation(std::shared_ptr<ks2::IKeystoreOperation> ks2Op,
                       std::optional<std::vector<uint8_t>> blob)
        : ks2Operation{ks2Op}, errorCode{km::ErrorCode::OK} {
        if (blob)
            upgradedBlob = std::optional(std::string(blob->begin(), blob->end()));
        else
            upgradedBlob = std::nullopt;
    }

    KeymasterOperation(km::ErrorCode errCode) : errorCode{errCode} {}

    bool updateCompletely(const char* input, size_t inputLen,
                          const std::function<void(const char*, size_t)> consumer);

    std::shared_ptr<ks2::IKeystoreOperation> ks2Operation;
    std::optional<std::string> upgradedBlob;
    km::ErrorCode errorCode;
    DISALLOW_COPY_AND_ASSIGN(KeymasterOperation);
    friend class Keymaster;
};

// Wrapper for keystore2 methods that vold uses.
class Keymaster {
  public:
    Keymaster();
    // false if we failed to get a keystore2 security level.
    explicit operator bool() { return (bool)securityLevel; }
    // Generate a key using keystore2 from the given params.
    bool generateKey(const km::AuthorizationSet& inParams, std::string* key);
    // Exports a keystore2 key with STORAGE_KEY tag wrapped with a per-boot ephemeral key
    bool exportKey(const KeyBuffer& kmKey, std::string* key);
    // If supported, permanently delete a key from the keymint device it belongs to.
    bool deleteKey(const std::string& key);
    // Begin a new cryptographic operation, collecting output parameters if pointer is non-null
    // If the key was upgraded as a result of a call to this method, the returned KeymasterOperation
    // also stores the upgraded key blob.
    KeymasterOperation begin(const std::string& key, const km::AuthorizationSet& inParams,
                             km::AuthorizationSet* outParams);

    // Tell all Keymint devices that early boot has ended and early boot-only keys can no longer
    // be created or used.
    static void earlyBootEnded();

    // Tell all Keymint devices to delete all rollback-protected keys.
    static void deleteAllKeys();

  private:
    std::shared_ptr<ks2::IKeystoreSecurityLevel> securityLevel;
    DISALLOW_COPY_AND_ASSIGN(Keymaster);
};
#endif
