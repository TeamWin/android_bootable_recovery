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

#ifndef ANDROID_VOLD_KEYUTIL_H
#define ANDROID_VOLD_KEYUTIL_H

#include "KeyBuffer.h"
#include "KeyStorage.h"

#include <fscrypt/fscrypt.h>

#include <memory>
#include <string>


using namespace android::fscrypt;

// Description of how to generate a key when needed.
struct KeyGeneration {
    size_t keysize;
    bool allow_gen;
    bool use_hw_wrapped_key;
};

// Generate a key as specified in KeyGeneration
bool generateStorageKey(const KeyGeneration& gen, KeyBuffer* key);

// Returns a key with allow_gen false so generateStorageKey returns false;
// this is used to indicate to retrieveOrGenerateKey that a key should not
// be generated.
const KeyGeneration neverGen();

bool isFsKeyringSupported(void);

// Install a file-based encryption key to the kernel, for use by encrypted files
// on the specified filesystem using the specified encryption policy version.
//
// For v1 policies, we use FS_IOC_ADD_ENCRYPTION_KEY if the kernel supports it.
// Otherwise we add the key to the global session keyring as a "logon" key.
//
// For v2 policies, we always use FS_IOC_ADD_ENCRYPTION_KEY; it's the only way
// the kernel supports.
//
// If kernel supports FS_IOC_ADD_ENCRYPTION_KEY, also installs key of
// fscrypt-provisioning type to the global session keyring. This makes it
// possible to unmount and then remount mountpoint without losing the file-based
// key.
//
// Returns %true on success, %false on failure.  On success also sets *policy
// to the EncryptionPolicy used to refer to this key.
bool installKey(const std::string& mountpoint, const EncryptionOptions& options,
                const KeyBuffer& key, EncryptionPolicy* policy);

// Evict a file-based encryption key from the kernel.
//
// This undoes the effect of installKey().
//
// If the kernel doesn't support the filesystem-level keyring, the caller is
// responsible for dropping caches.
bool evictKey(const std::string& mountpoint, const EncryptionPolicy& policy);

bool retrieveOrGenerateKey(const std::string& key_path, const std::string& tmp_path,
                           const KeyAuthentication& key_authentication, const KeyGeneration& gen,
                           KeyBuffer* key, bool keepOld = true);

// Re-installs a file-based encryption key of fscrypt-provisioning type from the
// global session keyring back into fs keyring of the mountpoint.
bool reloadKeyFromSessionKeyring(const std::string& mountpoint, const EncryptionPolicy& policy);

#endif
