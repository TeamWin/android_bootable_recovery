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

#ifndef _FS_CRYPT_H_
#define _FS_CRYPT_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <cutils/multiuser.h>
#include <linux/fs.h>

__BEGIN_DECLS

#define FS_KEY_DESCRIPTOR_SIZE_HEX (2 * FS_KEY_DESCRIPTOR_SIZE + 1)
#define FSCRYPT_KEY_IDENTIFIER_HEX_SIZE ((2 * FSCRYPT_KEY_IDENTIFIER_SIZE) + 1)

#ifdef USE_FSCRYPT_POLICY_V1
#define USER_CE_FSCRYPT_POLICY           "0CE"
#define USER_DE_FSCRYPT_POLICY           "0DE"
#define SYSTEM_DE_FSCRYPT_POLICY         "0DK"
#else
#define USER_CE_FSCRYPT_POLICY           "2CE"
#define USER_DE_FSCRYPT_POLICY           "2DE"
#define SYSTEM_DE_FSCRYPT_POLICY         "2DK"
#endif

#define FSCRYPT_V1                        "0"
#define FSCRYPT_V2                        "2"
#define SYSTEM_DE_KEY                     "DK"
#define USER_CE_KEY                       "C"
#define USER_DE_KEY                       "D"

/* modes not supported by upstream kernel, so not in <linux/fs.h> */
#define FS_ENCRYPTION_MODE_AES_256_HEH      126
#define FS_ENCRYPTION_MODE_PRIVATE          127

/* new definition, not yet in Bionic's <linux/fs.h> */
#ifndef FS_ENCRYPTION_MODE_ADIANTUM
#define FS_ENCRYPTION_MODE_ADIANTUM         9
#endif

/* new definition, not yet in Bionic's <linux/fs.h> */
#ifndef FS_POLICY_FLAG_DIRECT_KEY
#define FS_POLICY_FLAG_DIRECT_KEY           0x4
#endif

#define HEX_LOOKUP "0123456789abcdef"

bool fscrypt_set_mode();

#ifdef USE_FSCRYPT_POLICY_V1
bool lookup_ref_key(struct fscrypt_policy_v1 *fep, uint8_t* policy_type);
#else
bool lookup_ref_key(struct fscrypt_policy_v2 *fep, uint8_t* policy_type);
#endif

bool lookup_ref_tar(const uint8_t *policy_type, uint8_t *policy);

#ifdef USE_FSCRYPT_POLICY_V1
bool fscrypt_policy_get_struct(const char *directory, struct fscrypt_policy_v1  *fep);
#else
bool fscrypt_policy_get_struct(const char *directory, struct fscrypt_policy_v2  *fep);
#endif

#ifdef USE_FSCRYPT_POLICY_V1
bool fscrypt_policy_set_struct(const char *directory, const struct fscrypt_policy_v1  *fep);
#else
bool fscrypt_policy_set_struct(const char *directory, const struct fscrypt_policy_v2  *fep);
#endif

void bytes_to_hex(const uint8_t *bytes, size_t num_bytes, char *hex);
__END_DECLS

#endif // _FS_CRYPT_H_
