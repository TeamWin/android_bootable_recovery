/*
 * Copyright (C) 2010 The Android Open Source Project
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

#ifndef ANDROID_VOLD_CRYPTFS_H
#define ANDROID_VOLD_CRYPTFS_H

#include <string>

#include <linux/types.h>
#include <stdbool.h>
#include <stdint.h>

#include <cutils/properties.h>

#include "KeyBuffer.h"
#include "KeyUtil.h"

#define CRYPT_FOOTER_OFFSET 0x4000

/* Return values for cryptfs_crypto_complete */
#define CRYPTO_COMPLETE_NOT_ENCRYPTED 1
#define CRYPTO_COMPLETE_ENCRYPTED 0
#define CRYPTO_COMPLETE_BAD_METADATA (-1)
#define CRYPTO_COMPLETE_PARTIAL (-2)
#define CRYPTO_COMPLETE_INCONSISTENT (-3)
#define CRYPTO_COMPLETE_CORRUPT (-4)

/* Return values for cryptfs_getfield */
#define CRYPTO_GETFIELD_OK 0
#define CRYPTO_GETFIELD_ERROR_NO_FIELD (-1)
#define CRYPTO_GETFIELD_ERROR_OTHER (-2)
#define CRYPTO_GETFIELD_ERROR_BUF_TOO_SMALL (-3)

/* Return values for cryptfs_setfield */
#define CRYPTO_SETFIELD_OK 0
#define CRYPTO_SETFIELD_ERROR_OTHER (-1)
#define CRYPTO_SETFIELD_ERROR_FIELD_TOO_LONG (-2)
#define CRYPTO_SETFIELD_ERROR_VALUE_TOO_LONG (-3)

/* Return values for persist_del_key */
#define PERSIST_DEL_KEY_OK 0
#define PERSIST_DEL_KEY_ERROR_OTHER (-1)
#define PERSIST_DEL_KEY_ERROR_NO_FIELD (-2)

// Exposed for testing only
int match_multi_entry(const char* key, const char* field, unsigned index);

int cryptfs_crypto_complete(void);
int cryptfs_check_passwd(const char* pw);
int cryptfs_verify_passwd(const char* pw);
int cryptfs_restart(void);
int cryptfs_enable(int type, const char* passwd, int no_ui);
int cryptfs_changepw(int type, const char* newpw);
int cryptfs_enable_default(int no_ui);
int cryptfs_setup_ext_volume(const char* label, const char* real_blkdev,
                             const ::KeyBuffer& key, std::string* out_crypto_blkdev);
int cryptfs_getfield(const char* fieldname, char* value, int len);
int cryptfs_setfield(const char* fieldname, const char* value);
int cryptfs_mount_default_encrypted(void);
int cryptfs_get_password_type(void);
const char* cryptfs_get_password(void);
void cryptfs_clear_password(void);
int cryptfs_isConvertibleToFBE(void);
const KeyGeneration cryptfs_get_keygen();

#endif /* ANDROID_VOLD_CRYPTFS_H */
