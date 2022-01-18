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

#ifndef ANDROID_VOLD_KEYSTORAGE_H
#define ANDROID_VOLD_KEYSTORAGE_H

#include "KeyBuffer.h"

#include <cstdint>
#include <string>
#include <vector>

// Represents the information needed to decrypt a disk encryption key.
class KeyAuthentication {
  public:
    KeyAuthentication(const std::string& s) : secret{s} {};

    bool usesKeymaster() const { return secret.empty(); };

    const std::string secret;
};

extern const KeyAuthentication kEmptyAuthentication;

bool createSecdiscardable(const std::string& path, std::string* hash);
bool readSecdiscardable(const std::string& path, std::string* hash);

// Create a directory at the named path, and store "key" in it,
// in such a way that it can only be retrieved via Keymaster and
// can be securely deleted.
// It's safe to move/rename the directory after creation.
bool storeKey(const std::string& dir, const KeyAuthentication& auth, const KeyBuffer& key);

// Create a directory at the named path, and store "key" in it as storeKey
// This version creates the key in "tmp_path" then atomically renames "tmp_path"
// to "key_path" thereby ensuring that the key is either stored entirely or
// not at all.  All the needed files and directories are also fsync'ed to ensure
// that the key is actually persisted to disk.
bool storeKeyAtomically(const std::string& key_path, const std::string& tmp_path,
                        const KeyAuthentication& auth, const KeyBuffer& key);

// Retrieve the key from the named directory.
bool retrieveKey(const std::string& dir, const KeyAuthentication& auth, KeyBuffer* key,
                 bool keepOld);

// Securely destroy the key stored in the named directory and delete the directory.
bool destroyKey(const std::string& dir);

bool runSecdiscardSingle(const std::string& file);

// Generate wrapped storage key using keymaster. Uses STORAGE_KEY tag in keymaster.
bool generateWrappedStorageKey(KeyBuffer* key);
// Export the per-boot boot wrapped storage key using keymaster.
bool exportWrappedStorageKey(const KeyBuffer& kmKey, KeyBuffer* key);

// Set a seed to be mixed into all key storage encryption keys.
bool setKeyStorageBindingSeed(const std::vector<uint8_t>& seed);

#endif
