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

#include "Keymaster.h"

#include <android-base/logging.h>

#include <aidl/android/hardware/security/keymint/SecurityLevel.h>
#include <aidl/android/security/maintenance/IKeystoreMaintenance.h>
#include <aidl/android/system/keystore2/Domain.h>
#include <aidl/android/system/keystore2/EphemeralStorageKeyResponse.h>
#include <aidl/android/system/keystore2/KeyDescriptor.h>

// Keep these in sync with system/security/keystore2/src/keystore2_main.rs
static constexpr const char keystore2_service_name[] =
        "android.system.keystore2.IKeystoreService/default";
static constexpr const char maintenance_service_name[] = "android.security.maintenance";

/*
 * Keep this in sync with the description for update() in
 * system/hardware/interfaces/keystore2/aidl/android/system/keystore2/IKeystoreOperation.aidl
 */
static constexpr const size_t UPDATE_INPUT_MAX_SIZE = 32 * 1024;  // 32 KiB

// Keep this in sync with system/sepolicy/private/keystore2_key_contexts
static constexpr const int VOLD_NAMESPACE = 100;

namespace ks2_maint = ::aidl::android::security::maintenance;

KeymasterOperation::~KeymasterOperation() {
    if (ks2Operation) ks2Operation->abort();
}

static void zeroize_vector(std::vector<uint8_t>& vec) {
    memset_s(vec.data(), 0, vec.size());
}

static bool logKeystore2ExceptionIfPresent(::ndk::ScopedAStatus& rc, const std::string& func_name) {
    if (rc.isOk()) return false;

    auto exception_code = rc.getExceptionCode();
    if (exception_code == EX_SERVICE_SPECIFIC) {
        LOG(ERROR) << "keystore2 Keystore " << func_name
                   << " returned service specific error: " << rc.getServiceSpecificError();
    } else {
        LOG(ERROR) << "keystore2 Communication with Keystore " << func_name
                   << " failed error: " << exception_code;
    }
    return true;
}

bool KeymasterOperation::updateCompletely(const char* input, size_t inputLen,
                                          const std::function<void(const char*, size_t)> consumer) {
    if (!ks2Operation) return false;

    while (inputLen != 0) {
        size_t currLen = std::min(inputLen, UPDATE_INPUT_MAX_SIZE);
        std::vector<uint8_t> input_vec(input, input + currLen);
        inputLen -= currLen;
        input += currLen;

        std::optional<std::vector<uint8_t>> output;
        auto rc = ks2Operation->update(input_vec, &output);
        zeroize_vector(input_vec);
        if (logKeystore2ExceptionIfPresent(rc, "update")) {
            ks2Operation = nullptr;
            return false;
        }
        if (output) consumer((const char*)output->data(), output->size());
    }
    return true;
}

bool KeymasterOperation::finish(std::string* output) {
    std::optional<std::vector<uint8_t>> out_vec;

    if (!ks2Operation) return false;

    auto rc = ks2Operation->finish(std::nullopt, std::nullopt, &out_vec);
    if (logKeystore2ExceptionIfPresent(rc, "finish")) {
        ks2Operation = nullptr;
        return false;
    }

    if (output) *output = std::string(out_vec->begin(), out_vec->end());

    return true;
}

Keymaster::Keymaster() {
    ::ndk::SpAIBinder binder(AServiceManager_waitForService(keystore2_service_name));
    auto keystore2Service = ks2::IKeystoreService::fromBinder(binder);

    if (!keystore2Service) {
        LOG(ERROR) << "Vold unable to connect to keystore2.";
        return;
    }

    /*
     * There are only two options available to vold for the SecurityLevel: TRUSTED_ENVIRONMENT (TEE)
     * and STRONGBOX. We don't use STRONGBOX because if a TEE is present it will have Weaver, which
     * already strengthens CE, so there's no additional benefit from using StrongBox.
     *
     * The picture is slightly more complicated because Keystore2 reports a SOFTWARE instance as
     * a TEE instance when there isn't a TEE instance available, but in that case, a STRONGBOX
     * instance won't be available either, so we'll still be doing the best we can.
     */
    auto rc = keystore2Service->getSecurityLevel(km::SecurityLevel::TRUSTED_ENVIRONMENT,
                                                 &securityLevel);
    if (logKeystore2ExceptionIfPresent(rc, "getSecurityLevel"))
        LOG(ERROR) << "Vold unable to get security level from keystore2.";
}

bool Keymaster::generateKey(const km::AuthorizationSet& inParams, std::string* key) {
    ks2::KeyDescriptor in_key = {
            .domain = ks2::Domain::BLOB,
            .alias = std::nullopt,
            .nspace = VOLD_NAMESPACE,
            .blob = std::nullopt,
    };
    ks2::KeyMetadata keyMetadata;
    auto rc = securityLevel->generateKey(in_key, std::nullopt, inParams.vector_data(), 0, {},
                                         &keyMetadata);

    if (logKeystore2ExceptionIfPresent(rc, "generateKey")) return false;

    if (keyMetadata.key.blob == std::nullopt) {
        LOG(ERROR) << "keystore2 generated key blob was null";
        return false;
    }
    if (key) *key = std::string(keyMetadata.key.blob->begin(), keyMetadata.key.blob->end());

    zeroize_vector(keyMetadata.key.blob.value());
    return true;
}

bool Keymaster::exportKey(const KeyBuffer& kmKey, std::string* key) {
    bool ret = false;
    ks2::KeyDescriptor storageKey = {
            .domain = ks2::Domain::BLOB,
            .alias = std::nullopt,
            .nspace = VOLD_NAMESPACE,
    };
    storageKey.blob = std::make_optional<std::vector<uint8_t>>(kmKey.begin(), kmKey.end());
    ks2::EphemeralStorageKeyResponse ephemeral_key_response;
    auto rc = securityLevel->convertStorageKeyToEphemeral(storageKey, &ephemeral_key_response);

    if (logKeystore2ExceptionIfPresent(rc, "exportKey")) goto out;
    if (key)
        *key = std::string(ephemeral_key_response.ephemeralKey.begin(),
                           ephemeral_key_response.ephemeralKey.end());

    // TODO b/185811713 store the upgraded key blob if provided and delete the old key blob.

    ret = true;
out:
    zeroize_vector(ephemeral_key_response.ephemeralKey);
    zeroize_vector(storageKey.blob.value());
    return ret;
}

bool Keymaster::deleteKey(const std::string& key) {
    ks2::KeyDescriptor keyDesc = {
            .domain = ks2::Domain::BLOB,
            .alias = std::nullopt,
            .nspace = VOLD_NAMESPACE,
    };
    keyDesc.blob =
            std::optional<std::vector<uint8_t>>(std::vector<uint8_t>(key.begin(), key.end()));

    auto rc = securityLevel->deleteKey(keyDesc);
    return !logKeystore2ExceptionIfPresent(rc, "deleteKey");
}

KeymasterOperation Keymaster::begin(const std::string& key, const km::AuthorizationSet& inParams,
                                    km::AuthorizationSet* outParams) {
    ks2::KeyDescriptor keyDesc = {
            .domain = ks2::Domain::BLOB,
            .alias = std::nullopt,
            .nspace = VOLD_NAMESPACE,
    };
    keyDesc.blob =
            std::optional<std::vector<uint8_t>>(std::vector<uint8_t>(key.begin(), key.end()));

    ks2::CreateOperationResponse cor;
    auto rc = securityLevel->createOperation(keyDesc, inParams.vector_data(), true, &cor);
    if (logKeystore2ExceptionIfPresent(rc, "createOperation")) {
        if (rc.getExceptionCode() == EX_SERVICE_SPECIFIC)
            return KeymasterOperation((km::ErrorCode)rc.getServiceSpecificError());
        else
            return KeymasterOperation();
    }

    if (!cor.iOperation) {
        LOG(ERROR) << "keystore2 createOperation didn't return an operation";
        return KeymasterOperation();
    }

    if (outParams && cor.parameters) *outParams = cor.parameters->keyParameter;

    return KeymasterOperation(cor.iOperation, cor.upgradedBlob);
}

void Keymaster::earlyBootEnded() {
    ::ndk::SpAIBinder binder(AServiceManager_getService(maintenance_service_name));
    auto maint_service = ks2_maint::IKeystoreMaintenance::fromBinder(binder);

    if (!maint_service) {
        LOG(ERROR) << "Unable to connect to keystore2 maintenance service for earlyBootEnded";
        return;
    }

    auto rc = maint_service->earlyBootEnded();
    logKeystore2ExceptionIfPresent(rc, "earlyBootEnded");
}

void Keymaster::deleteAllKeys() {
    ::ndk::SpAIBinder binder(AServiceManager_getService(maintenance_service_name));
    auto maint_service = ks2_maint::IKeystoreMaintenance::fromBinder(binder);

    if (!maint_service) {
        LOG(ERROR) << "Unable to connect to keystore2 maintenance service for deleteAllKeys";
        return;
    }

    auto rc = maint_service->deleteAllKeys();
    logKeystore2ExceptionIfPresent(rc, "deleteAllKeys");
}
