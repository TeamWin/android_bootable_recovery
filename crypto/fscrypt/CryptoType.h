/*
 * Copyright (C) 2020 The Android Open Source Project
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

#pragma once

#include <stdlib.h>


// Struct representing an encryption algorithm supported by vold.
// "config_name" represents the name we give the algorithm in
// read-only properties and fstab files
// "kernel_name" is the name we present to the Linux kernel
// "keysize" is the size of the key in bytes.
struct CryptoType {
    // We should only be constructing CryptoTypes as part of
    // supported_crypto_types[].  We do it via this pseudo-builder pattern,
    // which isn't pure or fully protected as a concession to being able to
    // do it all at compile time.  Add new CryptoTypes in
    // supported_crypto_types[] below.
    constexpr CryptoType() : CryptoType(nullptr, nullptr, 0xFFFFFFFF) {}
    constexpr CryptoType set_keysize(size_t size) const {
        return CryptoType(this->config_name, this->kernel_name, size);
    }
    constexpr CryptoType set_config_name(const char* property) const {
        return CryptoType(property, this->kernel_name, this->keysize);
    }
    constexpr CryptoType set_kernel_name(const char* crypto) const {
        return CryptoType(this->config_name, crypto, this->keysize);
    }

    constexpr const char* get_config_name() const { return config_name; }
    constexpr const char* get_kernel_name() const { return kernel_name; }
    constexpr size_t get_keysize() const { return keysize; }

  private:
    const char* config_name;
    const char* kernel_name;
    size_t keysize;

    constexpr CryptoType(const char* property, const char* crypto, size_t ksize)
        : config_name(property), kernel_name(crypto), keysize(ksize) {}
};

// Use the named android property to look up a type from the table
// If the property is not set or matches no table entry, return the default.
const CryptoType& lookup_crypto_algorithm(const CryptoType table[], int table_len,
                                          const CryptoType& default_alg, const char* property);

// Some useful types

constexpr CryptoType invalid_crypto_type = CryptoType();

constexpr CryptoType aes_256_xts = CryptoType()
                                           .set_config_name("aes-256-xts")
                                           .set_kernel_name("aes-xts-plain64")
                                           .set_keysize(64);

constexpr CryptoType adiantum = CryptoType()
                                        .set_config_name("adiantum")
                                        .set_kernel_name("xchacha12,aes-adiantum-plain64")
                                        .set_keysize(32);

// Support compile-time validation of a crypto type table

template <typename T, size_t N>
constexpr size_t array_length(T (&)[N]) {
    return N;
}

constexpr bool isValidCryptoType(size_t max_keylen, const CryptoType& crypto_type) {
    return ((crypto_type.get_config_name() != nullptr) &&
            (crypto_type.get_kernel_name() != nullptr) &&
            (crypto_type.get_keysize() <= max_keylen));
}

// Confirms that all supported_crypto_types have a small enough keysize and
// had both set_config_name() and set_kernel_name() called.
// Note in C++11 that constexpr functions can only have a single line.
// So our code is a bit convoluted (using recursion instead of a loop),
// but it's asserting at compile time that all of our key lengths are valid.
constexpr bool validateSupportedCryptoTypes(size_t max_keylen, const CryptoType types[],
                                            size_t len) {
    return len == 0 || (isValidCryptoType(max_keylen, types[len - 1]) &&
                        validateSupportedCryptoTypes(max_keylen, types, len - 1));
}
