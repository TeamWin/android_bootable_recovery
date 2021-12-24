/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <array>

#include <asm/ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <cutils/properties.h>
#include <logwrap/logwrap.h>
#include <utils/misc.h>
#include <fscrypt/fscrypt.h>
#include "KeyUtil.h"

#include "fscrypt_policy.h"

static int encryption_mode = FS_ENCRYPTION_MODE_PRIVATE;

bool fscrypt_is_native() {
    LOG(ERROR) << "fscrypt_is_native::ro.crypto.type";
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.crypto.type", value, "none");
    return !strcmp(value, "file");
}

extern "C" void bytes_to_hex(const uint8_t *bytes, size_t num_bytes, char *hex) {
  for (size_t i = 0; i < num_bytes; i++) {
    sprintf(&hex[2 * i], "%02x", bytes[i]);
  }
}

static bool is_dir_empty(const char *dirname, bool *is_empty)
{
    int n = 0;
    auto dirp = std::unique_ptr<DIR, int (*)(DIR*)>(opendir(dirname), closedir);
    if (!dirp) {
        PLOG(ERROR) << "Unable to read directory: " << dirname;
        return false;
    }
    for (;;) {
        errno = 0;
        auto entry = readdir(dirp.get());
        if (!entry) {
            if (errno) {
                PLOG(ERROR) << "Unable to read directory: " << dirname;
                return false;
            }
            break;
        }
        if (strcmp(entry->d_name, "lost+found") != 0) { // Skip lost+found
            ++n;
            if (n > 2) {
                *is_empty = false;
                return true;
            }
        }
    }
    *is_empty = true;
    return true;
}

static uint8_t fscrypt_get_policy_flags(int filenames_encryption_mode) {
    if (filenames_encryption_mode == FS_ENCRYPTION_MODE_AES_256_CTS) {
        // Use legacy padding with our original filenames encryption mode.
        return FS_POLICY_FLAGS_PAD_4;
    } else if (filenames_encryption_mode == FS_ENCRYPTION_MODE_ADIANTUM) {
        // Use DIRECT_KEY for Adiantum, since it's much more efficient but just
        // as secure since Android doesn't reuse the same master key for
        // multiple encryption modes
        return (FS_POLICY_FLAGS_PAD_16 | FS_POLICY_FLAG_DIRECT_KEY);
    }
    // With a new mode we can use the better padding flag without breaking existing devices: pad
    // filenames with zeroes to the next 16-byte boundary.  This is more secure (helps hide the
    // length of filenames) and makes the inputs evenly divisible into blocks which is more
    // efficient for encryption and decryption.
    return FS_POLICY_FLAGS_PAD_16;
}

extern "C" bool fscrypt_set_mode() {
    const char* mode_file = "/data/unencrypted/mode";
    struct stat st;
    if (stat(mode_file, &st) != 0 || st.st_size <= 0) {
        printf("Invalid encryption mode file %s\n", mode_file);
        return false;
    }
    size_t mode_size = st.st_size;
    char contents_encryption_mode[mode_size + 1];
    memset((void*)contents_encryption_mode, 0, mode_size + 1);
    int fd = open(mode_file, O_RDONLY);
    if (fd < 0) {
        printf("error opening '%s': %s\n", mode_file, strerror(errno));
        return false;
    }
    if (read(fd, contents_encryption_mode, mode_size) != mode_size) {
        printf("read error on '%s': %s\n", mode_file, strerror(errno));
        close(fd);
        return false;
    }
    close(fd);

    std::string contents_encryption_mode_string = std::string(contents_encryption_mode);
    int pos = contents_encryption_mode_string.find(":");
    LOG(INFO) << "contents_encryption_mode_string: " << contents_encryption_mode_string.substr(0, pos);

    if (contents_encryption_mode_string.substr(0, pos) == "software") {
        encryption_mode = FS_ENCRYPTION_MODE_AES_256_XTS;
    } else if (contents_encryption_mode_string.substr(0, pos) == "ice") {
        encryption_mode = FS_ENCRYPTION_MODE_PRIVATE;
    } else {
        printf("Invalid encryption mode '%s'\n", contents_encryption_mode);
        return false;
    }

    printf("set encryption mode to %i\n", encryption_mode);
    return true;
}

#ifdef USE_FSCRYPT_POLICY_V1
extern "C" bool fscrypt_policy_set_struct(const char *directory, const struct fscrypt_policy_v1 *fep) {
#else
extern "C" bool fscrypt_policy_set_struct(const char *directory, const struct fscrypt_policy_v2 *fep) {
#endif
    int fd = open(directory, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd == -1) {
        printf("failed to open %s\n", directory);
        PLOG(ERROR) << "Failed to open directory " << directory;
        return false;
    }
    if (isFsKeyringSupported()) {
        if (ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, fep)) {
            PLOG(ERROR) << "Failed to set encryption policy for " << directory;
            close(fd);
            return false;
        }
    } else {
        if (ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, fep)) {
            PLOG(ERROR) << "Failed to set encryption policy for " << directory;
            close(fd);
            return false;
        }
    }
    close(fd);
    return true;
}

#ifdef USE_FSCRYPT_POLICY_V1
extern "C" bool fscrypt_policy_get_struct(const char *directory, struct fscrypt_policy_v1 *fep) {
#else
extern "C" bool fscrypt_policy_get_struct(const char *directory, struct fscrypt_policy_v2 *fep) {
#endif
    int fd = open(directory, O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open directory " << directory;
        return false;
    }
#ifdef USE_FSCRYPT_POLICY_V1
    memset(fep, 0, sizeof(fscrypt_policy_v1));
#else
    memset(fep, 0, sizeof(fscrypt_policy_v2));
#endif
    struct fscrypt_get_policy_ex_arg ex_policy = {0};

    if (isFsKeyringSupported()) {
        ex_policy.policy_size = sizeof(ex_policy.policy);
        if (ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX, &ex_policy) != 0) {
            PLOG(ERROR) << "Failed to get encryption policy for " << directory;
            close(fd);
            return false;
        }
#ifdef USE_FSCRYPT_POLICY_V1
        memcpy(fep, &ex_policy.policy.v1, sizeof(ex_policy.policy.v1));
#else
        memcpy(fep, &ex_policy.policy.v2, sizeof(ex_policy.policy.v2));
#endif
    } else {
        if (ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY, &ex_policy.policy.v1) != 0) {
            PLOG(ERROR) << "Failed to get encryption policy for " << directory;
            close(fd);
            return false;
        }
        memcpy(fep, &ex_policy.policy.v1, sizeof(ex_policy.policy.v1));
    }
    close(fd);
    return true;
}
