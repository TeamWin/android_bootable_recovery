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

#define LOG_TAG "Cryptfs"

#include "cryptfs.h"

#include "Checkpoint.h"
#include "CryptoType.h"
#include "EncryptInplace.h"
#include "FsCrypt.h"
#include "Keymaster.h"
#include "Process.h"
#include "ScryptParameters.h"
#include "Utils.h"
#include "VoldUtil.h"
#include "VolumeManager.h"

#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <bootloader_message/bootloader_message.h>
#include <cutils/android_reboot.h>
#include <cutils/properties.h>
#include <ext4_utils/ext4_utils.h>
#include <f2fs_sparseblock.h>
#include <fs_mgr.h>
#include <fscrypt/fscrypt.h>
#include <libdm/dm.h>
#include <log/log.h>
#include <logwrap/logwrap.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <selinux/selinux.h>
// #include <wakelock/wakelock.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <linux/kdev_t.h>
#include <math.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <thread>

extern "C" {
#include <crypto_scrypt.h>
}

using android::base::ParseUint;
using android::base::StringPrintf;
using android::fs_mgr::GetEntryForMountPoint;
using ::CryptoType;
using ::KeyBuffer;
using ::KeyGeneration;
using namespace android::dm;
using namespace std::chrono_literals;

/* The current cryptfs version */
#define CURRENT_MAJOR_VERSION 1
#define CURRENT_MINOR_VERSION 3

#define CRYPT_FOOTER_TO_PERSIST_OFFSET 0x1000
#define CRYPT_PERSIST_DATA_SIZE 0x1000

#define MAX_CRYPTO_TYPE_NAME_LEN 64

#define MAX_KEY_LEN 48
#define SALT_LEN 16
#define SCRYPT_LEN 32

/* definitions of flags in the structure below */
#define CRYPT_MNT_KEY_UNENCRYPTED 0x1 /* The key for the partition is not encrypted. */
#define CRYPT_ENCRYPTION_IN_PROGRESS       \
    0x2 /* Encryption partially completed, \
           encrypted_upto valid*/
#define CRYPT_INCONSISTENT_STATE                    \
    0x4 /* Set when starting encryption, clear when \
           exit cleanly, either through success or  \
           correctly marked partial encryption */
#define CRYPT_DATA_CORRUPT                      \
    0x8 /* Set when encryption is fine, but the \
           underlying volume is corrupt */
#define CRYPT_FORCE_ENCRYPTION                        \
    0x10 /* Set when it is time to encrypt this       \
            volume on boot. Everything in this        \
            structure is set up correctly as          \
            though device is encrypted except         \
            that the master key is encrypted with the \
            default password. */
#define CRYPT_FORCE_COMPLETE                           \
    0x20 /* Set when the above encryption cycle is     \
            complete. On next cryptkeeper entry, match \
            the password. If it matches fix the master \
            key and remove this flag. */

/* Allowed values for type in the structure below */
#define CRYPT_TYPE_PASSWORD                       \
    0 /* master_key is encrypted with a password  \
       * Must be zero to be compatible with pre-L \
       * devices where type is always password.*/
#define CRYPT_TYPE_DEFAULT                                            \
    1                         /* master_key is encrypted with default \
                               * password */
#define CRYPT_TYPE_PATTERN 2  /* master_key is encrypted with a pattern */
#define CRYPT_TYPE_PIN 3      /* master_key is encrypted with a pin */
#define CRYPT_TYPE_MAX_TYPE 3 /* type cannot be larger than this value */

#define CRYPT_MNT_MAGIC 0xD0B5B1C4
#define PERSIST_DATA_MAGIC 0xE950CD44

/* Key Derivation Function algorithms */
#define KDF_PBKDF2 1
#define KDF_SCRYPT 2
/* Algorithms 3 & 4 deprecated before shipping outside of google, so removed */
#define KDF_SCRYPT_KEYMASTER 5

/* Maximum allowed keymaster blob size. */
#define KEYMASTER_BLOB_SIZE 2048

/* __le32 and __le16 defined in system/extras/ext4_utils/ext4_utils.h */
#define __le8 unsigned char

#if !defined(SHA256_DIGEST_LENGTH)
#define SHA256_DIGEST_LENGTH 32
#endif

/* This structure starts 16,384 bytes before the end of a hardware
 * partition that is encrypted, or in a separate partition.  It's location
 * is specified by a property set in init.<device>.rc.
 * The structure allocates 48 bytes for a key, but the real key size is
 * specified in the struct.  Currently, the code is hardcoded to use 128
 * bit keys.
 * The fields after salt are only valid in rev 1.1 and later stuctures.
 * Obviously, the filesystem does not include the last 16 kbytes
 * of the partition if the crypt_mnt_ftr lives at the end of the
 * partition.
 */

struct crypt_mnt_ftr {
    __le32 magic; /* See above */
    __le16 major_version;
    __le16 minor_version;
    __le32 ftr_size;             /* in bytes, not including key following */
    __le32 flags;                /* See above */
    __le32 keysize;              /* in bytes */
    __le32 crypt_type;           /* how master_key is encrypted. Must be a
                                  * CRYPT_TYPE_XXX value */
    __le64 fs_size;              /* Size of the encrypted fs, in 512 byte sectors */
    __le32 failed_decrypt_count; /* count of # of failed attempts to decrypt and
                                    mount, set to 0 on successful mount */
    unsigned char crypto_type_name[MAX_CRYPTO_TYPE_NAME_LEN]; /* The type of encryption
                                                                 needed to decrypt this
                                                                 partition, null terminated */
    __le32 spare2;                                            /* ignored */
    unsigned char master_key[MAX_KEY_LEN]; /* The encrypted key for decrypting the filesystem */
    unsigned char salt[SALT_LEN];          /* The salt used for this encryption */
    __le64 persist_data_offset[2];         /* Absolute offset to both copies of crypt_persist_data
                                            * on device with that info, either the footer of the
                                            * real_blkdevice or the metadata partition. */

    __le32 persist_data_size; /* The number of bytes allocated to each copy of the
                               * persistent data table*/

    __le8 kdf_type; /* The key derivation function used. */

    /* scrypt parameters. See www.tarsnap.com/scrypt/scrypt.pdf */
    __le8 N_factor;        /* (1 << N) */
    __le8 r_factor;        /* (1 << r) */
    __le8 p_factor;        /* (1 << p) */
    __le64 encrypted_upto; /* If we are in state CRYPT_ENCRYPTION_IN_PROGRESS and
                              we have to stop (e.g. power low) this is the last
                              encrypted 512 byte sector.*/
    __le8 hash_first_block[SHA256_DIGEST_LENGTH]; /* When CRYPT_ENCRYPTION_IN_PROGRESS
                                                     set, hash of first block, used
                                                     to validate before continuing*/

    /* key_master key, used to sign the derived key which is then used to generate
     * the intermediate key
     * This key should be used for no other purposes! We use this key to sign unpadded
     * data, which is acceptable but only if the key is not reused elsewhere. */
    __le8 keymaster_blob[KEYMASTER_BLOB_SIZE];
    __le32 keymaster_blob_size;

    /* Store scrypt of salted intermediate key. When decryption fails, we can
       check if this matches, and if it does, we know that the problem is with the
       drive, and there is no point in asking the user for more passwords.

       Note that if any part of this structure is corrupt, this will not match and
       we will continue to believe the user entered the wrong password. In that
       case the only solution is for the user to enter a password enough times to
       force a wipe.

       Note also that there is no need to worry about migration. If this data is
       wrong, we simply won't recognise a right password, and will continue to
       prompt. On the first password change, this value will be populated and
       then we will be OK.
     */
    unsigned char scrypted_intermediate_key[SCRYPT_LEN];

    /* sha of this structure with this element set to zero
       Used when encrypting on reboot to validate structure before doing something
       fatal
     */
    unsigned char sha256[SHA256_DIGEST_LENGTH];
};

/* Persistant data that should be available before decryption.
 * Things like airplane mode, locale and timezone are kept
 * here and can be retrieved by the CryptKeeper UI to properly
 * configure the phone before asking for the password
 * This is only valid if the major and minor version above
 * is set to 1.1 or higher.
 *
 * This is a 4K structure.  There are 2 copies, and the code alternates
 * writing one and then clearing the previous one.  The reading
 * code reads the first valid copy it finds, based on the magic number.
 * The absolute offset to the first of the two copies is kept in rev 1.1
 * and higher crypt_mnt_ftr structures.
 */
struct crypt_persist_entry {
    char key[PROPERTY_KEY_MAX];
    char val[PROPERTY_VALUE_MAX];
};

/* Should be exactly 4K in size */
struct crypt_persist_data {
    __le32 persist_magic;
    __le32 persist_valid_entries;
    __le32 persist_spare[30];
    struct crypt_persist_entry persist_entry[0];
};

static int wait_and_unmount(const char* mountpoint, bool kill);

typedef int (*kdf_func)(const char* passwd, const unsigned char* salt, unsigned char* ikey,
                        void* params);

#define UNUSED __attribute__((unused))

#define HASH_COUNT 2000

constexpr size_t INTERMEDIATE_KEY_LEN_BYTES = 16;
constexpr size_t INTERMEDIATE_IV_LEN_BYTES = 16;
constexpr size_t INTERMEDIATE_BUF_SIZE = (INTERMEDIATE_KEY_LEN_BYTES + INTERMEDIATE_IV_LEN_BYTES);

// SCRYPT_LEN is used by struct crypt_mnt_ftr for its intermediate key.
static_assert(INTERMEDIATE_BUF_SIZE == SCRYPT_LEN, "Mismatch of intermediate key sizes");

#define KEY_IN_FOOTER "footer"

#define DEFAULT_PASSWORD "default_password"

#define CRYPTO_BLOCK_DEVICE "userdata"

#define BREADCRUMB_FILE "/data/misc/vold/convert_fde"

#define EXT4_FS 1
#define F2FS_FS 2

#define TABLE_LOAD_RETRIES 10

#define RSA_KEY_SIZE 2048
#define RSA_KEY_SIZE_BYTES (RSA_KEY_SIZE / 8)
#define RSA_EXPONENT 0x10001
#define KEYMASTER_CRYPTFS_RATE_LIMIT 1  // Maximum one try per second

#define RETRY_MOUNT_ATTEMPTS 10
#define RETRY_MOUNT_DELAY_SECONDS 1

#define CREATE_CRYPTO_BLK_DEV_FLAGS_ALLOW_ENCRYPT_OVERRIDE (1)

static int put_crypt_ftr_and_key(struct crypt_mnt_ftr* crypt_ftr);

static unsigned char saved_master_key[MAX_KEY_LEN];
static char* saved_mount_point;
static int master_key_saved = 0;
static struct crypt_persist_data* persist_data = NULL;

constexpr CryptoType aes_128_cbc = CryptoType()
                                           .set_config_name("AES-128-CBC")
                                           .set_kernel_name("aes-cbc-essiv:sha256")
                                           .set_keysize(16);

constexpr CryptoType supported_crypto_types[] = {aes_128_cbc, ::adiantum};

static_assert(validateSupportedCryptoTypes(MAX_KEY_LEN, supported_crypto_types,
                                           array_length(supported_crypto_types)),
              "We have a CryptoType with keysize > MAX_KEY_LEN or which was "
              "incompletely constructed.");

static const CryptoType& get_crypto_type() {
    // We only want to parse this read-only property once.  But we need to wait
    // until the system is initialized before we can read it.  So we use a static
    // scoped within this function to get it only once.
    static CryptoType crypto_type =
            lookup_crypto_algorithm(supported_crypto_types, array_length(supported_crypto_types),
                                    aes_128_cbc, "ro.crypto.fde_algorithm");
    return crypto_type;
}

const KeyGeneration cryptfs_get_keygen() {
    return KeyGeneration{get_crypto_type().get_keysize(), true, false};
}

/* Should we use keymaster? */
static int keymaster_check_compatibility() {
    return keymaster_compatibility_cryptfs_scrypt();
}

/* Create a new keymaster key and store it in this footer */
static int keymaster_create_key(struct crypt_mnt_ftr* ftr) {
    if (ftr->keymaster_blob_size) {
        SLOGI("Already have key");
        return 0;
    }

    int rc = keymaster_create_key_for_cryptfs_scrypt(
        RSA_KEY_SIZE, RSA_EXPONENT, KEYMASTER_CRYPTFS_RATE_LIMIT, ftr->keymaster_blob,
        KEYMASTER_BLOB_SIZE, &ftr->keymaster_blob_size);
    if (rc) {
        if (ftr->keymaster_blob_size > KEYMASTER_BLOB_SIZE) {
            SLOGE("Keymaster key blob too large");
            ftr->keymaster_blob_size = 0;
        }
        SLOGE("Failed to generate keypair");
        return -1;
    }
    return 0;
}

/* This signs the given object using the keymaster key. */
static int keymaster_sign_object(struct crypt_mnt_ftr* ftr, const unsigned char* object,
                                 const size_t object_size, unsigned char** signature,
                                 size_t* signature_size) {
    unsigned char to_sign[RSA_KEY_SIZE_BYTES];
    size_t to_sign_size = sizeof(to_sign);
    memset(to_sign, 0, RSA_KEY_SIZE_BYTES);

    // To sign a message with RSA, the message must satisfy two
    // constraints:
    //
    // 1. The message, when interpreted as a big-endian numeric value, must
    //    be strictly less than the public modulus of the RSA key.  Note
    //    that because the most significant bit of the public modulus is
    //    guaranteed to be 1 (else it's an (n-1)-bit key, not an n-bit
    //    key), an n-bit message with most significant bit 0 always
    //    satisfies this requirement.
    //
    // 2. The message must have the same length in bits as the public
    //    modulus of the RSA key.  This requirement isn't mathematically
    //    necessary, but is necessary to ensure consistency in
    //    implementations.
    switch (ftr->kdf_type) {
        case KDF_SCRYPT_KEYMASTER:
            // This ensures the most significant byte of the signed message
            // is zero.  We could have zero-padded to the left instead, but
            // this approach is slightly more robust against changes in
            // object size.  However, it's still broken (but not unusably
            // so) because we really should be using a proper deterministic
            // RSA padding function, such as PKCS1.
            memcpy(to_sign + 1, object, std::min((size_t)RSA_KEY_SIZE_BYTES - 1, object_size));
            SLOGI("Signing safely-padded object");
            break;
        default:
            SLOGE("Unknown KDF type %d", ftr->kdf_type);
            return -1;
    }
    for (;;) {
        auto result = keymaster_sign_object_for_cryptfs_scrypt(
            ftr->keymaster_blob, ftr->keymaster_blob_size, KEYMASTER_CRYPTFS_RATE_LIMIT, to_sign,
            to_sign_size, signature, signature_size);
        switch (result) {
            case KeymasterSignResult::ok:
                return 0;
            case KeymasterSignResult::upgrade:
                break;
            default:
                return -1;
        }
        SLOGD("Upgrading key");
        if (keymaster_upgrade_key_for_cryptfs_scrypt(
                RSA_KEY_SIZE, RSA_EXPONENT, KEYMASTER_CRYPTFS_RATE_LIMIT, ftr->keymaster_blob,
                ftr->keymaster_blob_size, ftr->keymaster_blob, KEYMASTER_BLOB_SIZE,
                &ftr->keymaster_blob_size) != 0) {
            SLOGE("Failed to upgrade key");
            return -1;
        }
        if (put_crypt_ftr_and_key(ftr) != 0) {
            SLOGE("Failed to write upgraded key to disk");
        }
        SLOGD("Key upgraded successfully");
    }
}

/* Store password when userdata is successfully decrypted and mounted.
 * Cleared by cryptfs_clear_password
 *
 * To avoid a double prompt at boot, we need to store the CryptKeeper
 * password and pass it to KeyGuard, which uses it to unlock KeyStore.
 * Since the entire framework is torn down and rebuilt after encryption,
 * we have to use a daemon or similar to store the password. Since vold
 * is secured against IPC except from system processes, it seems a reasonable
 * place to store this.
 *
 * password should be cleared once it has been used.
 *
 * password is aged out after password_max_age_seconds seconds.
 */
static char* password = 0;
static int password_expiry_time = 0;
static const int password_max_age_seconds = 60;

enum class RebootType { reboot, recovery, shutdown };
static void cryptfs_reboot(RebootType rt) {
    switch (rt) {
        case RebootType::reboot:
            property_set(ANDROID_RB_PROPERTY, "reboot");
            break;

        case RebootType::recovery:
            property_set(ANDROID_RB_PROPERTY, "reboot,recovery");
            break;

        case RebootType::shutdown:
            property_set(ANDROID_RB_PROPERTY, "shutdown");
            break;
    }

    sleep(20);

    /* Shouldn't get here, reboot should happen before sleep times out */
    return;
}

/**
 * Gets the default device scrypt parameters for key derivation time tuning.
 * The parameters should lead to about one second derivation time for the
 * given device.
 */
static void get_device_scrypt_params(struct crypt_mnt_ftr* ftr) {
    char paramstr[PROPERTY_VALUE_MAX];
    int Nf, rf, pf;

    property_get(SCRYPT_PROP, paramstr, SCRYPT_DEFAULTS);
    if (!parse_scrypt_parameters(paramstr, &Nf, &rf, &pf)) {
        SLOGW("bad scrypt parameters '%s' should be like '12:8:1'; using defaults", paramstr);
        parse_scrypt_parameters(SCRYPT_DEFAULTS, &Nf, &rf, &pf);
    }
    ftr->N_factor = Nf;
    ftr->r_factor = rf;
    ftr->p_factor = pf;
}

static uint64_t get_fs_size(const char* dev) {
    int fd, block_size;
    struct ext4_super_block sb;
    uint64_t len;

    if ((fd = open(dev, O_RDONLY | O_CLOEXEC)) < 0) {
        SLOGE("Cannot open device to get filesystem size ");
        return 0;
    }

    if (lseek64(fd, 1024, SEEK_SET) < 0) {
        SLOGE("Cannot seek to superblock");
        return 0;
    }

    if (read(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        SLOGE("Cannot read superblock");
        return 0;
    }

    close(fd);

    if (le32_to_cpu(sb.s_magic) != EXT4_SUPER_MAGIC) {
        SLOGE("Not a valid ext4 superblock");
        return 0;
    }
    block_size = 1024 << sb.s_log_block_size;
    /* compute length in bytes */
    len = (((uint64_t)sb.s_blocks_count_hi << 32) + sb.s_blocks_count_lo) * block_size;

    /* return length in sectors */
    return len / 512;
}

static void get_crypt_info(std::string* key_loc, std::string* real_blk_device) {
    for (const auto& entry : fstab_default) {
        if (!entry.fs_mgr_flags.vold_managed &&
            (entry.fs_mgr_flags.crypt || entry.fs_mgr_flags.force_crypt ||
             entry.fs_mgr_flags.force_fde_or_fbe || entry.fs_mgr_flags.file_encryption)) {
            if (key_loc != nullptr) {
                *key_loc = entry.key_loc;
            }
            if (real_blk_device != nullptr) {
                *real_blk_device = entry.blk_device;
            }
            return;
        }
    }
}

static int get_crypt_ftr_info(char** metadata_fname, off64_t* off) {
    static int cached_data = 0;
    static uint64_t cached_off = 0;
    static char cached_metadata_fname[PROPERTY_VALUE_MAX] = "";
    char key_loc[PROPERTY_VALUE_MAX];
    char real_blkdev[PROPERTY_VALUE_MAX];
    int rc = -1;

    if (!cached_data) {
        std::string key_loc;
        std::string real_blkdev;
        get_crypt_info(&key_loc, &real_blkdev);

        if (key_loc == KEY_IN_FOOTER) {
            if (::GetBlockDevSize(real_blkdev, &cached_off) == android::OK) {
                /* If it's an encrypted Android partition, the last 16 Kbytes contain the
                 * encryption info footer and key, and plenty of bytes to spare for future
                 * growth.
                 */
                strlcpy(cached_metadata_fname, real_blkdev.c_str(), sizeof(cached_metadata_fname));
                cached_off -= CRYPT_FOOTER_OFFSET;
                cached_data = 1;
            } else {
                SLOGE("Cannot get size of block device %s\n", real_blkdev.c_str());
            }
        } else {
            strlcpy(cached_metadata_fname, key_loc.c_str(), sizeof(cached_metadata_fname));
            cached_off = 0;
            cached_data = 1;
        }
    }

    if (cached_data) {
        if (metadata_fname) {
            *metadata_fname = cached_metadata_fname;
        }
        if (off) {
            *off = cached_off;
        }
        rc = 0;
    }

    return rc;
}

/* Set sha256 checksum in structure */
static void set_ftr_sha(struct crypt_mnt_ftr* crypt_ftr) {
    SHA256_CTX c;
    SHA256_Init(&c);
    memset(crypt_ftr->sha256, 0, sizeof(crypt_ftr->sha256));
    SHA256_Update(&c, crypt_ftr, sizeof(*crypt_ftr));
    SHA256_Final(crypt_ftr->sha256, &c);
}

/* key or salt can be NULL, in which case just skip writing that value.  Useful to
 * update the failed mount count but not change the key.
 */
static int put_crypt_ftr_and_key(struct crypt_mnt_ftr* crypt_ftr) {
    int fd;
    unsigned int cnt;
    /* starting_off is set to the SEEK_SET offset
     * where the crypto structure starts
     */
    off64_t starting_off;
    int rc = -1;
    char* fname = NULL;
    struct stat statbuf;

    set_ftr_sha(crypt_ftr);

    if (get_crypt_ftr_info(&fname, &starting_off)) {
        SLOGE("Unable to get crypt_ftr_info\n");
        return -1;
    }
    if (fname[0] != '/') {
        SLOGE("put_crypt_ftr_and_key::Unexpected value for crypto key location: %s\n", fname);
        return -1;
    }
    if ((fd = open(fname, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) < 0) {
        SLOGE("Cannot open footer file %s for put\n", fname);
        return -1;
    }

    /* Seek to the start of the crypt footer */
    if (lseek64(fd, starting_off, SEEK_SET) == -1) {
        SLOGE("Cannot seek to real block device footer\n");
        goto errout;
    }

    if ((cnt = write(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr))) != sizeof(struct crypt_mnt_ftr)) {
        SLOGE("Cannot write real block device footer\n");
        goto errout;
    }

    fstat(fd, &statbuf);
    /* If the keys are kept on a raw block device, do not try to truncate it. */
    if (S_ISREG(statbuf.st_mode)) {
        if (ftruncate(fd, 0x4000)) {
            SLOGE("Cannot set footer file size\n");
            goto errout;
        }
    }

    /* Success! */
    rc = 0;

errout:
    close(fd);
    return rc;
}

static bool check_ftr_sha(const struct crypt_mnt_ftr* crypt_ftr) {
    struct crypt_mnt_ftr copy;
    memcpy(&copy, crypt_ftr, sizeof(copy));
    set_ftr_sha(&copy);
    return memcmp(copy.sha256, crypt_ftr->sha256, sizeof(copy.sha256)) == 0;
}

static inline int unix_read(int fd, void* buff, int len) {
    return TEMP_FAILURE_RETRY(read(fd, buff, len));
}

static inline int unix_write(int fd, const void* buff, int len) {
    return TEMP_FAILURE_RETRY(write(fd, buff, len));
}

static void init_empty_persist_data(struct crypt_persist_data* pdata, int len) {
    memset(pdata, 0, len);
    pdata->persist_magic = PERSIST_DATA_MAGIC;
    pdata->persist_valid_entries = 0;
}

/* A routine to update the passed in crypt_ftr to the lastest version.
 * fd is open read/write on the device that holds the crypto footer and persistent
 * data, crypt_ftr is a pointer to the struct to be updated, and offset is the
 * absolute offset to the start of the crypt_mnt_ftr on the passed in fd.
 */
static void upgrade_crypt_ftr(int fd, struct crypt_mnt_ftr* crypt_ftr, off64_t offset) {
    int orig_major = crypt_ftr->major_version;
    int orig_minor = crypt_ftr->minor_version;

    if ((crypt_ftr->major_version == 1) && (crypt_ftr->minor_version == 0)) {
        struct crypt_persist_data* pdata;
        off64_t pdata_offset = offset + CRYPT_FOOTER_TO_PERSIST_OFFSET;

        SLOGW("upgrading crypto footer to 1.1");

        pdata = (crypt_persist_data*)malloc(CRYPT_PERSIST_DATA_SIZE);
        if (pdata == NULL) {
            SLOGE("Cannot allocate persisent data\n");
            return;
        }
        memset(pdata, 0, CRYPT_PERSIST_DATA_SIZE);

        /* Need to initialize the persistent data area */
        if (lseek64(fd, pdata_offset, SEEK_SET) == -1) {
            SLOGE("Cannot seek to persisent data offset\n");
            free(pdata);
            return;
        }
        /* Write all zeros to the first copy, making it invalid */
        unix_write(fd, pdata, CRYPT_PERSIST_DATA_SIZE);

        /* Write a valid but empty structure to the second copy */
        init_empty_persist_data(pdata, CRYPT_PERSIST_DATA_SIZE);
        unix_write(fd, pdata, CRYPT_PERSIST_DATA_SIZE);

        /* Update the footer */
        crypt_ftr->persist_data_size = CRYPT_PERSIST_DATA_SIZE;
        crypt_ftr->persist_data_offset[0] = pdata_offset;
        crypt_ftr->persist_data_offset[1] = pdata_offset + CRYPT_PERSIST_DATA_SIZE;
        crypt_ftr->minor_version = 1;
        free(pdata);
    }

    if ((crypt_ftr->major_version == 1) && (crypt_ftr->minor_version == 1)) {
        SLOGW("upgrading crypto footer to 1.2");
        /* But keep the old kdf_type.
         * It will get updated later to KDF_SCRYPT after the password has been verified.
         */
        crypt_ftr->kdf_type = KDF_PBKDF2;
        get_device_scrypt_params(crypt_ftr);
        crypt_ftr->minor_version = 2;
    }

    if ((crypt_ftr->major_version == 1) && (crypt_ftr->minor_version == 2)) {
        SLOGW("upgrading crypto footer to 1.3");
        crypt_ftr->crypt_type = CRYPT_TYPE_PASSWORD;
        crypt_ftr->minor_version = 3;
    }

    if ((orig_major != crypt_ftr->major_version) || (orig_minor != crypt_ftr->minor_version)) {
        if (lseek64(fd, offset, SEEK_SET) == -1) {
            SLOGE("Cannot seek to crypt footer\n");
            return;
        }
        unix_write(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr));
    }
}

static int get_crypt_ftr_and_key(struct crypt_mnt_ftr* crypt_ftr) {
    int fd;
    unsigned int cnt;
    off64_t starting_off;
    int rc = -1;
    char* fname = NULL;
    struct stat statbuf;

    if (get_crypt_ftr_info(&fname, &starting_off)) {
        SLOGE("Unable to get crypt_ftr_info\n");
        return -1;
    }
    if (fname[0] != '/') {
        SLOGE("get_crypt_ftr_and_key::Unexpected value for crypto key location: %s\n", fname);
        return -1;
    }
    if ((fd = open(fname, O_RDWR | O_CLOEXEC)) < 0) {
        SLOGE("Cannot open footer file %s for get\n", fname);
        return -1;
    }

    /* Make sure it's 16 Kbytes in length */
    fstat(fd, &statbuf);
    if (S_ISREG(statbuf.st_mode) && (statbuf.st_size != 0x4000)) {
        SLOGE("footer file %s is not the expected size!\n", fname);
        goto errout;
    }

    /* Seek to the start of the crypt footer */
    if (lseek64(fd, starting_off, SEEK_SET) == -1) {
        SLOGE("Cannot seek to real block device footer\n");
        goto errout;
    }

    if ((cnt = read(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr))) != sizeof(struct crypt_mnt_ftr)) {
        SLOGE("Cannot read real block device footer\n");
        goto errout;
    }

    if (crypt_ftr->magic != CRYPT_MNT_MAGIC) {
        SLOGE("Bad magic for real block device %s\n", fname);
        goto errout;
    }

    if (crypt_ftr->major_version != CURRENT_MAJOR_VERSION) {
        SLOGE("Cannot understand major version %d real block device footer; expected %d\n",
              crypt_ftr->major_version, CURRENT_MAJOR_VERSION);
        goto errout;
    }

    // We risk buffer overflows with oversized keys, so we just reject them.
    // 0-sized keys are problematic (essentially by-passing encryption), and
    // AES-CBC key wrapping only works for multiples of 16 bytes.
    if ((crypt_ftr->keysize == 0) || ((crypt_ftr->keysize % 16) != 0) ||
        (crypt_ftr->keysize > MAX_KEY_LEN)) {
        SLOGE(
            "Invalid keysize (%u) for block device %s; Must be non-zero, "
            "divisible by 16, and <= %d\n",
            crypt_ftr->keysize, fname, MAX_KEY_LEN);
        goto errout;
    }

    if (crypt_ftr->minor_version > CURRENT_MINOR_VERSION) {
        SLOGW("Warning: crypto footer minor version %d, expected <= %d, continuing...\n",
              crypt_ftr->minor_version, CURRENT_MINOR_VERSION);
    }

    /* If this is a verion 1.0 crypt_ftr, make it a 1.1 crypt footer, and update the
     * copy on disk before returning.
     */
    if (crypt_ftr->minor_version < CURRENT_MINOR_VERSION) {
        upgrade_crypt_ftr(fd, crypt_ftr, starting_off);
    }

    /* Success! */
    rc = 0;

errout:
    close(fd);
    return rc;
}

static int validate_persistent_data_storage(struct crypt_mnt_ftr* crypt_ftr) {
    if (crypt_ftr->persist_data_offset[0] + crypt_ftr->persist_data_size >
        crypt_ftr->persist_data_offset[1]) {
        SLOGE("Crypt_ftr persist data regions overlap");
        return -1;
    }

    if (crypt_ftr->persist_data_offset[0] >= crypt_ftr->persist_data_offset[1]) {
        SLOGE("Crypt_ftr persist data region 0 starts after region 1");
        return -1;
    }

    if (((crypt_ftr->persist_data_offset[1] + crypt_ftr->persist_data_size) -
         (crypt_ftr->persist_data_offset[0] - CRYPT_FOOTER_TO_PERSIST_OFFSET)) >
        CRYPT_FOOTER_OFFSET) {
        SLOGE("Persistent data extends past crypto footer");
        return -1;
    }

    return 0;
}

static int load_persistent_data(void) {
    struct crypt_mnt_ftr crypt_ftr;
    struct crypt_persist_data* pdata = NULL;
    char encrypted_state[PROPERTY_VALUE_MAX];
    char* fname;
    int found = 0;
    int fd;
    int ret;
    int i;

    if (persist_data) {
        /* Nothing to do, we've already loaded or initialized it */
        return 0;
    }

    /* If not encrypted, just allocate an empty table and initialize it */
    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "encrypted")) {
        pdata = (crypt_persist_data*)malloc(CRYPT_PERSIST_DATA_SIZE);
        if (pdata) {
            init_empty_persist_data(pdata, CRYPT_PERSIST_DATA_SIZE);
            persist_data = pdata;
            return 0;
        }
        return -1;
    }

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        return -1;
    }

    if ((crypt_ftr.major_version < 1) ||
        (crypt_ftr.major_version == 1 && crypt_ftr.minor_version < 1)) {
        SLOGE("Crypt_ftr version doesn't support persistent data");
        return -1;
    }

    if (get_crypt_ftr_info(&fname, NULL)) {
        return -1;
    }

    ret = validate_persistent_data_storage(&crypt_ftr);
    if (ret) {
        return -1;
    }

    fd = open(fname, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        SLOGE("Cannot open %s metadata file", fname);
        return -1;
    }

    pdata = (crypt_persist_data*)malloc(crypt_ftr.persist_data_size);
    if (pdata == NULL) {
        SLOGE("Cannot allocate memory for persistent data");
        goto err;
    }

    for (i = 0; i < 2; i++) {
        if (lseek64(fd, crypt_ftr.persist_data_offset[i], SEEK_SET) < 0) {
            SLOGE("Cannot seek to read persistent data on %s", fname);
            goto err2;
        }
        if (unix_read(fd, pdata, crypt_ftr.persist_data_size) < 0) {
            SLOGE("Error reading persistent data on iteration %d", i);
            goto err2;
        }
        if (pdata->persist_magic == PERSIST_DATA_MAGIC) {
            found = 1;
            break;
        }
    }

    if (!found) {
        SLOGI("Could not find valid persistent data, creating");
        init_empty_persist_data(pdata, crypt_ftr.persist_data_size);
    }

    /* Success */
    persist_data = pdata;
    close(fd);
    return 0;

err2:
    free(pdata);

err:
    close(fd);
    return -1;
}

static int save_persistent_data(void) {
    struct crypt_mnt_ftr crypt_ftr;
    struct crypt_persist_data* pdata;
    char* fname;
    off64_t write_offset;
    off64_t erase_offset;
    int fd;
    int ret;

    if (persist_data == NULL) {
        SLOGE("No persistent data to save");
        return -1;
    }

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        return -1;
    }

    if ((crypt_ftr.major_version < 1) ||
        (crypt_ftr.major_version == 1 && crypt_ftr.minor_version < 1)) {
        SLOGE("Crypt_ftr version doesn't support persistent data");
        return -1;
    }

    ret = validate_persistent_data_storage(&crypt_ftr);
    if (ret) {
        return -1;
    }

    if (get_crypt_ftr_info(&fname, NULL)) {
        return -1;
    }

    fd = open(fname, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        SLOGE("Cannot open %s metadata file", fname);
        return -1;
    }

    pdata = (crypt_persist_data*)malloc(crypt_ftr.persist_data_size);
    if (pdata == NULL) {
        SLOGE("Cannot allocate persistant data");
        goto err;
    }

    if (lseek64(fd, crypt_ftr.persist_data_offset[0], SEEK_SET) < 0) {
        SLOGE("Cannot seek to read persistent data on %s", fname);
        goto err2;
    }

    if (unix_read(fd, pdata, crypt_ftr.persist_data_size) < 0) {
        SLOGE("Error reading persistent data before save");
        goto err2;
    }

    if (pdata->persist_magic == PERSIST_DATA_MAGIC) {
        /* The first copy is the curent valid copy, so write to
         * the second copy and erase this one */
        write_offset = crypt_ftr.persist_data_offset[1];
        erase_offset = crypt_ftr.persist_data_offset[0];
    } else {
        /* The second copy must be the valid copy, so write to
         * the first copy, and erase the second */
        write_offset = crypt_ftr.persist_data_offset[0];
        erase_offset = crypt_ftr.persist_data_offset[1];
    }

    /* Write the new copy first, if successful, then erase the old copy */
    if (lseek64(fd, write_offset, SEEK_SET) < 0) {
        SLOGE("Cannot seek to write persistent data");
        goto err2;
    }
    if (unix_write(fd, persist_data, crypt_ftr.persist_data_size) ==
        (int)crypt_ftr.persist_data_size) {
        if (lseek64(fd, erase_offset, SEEK_SET) < 0) {
            SLOGE("Cannot seek to erase previous persistent data");
            goto err2;
        }
        fsync(fd);
        memset(pdata, 0, crypt_ftr.persist_data_size);
        if (unix_write(fd, pdata, crypt_ftr.persist_data_size) != (int)crypt_ftr.persist_data_size) {
            SLOGE("Cannot write to erase previous persistent data");
            goto err2;
        }
        fsync(fd);
    } else {
        SLOGE("Cannot write to save persistent data");
        goto err2;
    }

    /* Success */
    free(pdata);
    close(fd);
    return 0;

err2:
    free(pdata);
err:
    close(fd);
    return -1;
}

/* Convert a binary key of specified length into an ascii hex string equivalent,
 * without the leading 0x and with null termination
 */
static void convert_key_to_hex_ascii(const unsigned char* master_key, unsigned int keysize,
                                     char* master_key_ascii) {
    unsigned int i, a;
    unsigned char nibble;

    for (i = 0, a = 0; i < keysize; i++, a += 2) {
        /* For each byte, write out two ascii hex digits */
        nibble = (master_key[i] >> 4) & 0xf;
        master_key_ascii[a] = nibble + (nibble > 9 ? 0x37 : 0x30);

        nibble = master_key[i] & 0xf;
        master_key_ascii[a + 1] = nibble + (nibble > 9 ? 0x37 : 0x30);
    }

    /* Add the null termination */
    master_key_ascii[a] = '\0';
}

/*
 * If the ro.crypto.fde_sector_size system property is set, append the
 * parameters to make dm-crypt use the specified crypto sector size and round
 * the crypto device size down to a crypto sector boundary.
 */
static int add_sector_size_param(DmTargetCrypt* target, struct crypt_mnt_ftr* ftr) {
    constexpr char DM_CRYPT_SECTOR_SIZE[] = "ro.crypto.fde_sector_size";
    char value[PROPERTY_VALUE_MAX];

    if (property_get(DM_CRYPT_SECTOR_SIZE, value, "") > 0) {
        unsigned int sector_size;

        if (!ParseUint(value, &sector_size) || sector_size < 512 || sector_size > 4096 ||
            (sector_size & (sector_size - 1)) != 0) {
            SLOGE("Invalid value for %s: %s.  Must be >= 512, <= 4096, and a power of 2\n",
                  DM_CRYPT_SECTOR_SIZE, value);
            return -1;
        }

        target->SetSectorSize(sector_size);

        // With this option, IVs will match the sector numbering, instead
        // of being hard-coded to being based on 512-byte sectors.
        target->SetIvLargeSectors();

        // Round the crypto device size down to a crypto sector boundary.
        ftr->fs_size &= ~((sector_size / 512) - 1);
    }
    return 0;
}

static int create_crypto_blk_dev(struct crypt_mnt_ftr* crypt_ftr, const unsigned char* master_key,
                                 const char* real_blk_name, std::string* crypto_blk_name,
                                 const char* name, uint32_t flags) {
    auto& dm = DeviceMapper::Instance();
    ALOGE("create_crypto_blk_dev\n");

    // We need two ASCII characters to represent each byte, and need space for
    // the '\0' terminator.
    char master_key_ascii[MAX_KEY_LEN * 2 + 1];
    convert_key_to_hex_ascii(master_key, crypt_ftr->keysize, master_key_ascii);

    auto target = std::make_unique<DmTargetCrypt>(0, crypt_ftr->fs_size,
                                                  (const char*)crypt_ftr->crypto_type_name,
                                                  master_key_ascii, 0, real_blk_name, 0);
    target->AllowDiscards();

    if (flags & CREATE_CRYPTO_BLK_DEV_FLAGS_ALLOW_ENCRYPT_OVERRIDE) {
        target->AllowEncryptOverride();
    }
    if (add_sector_size_param(target.get(), crypt_ftr)) {
        SLOGE("Error processing dm-crypt sector size param\n");
        return -1;
    }

    DmTable table;
    table.AddTarget(std::move(target));

    int load_count = 1;
    while (load_count < TABLE_LOAD_RETRIES) {
        if (dm.CreateDevice(name, table)) {
            break;
        }
        load_count++;
    }

    if (load_count >= TABLE_LOAD_RETRIES) {
        SLOGE("Cannot load dm-crypt mapping table.\n");
        return -1;
    }
    if (load_count > 1) {
        SLOGI("Took %d tries to load dmcrypt table.\n", load_count);
    }

    // ALOGE("GetDmDevicePathByName::%s::%s\n", name, crypto_blk_name->c_str());
    if (!dm.GetDmDevicePathByName(name, crypto_blk_name)) {
        SLOGE("Cannot determine dm-crypt path for %s.\n", name);
        return -1;
    }

    /* Ensure the dm device has been created before returning. */
    if (::WaitForFile(crypto_blk_name->c_str(), 1s) < 0) {
        // WaitForFile generates a suitable log message
        return -1;
    }
    return 0;
}

static int delete_crypto_blk_dev(const std::string& name) {
    bool ret;
    auto& dm = DeviceMapper::Instance();
    // TODO(b/149396179) there appears to be a race somewhere in the system where trying
    // to delete the device fails with EBUSY; for now, work around this by retrying.
    int tries = 5;
    while (tries-- > 0) {
        ret = dm.DeleteDevice(name);
        if (ret || errno != EBUSY) {
            break;
        }
        SLOGW("DM_DEV Cannot remove dm-crypt device %s: %s, retrying...\n", name.c_str(),
              strerror(errno));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ret) {
        SLOGE("DM_DEV Cannot remove dm-crypt device %s: %s\n", name.c_str(), strerror(errno));
        return -1;
    }
    return 0;
}

static int pbkdf2(const char* passwd, const unsigned char* salt, unsigned char* ikey,
                  void* params UNUSED) {
    SLOGI("Using pbkdf2 for cryptfs KDF");

    /* Turn the password into a key and IV that can decrypt the master key */
    return PKCS5_PBKDF2_HMAC_SHA1(passwd, strlen(passwd), salt, SALT_LEN, HASH_COUNT,
                                  INTERMEDIATE_BUF_SIZE, ikey) != 1;
}

static int scrypt(const char* passwd, const unsigned char* salt, unsigned char* ikey, void* params) {
    SLOGI("Using scrypt for cryptfs KDF");

    struct crypt_mnt_ftr* ftr = (struct crypt_mnt_ftr*)params;

    int N = 1 << ftr->N_factor;
    int r = 1 << ftr->r_factor;
    int p = 1 << ftr->p_factor;

    /* Turn the password into a key and IV that can decrypt the master key */
    crypto_scrypt((const uint8_t*)passwd, strlen(passwd), salt, SALT_LEN, N, r, p, ikey,
                  INTERMEDIATE_BUF_SIZE);

    return 0;
}

static int scrypt_keymaster(const char* passwd, const unsigned char* salt, unsigned char* ikey,
                            void* params) {
    SLOGI("Using scrypt with keymaster for cryptfs KDF");

    int rc;
    size_t signature_size;
    unsigned char* signature;
    struct crypt_mnt_ftr* ftr = (struct crypt_mnt_ftr*)params;

    int N = 1 << ftr->N_factor;
    int r = 1 << ftr->r_factor;
    int p = 1 << ftr->p_factor;

    rc = crypto_scrypt((const uint8_t*)passwd, strlen(passwd), salt, SALT_LEN, N, r, p, ikey,
                       INTERMEDIATE_BUF_SIZE);

    if (rc) {
        SLOGE("scrypt failed");
        return -1;
    }

    if (keymaster_sign_object(ftr, ikey, INTERMEDIATE_BUF_SIZE, &signature, &signature_size)) {
        SLOGE("Signing failed");
        return -1;
    }

    rc = crypto_scrypt(signature, signature_size, salt, SALT_LEN, N, r, p, ikey,
                       INTERMEDIATE_BUF_SIZE);
    free(signature);

    if (rc) {
        SLOGE("scrypt failed");
        return -1;
    }

    return 0;
}

static int encrypt_master_key(const char* passwd, const unsigned char* salt,
                              const unsigned char* decrypted_master_key,
                              unsigned char* encrypted_master_key, struct crypt_mnt_ftr* crypt_ftr) {
    unsigned char ikey[INTERMEDIATE_BUF_SIZE] = {0};
    EVP_CIPHER_CTX e_ctx;
    int encrypted_len, final_len;
    int rc = 0;

    /* Turn the password into an intermediate key and IV that can decrypt the master key */
    get_device_scrypt_params(crypt_ftr);

    switch (crypt_ftr->kdf_type) {
        case KDF_SCRYPT_KEYMASTER:
            if (keymaster_create_key(crypt_ftr)) {
                SLOGE("keymaster_create_key failed");
                return -1;
            }

            if (scrypt_keymaster(passwd, salt, ikey, crypt_ftr)) {
                SLOGE("scrypt failed");
                return -1;
            }
            break;

        case KDF_SCRYPT:
            if (scrypt(passwd, salt, ikey, crypt_ftr)) {
                SLOGE("scrypt failed");
                return -1;
            }
            break;

        default:
            SLOGE("Invalid kdf_type");
            return -1;
    }

    /* Initialize the decryption engine */
    EVP_CIPHER_CTX_init(&e_ctx);
    if (!EVP_EncryptInit_ex(&e_ctx, EVP_aes_128_cbc(), NULL, ikey,
                            ikey + INTERMEDIATE_KEY_LEN_BYTES)) {
        SLOGE("EVP_EncryptInit failed\n");
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(&e_ctx, 0); /* Turn off padding as our data is block aligned */

    /* Encrypt the master key */
    if (!EVP_EncryptUpdate(&e_ctx, encrypted_master_key, &encrypted_len, decrypted_master_key,
                           crypt_ftr->keysize)) {
        SLOGE("EVP_EncryptUpdate failed\n");
        return -1;
    }
    if (!EVP_EncryptFinal_ex(&e_ctx, encrypted_master_key + encrypted_len, &final_len)) {
        SLOGE("EVP_EncryptFinal failed\n");
        return -1;
    }

    if (encrypted_len + final_len != static_cast<int>(crypt_ftr->keysize)) {
        SLOGE("EVP_Encryption length check failed with %d, %d bytes\n", encrypted_len, final_len);
        return -1;
    }

    /* Store the scrypt of the intermediate key, so we can validate if it's a
       password error or mount error when things go wrong.
       Note there's no need to check for errors, since if this is incorrect, we
       simply won't wipe userdata, which is the correct default behavior
    */
    int N = 1 << crypt_ftr->N_factor;
    int r = 1 << crypt_ftr->r_factor;
    int p = 1 << crypt_ftr->p_factor;

    rc = crypto_scrypt(ikey, INTERMEDIATE_KEY_LEN_BYTES, crypt_ftr->salt, sizeof(crypt_ftr->salt),
                       N, r, p, crypt_ftr->scrypted_intermediate_key,
                       sizeof(crypt_ftr->scrypted_intermediate_key));

    if (rc) {
        SLOGE("encrypt_master_key: crypto_scrypt failed");
    }

    EVP_CIPHER_CTX_cleanup(&e_ctx);

    return 0;
}

static int decrypt_master_key_aux(const char* passwd, unsigned char* salt,
                                  const unsigned char* encrypted_master_key, size_t keysize,
                                  unsigned char* decrypted_master_key, kdf_func kdf,
                                  void* kdf_params, unsigned char** intermediate_key,
                                  size_t* intermediate_key_size) {
    unsigned char ikey[INTERMEDIATE_BUF_SIZE] = {0};
    EVP_CIPHER_CTX d_ctx;
    int decrypted_len, final_len;

    /* Turn the password into an intermediate key and IV that can decrypt the
       master key */
    if (kdf(passwd, salt, ikey, kdf_params)) {
        SLOGE("kdf failed");
        return -1;
    }

    /* Initialize the decryption engine */
    EVP_CIPHER_CTX_init(&d_ctx);
    if (!EVP_DecryptInit_ex(&d_ctx, EVP_aes_128_cbc(), NULL, ikey,
                            ikey + INTERMEDIATE_KEY_LEN_BYTES)) {
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(&d_ctx, 0); /* Turn off padding as our data is block aligned */
    /* Decrypt the master key */
    if (!EVP_DecryptUpdate(&d_ctx, decrypted_master_key, &decrypted_len, encrypted_master_key,
                           keysize)) {
        return -1;
    }
    if (!EVP_DecryptFinal_ex(&d_ctx, decrypted_master_key + decrypted_len, &final_len)) {
        return -1;
    }

    if (decrypted_len + final_len != static_cast<int>(keysize)) {
        return -1;
    }

    /* Copy intermediate key if needed by params */
    if (intermediate_key && intermediate_key_size) {
        *intermediate_key = (unsigned char*)malloc(INTERMEDIATE_KEY_LEN_BYTES);
        if (*intermediate_key) {
            memcpy(*intermediate_key, ikey, INTERMEDIATE_KEY_LEN_BYTES);
            *intermediate_key_size = INTERMEDIATE_KEY_LEN_BYTES;
        }
    }

    EVP_CIPHER_CTX_cleanup(&d_ctx);

    return 0;
}

static void get_kdf_func(struct crypt_mnt_ftr* ftr, kdf_func* kdf, void** kdf_params) {
    if (ftr->kdf_type == KDF_SCRYPT_KEYMASTER) {
        *kdf = scrypt_keymaster;
        *kdf_params = ftr;
    } else if (ftr->kdf_type == KDF_SCRYPT) {
        *kdf = scrypt;
        *kdf_params = ftr;
    } else {
        *kdf = pbkdf2;
        *kdf_params = NULL;
    }
}

static int decrypt_master_key(const char* passwd, unsigned char* decrypted_master_key,
                              struct crypt_mnt_ftr* crypt_ftr, unsigned char** intermediate_key,
                              size_t* intermediate_key_size) {
    kdf_func kdf;
    void* kdf_params;
    int ret;

    get_kdf_func(crypt_ftr, &kdf, &kdf_params);
    ret = decrypt_master_key_aux(passwd, crypt_ftr->salt, crypt_ftr->master_key, crypt_ftr->keysize,
                                 decrypted_master_key, kdf, kdf_params, intermediate_key,
                                 intermediate_key_size);
    if (ret != 0) {
        SLOGW("failure decrypting master key");
    }

    return ret;
}

static int create_encrypted_random_key(const char* passwd, unsigned char* master_key,
                                       unsigned char* salt, struct crypt_mnt_ftr* crypt_ftr) {
    unsigned char key_buf[MAX_KEY_LEN];

    /* Get some random bits for a key and salt */
    if (::ReadRandomBytes(sizeof(key_buf), reinterpret_cast<char*>(key_buf)) != 0) {
        return -1;
    }
    if (::ReadRandomBytes(SALT_LEN, reinterpret_cast<char*>(salt)) != 0) {
        return -1;
    }

    /* Now encrypt it with the password */
    return encrypt_master_key(passwd, salt, key_buf, master_key, crypt_ftr);
}

static void ensure_subdirectory_unmounted(const char *prefix) {
    std::vector<std::string> umount_points;
    std::unique_ptr<FILE, int (*)(FILE*)> mnts(setmntent("/proc/mounts", "r"), endmntent);
    if (!mnts) {
        SLOGW("could not read mount files");
        return;
    }

    //Find sudirectory mount point
    mntent* mentry;
    std::string top_directory(prefix);
    if (!android::base::EndsWith(prefix, "/")) {
        top_directory = top_directory + "/";
    }
    while ((mentry = getmntent(mnts.get())) != nullptr) {
        if (strcmp(mentry->mnt_dir, top_directory.c_str()) == 0) {
            continue;
        }

        if (android::base::StartsWith(mentry->mnt_dir, top_directory)) {
            SLOGW("found sub-directory mount %s - %s\n", prefix, mentry->mnt_dir);
            umount_points.push_back(mentry->mnt_dir);
        }
    }

    //Sort by path length to umount longest path first
    std::sort(std::begin(umount_points), std::end(umount_points),
        [](const std::string& s1, const std::string& s2) {return s1.length() > s2.length(); });

    for (std::string& mount_point : umount_points) {
        umount(mount_point.c_str());
        SLOGW("umount sub-directory mount %s\n", mount_point.c_str());
    }
}

static int wait_and_unmount(const char* mountpoint, bool kill) {
    int i, err, rc;

    // Subdirectory mount will cause a failure of umount.
    ensure_subdirectory_unmounted(mountpoint);
#define WAIT_UNMOUNT_COUNT 20

    /*  Now umount the tmpfs filesystem */
    for (i = 0; i < WAIT_UNMOUNT_COUNT; i++) {
        if (umount(mountpoint) == 0) {
            break;
        }

        if (errno == EINVAL) {
            /* EINVAL is returned if the directory is not a mountpoint,
             * i.e. there is no filesystem mounted there.  So just get out.
             */
            break;
        }

        err = errno;

        /* If allowed, be increasingly aggressive before the last two retries */
        if (kill) {
            if (i == (WAIT_UNMOUNT_COUNT - 3)) {
                SLOGW("sending SIGHUP to processes with open files\n");
                android::vold::KillProcessesWithOpenFiles(mountpoint, SIGTERM);
            } else if (i == (WAIT_UNMOUNT_COUNT - 2)) {
                SLOGW("sending SIGKILL to processes with open files\n");
                android::vold::KillProcessesWithOpenFiles(mountpoint, SIGKILL);
            }
        }

        sleep(1);
    }

    if (i < WAIT_UNMOUNT_COUNT) {
        SLOGD("unmounting %s succeeded\n", mountpoint);
        rc = 0;
    } else {
        android::vold::KillProcessesWithOpenFiles(mountpoint, 0);
        SLOGE("unmounting %s failed: %s\n", mountpoint, strerror(err));
        rc = -1;
    }

    return rc;
}

static void prep_data_fs(void) {
    // NOTE: post_fs_data results in init calling back around to vold, so all
    // callers to this method must be async

    /* Do the prep of the /data filesystem */
    property_set("vold.post_fs_data_done", "0");
    property_set("vold.decrypt", "trigger_post_fs_data");
    SLOGD("Just triggered post_fs_data");

    /* Wait a max of 50 seconds, hopefully it takes much less */
    while (!android::base::WaitForProperty("vold.post_fs_data_done", "1", std::chrono::seconds(15))) {
        /* We timed out to prep /data in time.  Continue wait. */
        SLOGE("waited 15s for vold.post_fs_data_done, still waiting...");
    }
    SLOGD("post_fs_data done");
}

static void cryptfs_set_corrupt() {
    // Mark the footer as bad
    struct crypt_mnt_ftr crypt_ftr;
    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        SLOGE("Failed to get crypto footer - panic");
        return;
    }

    crypt_ftr.flags |= CRYPT_DATA_CORRUPT;
    if (put_crypt_ftr_and_key(&crypt_ftr)) {
        SLOGE("Failed to set crypto footer - panic");
        return;
    }
}

static void cryptfs_trigger_restart_min_framework() {
    if (fs_mgr_do_tmpfs_mount(DATA_MNT_POINT)) {
        SLOGE("Failed to mount tmpfs on data - panic");
        return;
    }

    if (property_set("vold.decrypt", "trigger_post_fs_data")) {
        SLOGE("Failed to trigger post fs data - panic");
        return;
    }

    if (property_set("vold.decrypt", "trigger_restart_min_framework")) {
        SLOGE("Failed to trigger restart min framework - panic");
        return;
    }
}

/* returns < 0 on failure */
static int cryptfs_restart_internal(int restart_main) {
    char crypto_blkdev[MAXPATHLEN];
    int rc = -1;
    static int restart_successful = 0;

    /* Validate that it's OK to call this routine */
    if (!master_key_saved) {
        SLOGE("Encrypted filesystem not validated, aborting");
        return -1;
    }

    if (restart_successful) {
        SLOGE("System already restarted with encrypted disk, aborting");
        return -1;
    }

    if (restart_main) {
        /* Here is where we shut down the framework.  The init scripts
         * start all services in one of these classes: core, early_hal, hal,
         * main and late_start. To get to the minimal UI for PIN entry, we
         * need to start core, early_hal, hal and main. When we want to
         * shutdown the framework again, we need to stop most of the services in
         * these classes, but only those services that were started after
         * /data was mounted. This excludes critical services like vold and
         * ueventd, which need to keep running. We could possible stop
         * even fewer services, but because we want services to pick up APEX
         * libraries from the real /data, restarting is better, as it makes
         * these devices consistent with FBE devices and lets them use the
         * most recent code.
         *
         * Once these services have stopped, we should be able
         * to umount the tmpfs /data, then mount the encrypted /data.
         * We then restart the class core, hal, main, and also the class
         * late_start.
         *
         * At the moment, I've only put a few things in late_start that I know
         * are not needed to bring up the framework, and that also cause problems
         * with unmounting the tmpfs /data, but I hope to add add more services
         * to the late_start class as we optimize this to decrease the delay
         * till the user is asked for the password to the filesystem.
         */

        /* The init files are setup to stop the right set of services when
         * vold.decrypt is set to trigger_shutdown_framework.
         */
        property_set("vold.decrypt", "trigger_shutdown_framework");
        SLOGD("Just asked init to shut down class main\n");

        /* Ugh, shutting down the framework is not synchronous, so until it
         * can be fixed, this horrible hack will wait a moment for it all to
         * shut down before proceeding.  Without it, some devices cannot
         * restart the graphics services.
         */
        sleep(2);
    }

    /* Now that the framework is shutdown, we should be able to umount()
     * the tmpfs filesystem, and mount the real one.
     */

    property_get("ro.crypto.fs_crypto_blkdev", crypto_blkdev, "");
    if (strlen(crypto_blkdev) == 0) {
        SLOGE("fs_crypto_blkdev not set\n");
        return -1;
    }

    if (!(rc = wait_and_unmount(DATA_MNT_POINT, true))) {
        /* If ro.crypto.readonly is set to 1, mount the decrypted
         * filesystem readonly.  This is used when /data is mounted by
         * recovery mode.
         */
        char ro_prop[PROPERTY_VALUE_MAX];
        property_get("ro.crypto.readonly", ro_prop, "");
        if (strlen(ro_prop) > 0 && std::stoi(ro_prop)) {
            auto entry = GetEntryForMountPoint(&fstab_default, DATA_MNT_POINT);
            if (entry != nullptr) {
                entry->flags |= MS_RDONLY;
            }
        }

        /* If that succeeded, then mount the decrypted filesystem */
        int retries = RETRY_MOUNT_ATTEMPTS;
        int mount_rc;

        /*
         * fs_mgr_do_mount runs fsck. Use setexeccon to run trusted
         * partitions in the fsck domain.
         */
        if (setexeccon(::sFsckContext)) {
            SLOGE("Failed to setexeccon");
            return -1;
        }
        bool needs_cp = ::cp_needsCheckpoint();
        while ((mount_rc = fs_mgr_do_mount(&fstab_default, DATA_MNT_POINT, crypto_blkdev, 0,
                                           needs_cp, false)) != 0) {
            if (mount_rc == FS_MGR_DOMNT_BUSY) {
                /* TODO: invoke something similar to
                   Process::killProcessWithOpenFiles(DATA_MNT_POINT,
                                   retries > RETRY_MOUNT_ATTEMPT/2 ? 1 : 2 ) */
                SLOGI("Failed to mount %s because it is busy - waiting", crypto_blkdev);
                if (--retries) {
                    sleep(RETRY_MOUNT_DELAY_SECONDS);
                } else {
                    /* Let's hope that a reboot clears away whatever is keeping
                       the mount busy */
                    cryptfs_reboot(RebootType::reboot);
                }
            } else {
                SLOGE("Failed to mount decrypted data");
                cryptfs_set_corrupt();
                cryptfs_trigger_restart_min_framework();
                SLOGI("Started framework to offer wipe");
                if (setexeccon(NULL)) {
                    SLOGE("Failed to setexeccon");
                }
                return -1;
            }
        }
        if (setexeccon(NULL)) {
            SLOGE("Failed to setexeccon");
            return -1;
        }

        /* Create necessary paths on /data */
        prep_data_fs();
        property_set("vold.decrypt", "trigger_load_persist_props");

        /* startup service classes main and late_start */
        property_set("vold.decrypt", "trigger_restart_framework");
        SLOGD("Just triggered restart_framework\n");

        /* Give it a few moments to get started */
        sleep(1);
    }

    if (rc == 0) {
        restart_successful = 1;
    }

    return rc;
}

int cryptfs_restart(void) {
    SLOGI("cryptfs_restart");
    if (fscrypt_is_native()) {
        SLOGE("cryptfs_restart not valid for file encryption:");
        return -1;
    }

    /* Call internal implementation forcing a restart of main service group */
    return cryptfs_restart_internal(1);
}

static int do_crypto_complete(const char* mount_point) {
    struct crypt_mnt_ftr crypt_ftr;
    char encrypted_state[PROPERTY_VALUE_MAX];

    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "encrypted")) {
        SLOGE("not running with encryption, aborting");
        return CRYPTO_COMPLETE_NOT_ENCRYPTED;
    }

    // crypto_complete is full disk encrypted status
    if (fscrypt_is_native()) {
        return CRYPTO_COMPLETE_NOT_ENCRYPTED;
    }

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        std::string key_loc;
        get_crypt_info(&key_loc, nullptr);

        /*
         * Only report this error if key_loc is a file and it exists.
         * If the device was never encrypted, and /data is not mountable for
         * some reason, returning 1 should prevent the UI from presenting the
         * a "enter password" screen, or worse, a "press button to wipe the
         * device" screen.
         */
        if (!key_loc.empty() && key_loc[0] == '/' && (access("key_loc", F_OK) == -1)) {
            SLOGE("master key file does not exist, aborting");
            return CRYPTO_COMPLETE_NOT_ENCRYPTED;
        } else {
            SLOGE("Error getting crypt footer and key\n");
            return CRYPTO_COMPLETE_BAD_METADATA;
        }
    }

    // Test for possible error flags
    if (crypt_ftr.flags & CRYPT_ENCRYPTION_IN_PROGRESS) {
        SLOGE("Encryption process is partway completed\n");
        return CRYPTO_COMPLETE_PARTIAL;
    }

    if (crypt_ftr.flags & CRYPT_INCONSISTENT_STATE) {
        SLOGE("Encryption process was interrupted but cannot continue\n");
        return CRYPTO_COMPLETE_INCONSISTENT;
    }

    if (crypt_ftr.flags & CRYPT_DATA_CORRUPT) {
        SLOGE("Encryption is successful but data is corrupt\n");
        return CRYPTO_COMPLETE_CORRUPT;
    }

    /* We passed the test! We shall diminish, and return to the west */
    return CRYPTO_COMPLETE_ENCRYPTED;
}

static int test_mount_encrypted_fs(struct crypt_mnt_ftr* crypt_ftr, const char* passwd,
                                   const char* mount_point, const char* label) {
    unsigned char decrypted_master_key[MAX_KEY_LEN];
    std::string crypto_blkdev;
    std::string real_blkdev;
    char tmp_mount_point[64];
    unsigned int orig_failed_decrypt_count;
    int rc;
    int use_keymaster = 0;
    int upgrade = 0;
    unsigned char* intermediate_key = 0;
    size_t intermediate_key_size = 0;
    int N = 1 << crypt_ftr->N_factor;
    int r = 1 << crypt_ftr->r_factor;
    int p = 1 << crypt_ftr->p_factor;

    SLOGD("crypt_ftr->fs_size = %lld\n", crypt_ftr->fs_size);
    orig_failed_decrypt_count = crypt_ftr->failed_decrypt_count;

    if (!(crypt_ftr->flags & CRYPT_MNT_KEY_UNENCRYPTED)) {
        if (decrypt_master_key(passwd, decrypted_master_key, crypt_ftr, &intermediate_key,
                               &intermediate_key_size)) {
            SLOGE("Failed to decrypt master key\n");
            rc = -1;
            goto errout;
        }
    }

    get_crypt_info(nullptr, &real_blkdev);

    // Create crypto block device - all (non fatal) code paths
    // need it
    if (create_crypto_blk_dev(crypt_ftr, decrypted_master_key, real_blkdev.c_str(), &crypto_blkdev,
                              label, 0)) {
        SLOGE("Error creating decrypted block device\n");
        rc = -1;
        goto errout;
    }

    /* Work out if the problem is the password or the data */
    unsigned char scrypted_intermediate_key[sizeof(crypt_ftr->scrypted_intermediate_key)];

    rc = crypto_scrypt(intermediate_key, intermediate_key_size, crypt_ftr->salt,
                       sizeof(crypt_ftr->salt), N, r, p, scrypted_intermediate_key,
                       sizeof(scrypted_intermediate_key));

    // Does the key match the crypto footer?
    if (rc == 0 && memcmp(scrypted_intermediate_key, crypt_ftr->scrypted_intermediate_key,
                          sizeof(scrypted_intermediate_key)) == 0) {
        SLOGI("Password matches");
        rc = 0;
    } else {
        /* Try mounting the file system anyway, just in case the problem's with
         * the footer, not the key. */
        snprintf(tmp_mount_point, sizeof(tmp_mount_point), "%s/tmp_mnt", mount_point);
        mkdir(tmp_mount_point, 0755);
        if (fs_mgr_do_mount(&fstab_default, DATA_MNT_POINT,
                            const_cast<char*>(crypto_blkdev.c_str()), tmp_mount_point)) {
            SLOGE("Error temp mounting decrypted block device\n");
            delete_crypto_blk_dev(label);

            rc = ++crypt_ftr->failed_decrypt_count;
            put_crypt_ftr_and_key(crypt_ftr);
        } else {
            /* Success! */
            SLOGI("Password did not match but decrypted drive mounted - continue");
            umount(tmp_mount_point);
            rc = 0;
        }
    }

    if (rc == 0) {
        crypt_ftr->failed_decrypt_count = 0;
        if (orig_failed_decrypt_count != 0) {
            put_crypt_ftr_and_key(crypt_ftr);
        }

        /* Save the name of the crypto block device
         * so we can mount it when restarting the framework. */
        property_set("ro.crypto.fs_crypto_blkdev", crypto_blkdev.c_str());

        /* Also save a the master key so we can reencrypted the key
         * the key when we want to change the password on it. */
        memcpy(saved_master_key, decrypted_master_key, crypt_ftr->keysize);
        saved_mount_point = strdup(mount_point);
        master_key_saved = 1;
        SLOGD("%s(): Master key saved\n", __FUNCTION__);
        rc = 0;

        // Upgrade if we're not using the latest KDF.
        use_keymaster = keymaster_check_compatibility();
        if (crypt_ftr->kdf_type == KDF_SCRYPT_KEYMASTER) {
            // Don't allow downgrade
        } else if (use_keymaster == 1 && crypt_ftr->kdf_type != KDF_SCRYPT_KEYMASTER) {
            crypt_ftr->kdf_type = KDF_SCRYPT_KEYMASTER;
            upgrade = 1;
        } else if (use_keymaster == 0 && crypt_ftr->kdf_type != KDF_SCRYPT) {
            crypt_ftr->kdf_type = KDF_SCRYPT;
            upgrade = 1;
        }

        if (upgrade) {
            rc = encrypt_master_key(passwd, crypt_ftr->salt, saved_master_key,
                                    crypt_ftr->master_key, crypt_ftr);
            if (!rc) {
                rc = put_crypt_ftr_and_key(crypt_ftr);
            }
            SLOGD("Key Derivation Function upgrade: rc=%d\n", rc);

            // Do not fail even if upgrade failed - machine is bootable
            // Note that if this code is ever hit, there is a *serious* problem
            // since KDFs should never fail. You *must* fix the kdf before
            // proceeding!
            if (rc) {
                SLOGW(
                    "Upgrade failed with error %d,"
                    " but continuing with previous state",
                    rc);
                rc = 0;
            }
        }
    }

errout:
    if (intermediate_key) {
        memset(intermediate_key, 0, intermediate_key_size);
        free(intermediate_key);
    }
    return rc;
}

/*
 * Called by vold when it's asked to mount an encrypted external
 * storage volume. The incoming partition has no crypto header/footer,
 * as any metadata is been stored in a separate, small partition.  We
 * assume it must be using our same crypt type and keysize.
 */
int cryptfs_setup_ext_volume(const char* label, const char* real_blkdev, const KeyBuffer& key,
                             std::string* out_crypto_blkdev) {
    auto crypto_type = get_crypto_type();
    if (key.size() != crypto_type.get_keysize()) {
        SLOGE("Raw keysize %zu does not match crypt keysize %zu", key.size(),
              crypto_type.get_keysize());
        return -1;
    }
    uint64_t nr_sec = 0;
    if (::GetBlockDev512Sectors(real_blkdev, &nr_sec) != android::OK) {
        SLOGE("Failed to get size of %s: %s", real_blkdev, strerror(errno));
        return -1;
    }

    struct crypt_mnt_ftr ext_crypt_ftr;
    memset(&ext_crypt_ftr, 0, sizeof(ext_crypt_ftr));
    ext_crypt_ftr.fs_size = nr_sec;
    ext_crypt_ftr.keysize = crypto_type.get_keysize();
    strlcpy((char*)ext_crypt_ftr.crypto_type_name, crypto_type.get_kernel_name(),
            MAX_CRYPTO_TYPE_NAME_LEN);
    uint32_t flags = 0;
    if (fscrypt_is_native() &&
        android::base::GetBoolProperty("ro.crypto.allow_encrypt_override", false))
        flags |= CREATE_CRYPTO_BLK_DEV_FLAGS_ALLOW_ENCRYPT_OVERRIDE;

    return create_crypto_blk_dev(&ext_crypt_ftr, reinterpret_cast<const unsigned char*>(key.data()),
                                 real_blkdev, out_crypto_blkdev, label, flags);
}

int cryptfs_crypto_complete(void) {
    return do_crypto_complete("/data");
}

int check_unmounted_and_get_ftr(struct crypt_mnt_ftr* crypt_ftr) {
    char encrypted_state[PROPERTY_VALUE_MAX];
    property_get("ro.crypto.state", encrypted_state, "");
    if (master_key_saved || strcmp(encrypted_state, "encrypted")) {
        SLOGE(
            "encrypted fs already validated or not running with encryption,"
            " aborting");
        return -1;
    }

    if (get_crypt_ftr_and_key(crypt_ftr)) {
        SLOGE("Error getting crypt footer and key");
        return -1;
    }

    return 0;
}

int cryptfs_check_passwd(const char* passwd) {
    SLOGI("cryptfs_check_passwd");
    if (fscrypt_is_native()) {
        SLOGE("cryptfs_check_passwd not valid for file encryption");
        return -1;
    }

    struct crypt_mnt_ftr crypt_ftr;
    int rc;

    rc = check_unmounted_and_get_ftr(&crypt_ftr);
    if (rc) {
        SLOGE("Could not get footer");
        return rc;
    }

    rc = test_mount_encrypted_fs(&crypt_ftr, passwd, DATA_MNT_POINT, CRYPTO_BLOCK_DEVICE);
    if (rc) {
        SLOGE("Password did not match");
        return rc;
    }

    if (crypt_ftr.flags & CRYPT_FORCE_COMPLETE) {
        // Here we have a default actual password but a real password
        // we must test against the scrypted value
        // First, we must delete the crypto block device that
        // test_mount_encrypted_fs leaves behind as a side effect
        delete_crypto_blk_dev(CRYPTO_BLOCK_DEVICE);
        rc = test_mount_encrypted_fs(&crypt_ftr, DEFAULT_PASSWORD, DATA_MNT_POINT,
                                     CRYPTO_BLOCK_DEVICE);
        if (rc) {
            SLOGE("Default password did not match on reboot encryption");
            return rc;
        }

        crypt_ftr.flags &= ~CRYPT_FORCE_COMPLETE;
        put_crypt_ftr_and_key(&crypt_ftr);
        rc = cryptfs_changepw(crypt_ftr.crypt_type, passwd);
        if (rc) {
            SLOGE("Could not change password on reboot encryption");
            return rc;
        }
    }

    if (crypt_ftr.crypt_type != CRYPT_TYPE_DEFAULT) {
        cryptfs_clear_password();
        password = strdup(passwd);
        struct timespec now;
        clock_gettime(CLOCK_BOOTTIME, &now);
        password_expiry_time = now.tv_sec + password_max_age_seconds;
    }

    return rc;
}

int cryptfs_verify_passwd(const char* passwd) {
    struct crypt_mnt_ftr crypt_ftr;
    unsigned char decrypted_master_key[MAX_KEY_LEN];
    char encrypted_state[PROPERTY_VALUE_MAX];
    int rc;

    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "encrypted")) {
        SLOGE("device not encrypted, aborting");
        return -2;
    }

    if (!master_key_saved) {
        SLOGE("encrypted fs not yet mounted, aborting");
        return -1;
    }

    if (!saved_mount_point) {
        SLOGE("encrypted fs failed to save mount point, aborting");
        return -1;
    }

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        SLOGE("Error getting crypt footer and key\n");
        return -1;
    }

    if (crypt_ftr.flags & CRYPT_MNT_KEY_UNENCRYPTED) {
        /* If the device has no password, then just say the password is valid */
        rc = 0;
    } else {
        decrypt_master_key(passwd, decrypted_master_key, &crypt_ftr, 0, 0);
        if (!memcmp(decrypted_master_key, saved_master_key, crypt_ftr.keysize)) {
            /* They match, the password is correct */
            rc = 0;
        } else {
            /* If incorrect, sleep for a bit to prevent dictionary attacks */
            sleep(1);
            rc = 1;
        }
    }

    return rc;
}

/* Initialize a crypt_mnt_ftr structure.  The keysize is
 * defaulted to get_crypto_type().get_keysize() bytes, and the filesystem size to 0.
 * Presumably, at a minimum, the caller will update the
 * filesystem size and crypto_type_name after calling this function.
 */
static int cryptfs_init_crypt_mnt_ftr(struct crypt_mnt_ftr* ftr) {
    off64_t off;

    memset(ftr, 0, sizeof(struct crypt_mnt_ftr));
    ftr->magic = CRYPT_MNT_MAGIC;
    ftr->major_version = CURRENT_MAJOR_VERSION;
    ftr->minor_version = CURRENT_MINOR_VERSION;
    ftr->ftr_size = sizeof(struct crypt_mnt_ftr);
    ftr->keysize = get_crypto_type().get_keysize();

    switch (keymaster_check_compatibility()) {
        case 1:
            ftr->kdf_type = KDF_SCRYPT_KEYMASTER;
            break;

        case 0:
            ftr->kdf_type = KDF_SCRYPT;
            break;

        default:
            SLOGE("keymaster_check_compatibility failed");
            return -1;
    }

    get_device_scrypt_params(ftr);

    ftr->persist_data_size = CRYPT_PERSIST_DATA_SIZE;
    if (get_crypt_ftr_info(NULL, &off) == 0) {
        ftr->persist_data_offset[0] = off + CRYPT_FOOTER_TO_PERSIST_OFFSET;
        ftr->persist_data_offset[1] = off + CRYPT_FOOTER_TO_PERSIST_OFFSET + ftr->persist_data_size;
    }

    return 0;
}

#define FRAMEWORK_BOOT_WAIT 60

static int cryptfs_SHA256_fileblock(const char* filename, __le8* buf) {
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        SLOGE("Error opening file %s", filename);
        return -1;
    }

    char block[CRYPT_INPLACE_BUFSIZE];
    memset(block, 0, sizeof(block));
    if (unix_read(fd, block, sizeof(block)) < 0) {
        SLOGE("Error reading file %s", filename);
        close(fd);
        return -1;
    }

    close(fd);

    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, block, sizeof(block));
    SHA256_Final(buf, &c);

    return 0;
}

static int cryptfs_enable_all_volumes(struct crypt_mnt_ftr* crypt_ftr, const char* crypto_blkdev,
                                      const char* real_blkdev, int previously_encrypted_upto) {
    off64_t cur_encryption_done = 0, tot_encryption_size = 0;
    int rc = -1;

    /* The size of the userdata partition, and add in the vold volumes below */
    tot_encryption_size = crypt_ftr->fs_size;

    rc = cryptfs_enable_inplace(crypto_blkdev, real_blkdev, crypt_ftr->fs_size, &cur_encryption_done,
                                tot_encryption_size, previously_encrypted_upto, true);

    if (rc == ENABLE_INPLACE_ERR_DEV) {
        /* Hack for b/17898962 */
        SLOGE("cryptfs_enable: crypto block dev failure. Must reboot...\n");
        cryptfs_reboot(RebootType::reboot);
    }

    if (!rc) {
        crypt_ftr->encrypted_upto = cur_encryption_done;
    }

    if (!rc && crypt_ftr->encrypted_upto == crypt_ftr->fs_size) {
        /* The inplace routine never actually sets the progress to 100% due
         * to the round down nature of integer division, so set it here */
        property_set("vold.encrypt_progress", "100");
    }

    return rc;
}

// static int vold_unmountAll(void) {
//     VolumeManager* vm = VolumeManager::Instance();
//     return vm->unmountAll();
// }

int cryptfs_enable_internal(int crypt_type, const char* passwd, int no_ui) {
    std::string crypto_blkdev;
    std::string real_blkdev;
    unsigned char decrypted_master_key[MAX_KEY_LEN];
    int rc = -1, i;
    struct crypt_mnt_ftr crypt_ftr;
    struct crypt_persist_data* pdata;
    char encrypted_state[PROPERTY_VALUE_MAX];
    char lockid[32] = {0};
    std::string key_loc;
    int num_vols;
    off64_t previously_encrypted_upto = 0;
    bool rebootEncryption = false;
    bool onlyCreateHeader = false;
    // std::unique_ptr<android::wakelock::WakeLock> wakeLock = nullptr;

    if (get_crypt_ftr_and_key(&crypt_ftr) == 0) {
        if (crypt_ftr.flags & CRYPT_ENCRYPTION_IN_PROGRESS) {
            /* An encryption was underway and was interrupted */
            previously_encrypted_upto = crypt_ftr.encrypted_upto;
            crypt_ftr.encrypted_upto = 0;
            crypt_ftr.flags &= ~CRYPT_ENCRYPTION_IN_PROGRESS;

            /* At this point, we are in an inconsistent state. Until we successfully
               complete encryption, a reboot will leave us broken. So mark the
               encryption failed in case that happens.
               On successfully completing encryption, remove this flag */
            crypt_ftr.flags |= CRYPT_INCONSISTENT_STATE;

            put_crypt_ftr_and_key(&crypt_ftr);
        } else if (crypt_ftr.flags & CRYPT_FORCE_ENCRYPTION) {
            if (!check_ftr_sha(&crypt_ftr)) {
                memset(&crypt_ftr, 0, sizeof(crypt_ftr));
                put_crypt_ftr_and_key(&crypt_ftr);
                goto error_unencrypted;
            }

            /* Doing a reboot-encryption*/
            crypt_ftr.flags &= ~CRYPT_FORCE_ENCRYPTION;
            crypt_ftr.flags |= CRYPT_FORCE_COMPLETE;
            rebootEncryption = true;
        }
    } else {
        // We don't want to accidentally reference invalid data.
        memset(&crypt_ftr, 0, sizeof(crypt_ftr));
    }

    property_get("ro.crypto.state", encrypted_state, "");
    if (!strcmp(encrypted_state, "encrypted") && !previously_encrypted_upto) {
        SLOGE("Device is already running encrypted, aborting");
        goto error_unencrypted;
    }

    get_crypt_info(&key_loc, &real_blkdev);

    /* Get the size of the real block device */
    uint64_t nr_sec;
    if (::GetBlockDev512Sectors(real_blkdev, &nr_sec) != android::OK) {
        SLOGE("Cannot get size of block device %s\n", real_blkdev.c_str());
        goto error_unencrypted;
    }

    /* If doing inplace encryption, make sure the orig fs doesn't include the crypto footer */
    if (key_loc == KEY_IN_FOOTER) {
        uint64_t fs_size_sec, max_fs_size_sec;
        fs_size_sec = get_fs_size(real_blkdev.c_str());
        if (fs_size_sec == 0) fs_size_sec = get_f2fs_filesystem_size_sec(real_blkdev.data());

        max_fs_size_sec = nr_sec - (CRYPT_FOOTER_OFFSET / CRYPT_SECTOR_SIZE);

        if (fs_size_sec > max_fs_size_sec) {
            SLOGE("Orig filesystem overlaps crypto footer region.  Cannot encrypt in place.");
            goto error_unencrypted;
        }
    }

    /* Get a wakelock as this may take a while, and we don't want the
     * device to sleep on us.  We'll grab a partial wakelock, and if the UI
     * wants to keep the screen on, it can grab a full wakelock.
     */
    snprintf(lockid, sizeof(lockid), "enablecrypto%d", (int)getpid());
    // wakeLock = std::make_unique<android::wakelock::WakeLock>(lockid);

    /* The init files are setup to stop the class main and late start when
     * vold sets trigger_shutdown_framework.
     */
    property_set("vold.decrypt", "trigger_shutdown_framework");
    SLOGD("Just asked init to shut down class main\n");

    /* Ask vold to unmount all devices that it manages */
    // if (vold_unmountAll()) {
    //     SLOGE("Failed to unmount all vold managed devices");
    // }

    /* no_ui means we are being called from init, not settings.
       Now we always reboot from settings, so !no_ui means reboot
     */
    if (!no_ui) {
        /* Try fallback, which is to reboot and try there */
        onlyCreateHeader = true;
        FILE* breadcrumb = fopen(BREADCRUMB_FILE, "we");
        if (breadcrumb == 0) {
            SLOGE("Failed to create breadcrumb file");
            goto error_shutting_down;
        }
        fclose(breadcrumb);
    }

    /* Do extra work for a better UX when doing the long inplace encryption */
    if (!onlyCreateHeader) {
        /* Now that /data is unmounted, we need to mount a tmpfs
         * /data, set a property saying we're doing inplace encryption,
         * and restart the framework.
         */
        wait_and_unmount(DATA_MNT_POINT, true);
        if (fs_mgr_do_tmpfs_mount(DATA_MNT_POINT)) {
            goto error_shutting_down;
        }
        /* Tells the framework that inplace encryption is starting */
        property_set("vold.encrypt_progress", "0");

        /* restart the framework. */
        /* Create necessary paths on /data */
        prep_data_fs();

        /* Ugh, shutting down the framework is not synchronous, so until it
         * can be fixed, this horrible hack will wait a moment for it all to
         * shut down before proceeding.  Without it, some devices cannot
         * restart the graphics services.
         */
        sleep(2);
    }

    /* Start the actual work of making an encrypted filesystem */
    /* Initialize a crypt_mnt_ftr for the partition */
    if (previously_encrypted_upto == 0 && !rebootEncryption) {
        if (cryptfs_init_crypt_mnt_ftr(&crypt_ftr)) {
            goto error_shutting_down;
        }

        if (key_loc == KEY_IN_FOOTER) {
            crypt_ftr.fs_size = nr_sec - (CRYPT_FOOTER_OFFSET / CRYPT_SECTOR_SIZE);
        } else {
            crypt_ftr.fs_size = nr_sec;
        }
        /* At this point, we are in an inconsistent state. Until we successfully
           complete encryption, a reboot will leave us broken. So mark the
           encryption failed in case that happens.
           On successfully completing encryption, remove this flag */
        if (onlyCreateHeader) {
            crypt_ftr.flags |= CRYPT_FORCE_ENCRYPTION;
        } else {
            crypt_ftr.flags |= CRYPT_INCONSISTENT_STATE;
        }
        crypt_ftr.crypt_type = crypt_type;
        strlcpy((char*)crypt_ftr.crypto_type_name, get_crypto_type().get_kernel_name(),
                MAX_CRYPTO_TYPE_NAME_LEN);

        /* Make an encrypted master key */
        if (create_encrypted_random_key(onlyCreateHeader ? DEFAULT_PASSWORD : passwd,
                                        crypt_ftr.master_key, crypt_ftr.salt, &crypt_ftr)) {
            SLOGE("Cannot create encrypted master key\n");
            goto error_shutting_down;
        }

        /* Replace scrypted intermediate key if we are preparing for a reboot */
        if (onlyCreateHeader) {
            unsigned char fake_master_key[MAX_KEY_LEN];
            unsigned char encrypted_fake_master_key[MAX_KEY_LEN];
            memset(fake_master_key, 0, sizeof(fake_master_key));
            encrypt_master_key(passwd, crypt_ftr.salt, fake_master_key, encrypted_fake_master_key,
                               &crypt_ftr);
        }

        /* Write the key to the end of the partition */
        put_crypt_ftr_and_key(&crypt_ftr);

        /* If any persistent data has been remembered, save it.
         * If none, create a valid empty table and save that.
         */
        if (!persist_data) {
            pdata = (crypt_persist_data*)malloc(CRYPT_PERSIST_DATA_SIZE);
            if (pdata) {
                init_empty_persist_data(pdata, CRYPT_PERSIST_DATA_SIZE);
                persist_data = pdata;
            }
        }
        if (persist_data) {
            save_persistent_data();
        }
    }

    if (onlyCreateHeader) {
        sleep(2);
        cryptfs_reboot(RebootType::reboot);
    }

    if (!no_ui || rebootEncryption) {
        /* startup service classes main and late_start */
        property_set("vold.decrypt", "trigger_restart_min_framework");
        SLOGD("Just triggered restart_min_framework\n");

        /* OK, the framework is restarted and will soon be showing a
         * progress bar.  Time to setup an encrypted mapping, and
         * either write a new filesystem, or encrypt in place updating
         * the progress bar as we work.
         */
    }

    decrypt_master_key(passwd, decrypted_master_key, &crypt_ftr, 0, 0);
    ALOGE("cryptfs_enable_internal\n");
    create_crypto_blk_dev(&crypt_ftr, decrypted_master_key, real_blkdev.c_str(), &crypto_blkdev,
                          CRYPTO_BLOCK_DEVICE, 0);

    /* If we are continuing, check checksums match */
    rc = 0;
    if (previously_encrypted_upto) {
        __le8 hash_first_block[SHA256_DIGEST_LENGTH];
        rc = cryptfs_SHA256_fileblock(crypto_blkdev.c_str(), hash_first_block);

        if (!rc &&
            memcmp(hash_first_block, crypt_ftr.hash_first_block, sizeof(hash_first_block)) != 0) {
            SLOGE("Checksums do not match - trigger wipe");
            rc = -1;
        }
    }

    if (!rc) {
        rc = cryptfs_enable_all_volumes(&crypt_ftr, crypto_blkdev.c_str(), real_blkdev.data(),
                                        previously_encrypted_upto);
    }

    /* Calculate checksum if we are not finished */
    if (!rc && crypt_ftr.encrypted_upto != crypt_ftr.fs_size) {
        rc = cryptfs_SHA256_fileblock(crypto_blkdev.c_str(), crypt_ftr.hash_first_block);
        if (rc) {
            SLOGE("Error calculating checksum for continuing encryption");
            rc = -1;
        }
    }

    /* Undo the dm-crypt mapping whether we succeed or not */
    delete_crypto_blk_dev(CRYPTO_BLOCK_DEVICE);

    if (!rc) {
        /* Success */
        crypt_ftr.flags &= ~CRYPT_INCONSISTENT_STATE;

        if (crypt_ftr.encrypted_upto != crypt_ftr.fs_size) {
            SLOGD("Encrypted up to sector %lld - will continue after reboot",
                  crypt_ftr.encrypted_upto);
            crypt_ftr.flags |= CRYPT_ENCRYPTION_IN_PROGRESS;
        }

        put_crypt_ftr_and_key(&crypt_ftr);

        if (crypt_ftr.encrypted_upto == crypt_ftr.fs_size) {
            char value[PROPERTY_VALUE_MAX];
            property_get("ro.crypto.state", value, "");
            if (!strcmp(value, "")) {
                /* default encryption - continue first boot sequence */
                property_set("ro.crypto.state", "encrypted");
                property_set("ro.crypto.type", "block");
                // wakeLock.reset(nullptr);
                if (rebootEncryption && crypt_ftr.crypt_type != CRYPT_TYPE_DEFAULT) {
                    // Bring up cryptkeeper that will check the password and set it
                    property_set("vold.decrypt", "trigger_shutdown_framework");
                    sleep(2);
                    property_set("vold.encrypt_progress", "");
                    cryptfs_trigger_restart_min_framework();
                } else {
                    cryptfs_check_passwd(DEFAULT_PASSWORD);
                    cryptfs_restart_internal(1);
                }
                return 0;
            } else {
                sleep(2); /* Give the UI a chance to show 100% progress */
                cryptfs_reboot(RebootType::reboot);
            }
        } else {
            sleep(2); /* Partially encrypted, ensure writes flushed to ssd */
            cryptfs_reboot(RebootType::shutdown);
        }
    } else {
        char value[PROPERTY_VALUE_MAX];

        property_get("ro.vold.wipe_on_crypt_fail", value, "0");
        if (!strcmp(value, "1")) {
            /* wipe data if encryption failed */
            SLOGE("encryption failed - rebooting into recovery to wipe data\n");
            std::string err;
            const std::vector<std::string> options = {
                "--wipe_data\n--reason=cryptfs_enable_internal\n"};
            if (!write_bootloader_message(options, &err)) {
                SLOGE("could not write bootloader message: %s", err.c_str());
            }
            cryptfs_reboot(RebootType::recovery);
        } else {
            /* set property to trigger dialog */
            property_set("vold.encrypt_progress", "error_partially_encrypted");
        }
        return -1;
    }

    /* hrm, the encrypt step claims success, but the reboot failed.
     * This should not happen.
     * Set the property and return.  Hope the framework can deal with it.
     */
    property_set("vold.encrypt_progress", "error_reboot_failed");
    return rc;

error_unencrypted:
    property_set("vold.encrypt_progress", "error_not_encrypted");
    return -1;

error_shutting_down:
    /* we failed, and have not encrypted anthing, so the users's data is still intact,
     * but the framework is stopped and not restarted to show the error, so it's up to
     * vold to restart the system.
     */
    SLOGE(
        "Error enabling encryption after framework is shutdown, no data changed, restarting "
        "system");
    cryptfs_reboot(RebootType::reboot);

    /* shouldn't get here */
    property_set("vold.encrypt_progress", "error_shutting_down");
    return -1;
}

int cryptfs_enable(int type, const char* passwd, int no_ui) {
    return cryptfs_enable_internal(type, passwd, no_ui);
}

int cryptfs_enable_default(int no_ui) {
    return cryptfs_enable_internal(CRYPT_TYPE_DEFAULT, DEFAULT_PASSWORD, no_ui);
}

int cryptfs_changepw(int crypt_type, const char* newpw) {
    if (fscrypt_is_native()) {
        SLOGE("cryptfs_changepw not valid for file encryption");
        return -1;
    }

    struct crypt_mnt_ftr crypt_ftr;
    int rc;

    /* This is only allowed after we've successfully decrypted the master key */
    if (!master_key_saved) {
        SLOGE("Key not saved, aborting");
        return -1;
    }

    if (crypt_type < 0 || crypt_type > CRYPT_TYPE_MAX_TYPE) {
        SLOGE("Invalid crypt_type %d", crypt_type);
        return -1;
    }

    /* get key */
    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        SLOGE("Error getting crypt footer and key");
        return -1;
    }

    crypt_ftr.crypt_type = crypt_type;

    rc = encrypt_master_key(crypt_type == CRYPT_TYPE_DEFAULT ? DEFAULT_PASSWORD : newpw,
                            crypt_ftr.salt, saved_master_key, crypt_ftr.master_key, &crypt_ftr);
    if (rc) {
        SLOGE("Encrypt master key failed: %d", rc);
        return -1;
    }
    /* save the key */
    put_crypt_ftr_and_key(&crypt_ftr);

    return 0;
}

static unsigned int persist_get_max_entries(int encrypted) {
    struct crypt_mnt_ftr crypt_ftr;
    unsigned int dsize;

    /* If encrypted, use the values from the crypt_ftr, otherwise
     * use the values for the current spec.
     */
    if (encrypted) {
        if (get_crypt_ftr_and_key(&crypt_ftr)) {
            /* Something is wrong, assume no space for entries */
            return 0;
        }
        dsize = crypt_ftr.persist_data_size;
    } else {
        dsize = CRYPT_PERSIST_DATA_SIZE;
    }

    if (dsize > sizeof(struct crypt_persist_data)) {
        return (dsize - sizeof(struct crypt_persist_data)) / sizeof(struct crypt_persist_entry);
    } else {
        return 0;
    }
}

static int persist_get_key(const char* fieldname, char* value) {
    unsigned int i;

    if (persist_data == NULL) {
        return -1;
    }
    for (i = 0; i < persist_data->persist_valid_entries; i++) {
        if (!strncmp(persist_data->persist_entry[i].key, fieldname, PROPERTY_KEY_MAX)) {
            /* We found it! */
            strlcpy(value, persist_data->persist_entry[i].val, PROPERTY_VALUE_MAX);
            return 0;
        }
    }

    return -1;
}

static int persist_set_key(const char* fieldname, const char* value, int encrypted) {
    unsigned int i;
    unsigned int num;
    unsigned int max_persistent_entries;

    if (persist_data == NULL) {
        return -1;
    }

    max_persistent_entries = persist_get_max_entries(encrypted);

    num = persist_data->persist_valid_entries;

    for (i = 0; i < num; i++) {
        if (!strncmp(persist_data->persist_entry[i].key, fieldname, PROPERTY_KEY_MAX)) {
            /* We found an existing entry, update it! */
            memset(persist_data->persist_entry[i].val, 0, PROPERTY_VALUE_MAX);
            strlcpy(persist_data->persist_entry[i].val, value, PROPERTY_VALUE_MAX);
            return 0;
        }
    }

    /* We didn't find it, add it to the end, if there is room */
    if (persist_data->persist_valid_entries < max_persistent_entries) {
        memset(&persist_data->persist_entry[num], 0, sizeof(struct crypt_persist_entry));
        strlcpy(persist_data->persist_entry[num].key, fieldname, PROPERTY_KEY_MAX);
        strlcpy(persist_data->persist_entry[num].val, value, PROPERTY_VALUE_MAX);
        persist_data->persist_valid_entries++;
        return 0;
    }

    return -1;
}

/**
 * Test if key is part of the multi-entry (field, index) sequence. Return non-zero if key is in the
 * sequence and its index is greater than or equal to index. Return 0 otherwise.
 */
int match_multi_entry(const char* key, const char* field, unsigned index) {
    std::string key_ = key;
    std::string field_ = field;

    std::string parsed_field;
    unsigned parsed_index;

    std::string::size_type split = key_.find_last_of('_');
    if (split == std::string::npos) {
        parsed_field = key_;
        parsed_index = 0;
    } else {
        parsed_field = key_.substr(0, split);
        parsed_index = std::stoi(key_.substr(split + 1));
    }

    return parsed_field == field_ && parsed_index >= index;
}

/*
 * Delete entry/entries from persist_data. If the entries are part of a multi-segment field, all
 * remaining entries starting from index will be deleted.
 * returns PERSIST_DEL_KEY_OK if deletion succeeds,
 * PERSIST_DEL_KEY_ERROR_NO_FIELD if the field does not exist,
 * and PERSIST_DEL_KEY_ERROR_OTHER if error occurs.
 *
 */
static int persist_del_keys(const char* fieldname, unsigned index) {
    unsigned int i;
    unsigned int j;
    unsigned int num;

    if (persist_data == NULL) {
        return PERSIST_DEL_KEY_ERROR_OTHER;
    }

    num = persist_data->persist_valid_entries;

    j = 0;  // points to the end of non-deleted entries.
    // Filter out to-be-deleted entries in place.
    for (i = 0; i < num; i++) {
        if (!match_multi_entry(persist_data->persist_entry[i].key, fieldname, index)) {
            persist_data->persist_entry[j] = persist_data->persist_entry[i];
            j++;
        }
    }

    if (j < num) {
        persist_data->persist_valid_entries = j;
        // Zeroise the remaining entries
        memset(&persist_data->persist_entry[j], 0, (num - j) * sizeof(struct crypt_persist_entry));
        return PERSIST_DEL_KEY_OK;
    } else {
        // Did not find an entry matching the given fieldname
        return PERSIST_DEL_KEY_ERROR_NO_FIELD;
    }
}

static int persist_count_keys(const char* fieldname) {
    unsigned int i;
    unsigned int count;

    if (persist_data == NULL) {
        return -1;
    }

    count = 0;
    for (i = 0; i < persist_data->persist_valid_entries; i++) {
        if (match_multi_entry(persist_data->persist_entry[i].key, fieldname, 0)) {
            count++;
        }
    }

    return count;
}

/* Return the value of the specified field. */
int cryptfs_getfield(const char* fieldname, char* value, int len) {
    if (fscrypt_is_native()) {
        SLOGE("Cannot get field when file encrypted");
        return -1;
    }

    char temp_value[PROPERTY_VALUE_MAX];
    /* CRYPTO_GETFIELD_OK is success,
     * CRYPTO_GETFIELD_ERROR_NO_FIELD is value not set,
     * CRYPTO_GETFIELD_ERROR_BUF_TOO_SMALL is buffer (as given by len) too small,
     * CRYPTO_GETFIELD_ERROR_OTHER is any other error
     */
    int rc = CRYPTO_GETFIELD_ERROR_OTHER;
    int i;
    char temp_field[PROPERTY_KEY_MAX];

    if (persist_data == NULL) {
        load_persistent_data();
        if (persist_data == NULL) {
            SLOGE("Getfield error, cannot load persistent data");
            goto out;
        }
    }

    // Read value from persistent entries. If the original value is split into multiple entries,
    // stitch them back together.
    if (!persist_get_key(fieldname, temp_value)) {
        // We found it, copy it to the caller's buffer and keep going until all entries are read.
        if (strlcpy(value, temp_value, len) >= (unsigned)len) {
            // value too small
            rc = CRYPTO_GETFIELD_ERROR_BUF_TOO_SMALL;
            goto out;
        }
        rc = CRYPTO_GETFIELD_OK;

        for (i = 1; /* break explicitly */; i++) {
            if (snprintf(temp_field, sizeof(temp_field), "%s_%d", fieldname, i) >=
                (int)sizeof(temp_field)) {
                // If the fieldname is very long, we stop as soon as it begins to overflow the
                // maximum field length. At this point we have in fact fully read out the original
                // value because cryptfs_setfield would not allow fields with longer names to be
                // written in the first place.
                break;
            }
            if (!persist_get_key(temp_field, temp_value)) {
                if (strlcat(value, temp_value, len) >= (unsigned)len) {
                    // value too small.
                    rc = CRYPTO_GETFIELD_ERROR_BUF_TOO_SMALL;
                    goto out;
                }
            } else {
                // Exhaust all entries.
                break;
            }
        }
    } else {
        /* Sadness, it's not there.  Return the error */
        rc = CRYPTO_GETFIELD_ERROR_NO_FIELD;
    }

out:
    return rc;
}

/* Set the value of the specified field. */
int cryptfs_setfield(const char* fieldname, const char* value) {
    if (fscrypt_is_native()) {
        SLOGE("Cannot set field when file encrypted");
        return -1;
    }

    char encrypted_state[PROPERTY_VALUE_MAX];
    /* 0 is success, negative values are error */
    int rc = CRYPTO_SETFIELD_ERROR_OTHER;
    int encrypted = 0;
    unsigned int field_id;
    char temp_field[PROPERTY_KEY_MAX];
    unsigned int num_entries;
    unsigned int max_keylen;

    if (persist_data == NULL) {
        load_persistent_data();
        if (persist_data == NULL) {
            SLOGE("Setfield error, cannot load persistent data");
            goto out;
        }
    }

    property_get("ro.crypto.state", encrypted_state, "");
    if (!strcmp(encrypted_state, "encrypted")) {
        encrypted = 1;
    }

    // Compute the number of entries required to store value, each entry can store up to
    // (PROPERTY_VALUE_MAX - 1) chars
    if (strlen(value) == 0) {
        // Empty value also needs one entry to store.
        num_entries = 1;
    } else {
        num_entries = (strlen(value) + (PROPERTY_VALUE_MAX - 1) - 1) / (PROPERTY_VALUE_MAX - 1);
    }

    max_keylen = strlen(fieldname);
    if (num_entries > 1) {
        // Need an extra "_%d" suffix.
        max_keylen += 1 + log10(num_entries);
    }
    if (max_keylen > PROPERTY_KEY_MAX - 1) {
        rc = CRYPTO_SETFIELD_ERROR_FIELD_TOO_LONG;
        goto out;
    }

    // Make sure we have enough space to write the new value
    if (persist_data->persist_valid_entries + num_entries - persist_count_keys(fieldname) >
        persist_get_max_entries(encrypted)) {
        rc = CRYPTO_SETFIELD_ERROR_VALUE_TOO_LONG;
        goto out;
    }

    // Now that we know persist_data has enough space for value, let's delete the old field first
    // to make up space.
    persist_del_keys(fieldname, 0);

    if (persist_set_key(fieldname, value, encrypted)) {
        // fail to set key, should not happen as we have already checked the available space
        SLOGE("persist_set_key() error during setfield()");
        goto out;
    }

    for (field_id = 1; field_id < num_entries; field_id++) {
        snprintf(temp_field, sizeof(temp_field), "%s_%u", fieldname, field_id);

        if (persist_set_key(temp_field, value + field_id * (PROPERTY_VALUE_MAX - 1), encrypted)) {
            // fail to set key, should not happen as we have already checked the available space.
            SLOGE("persist_set_key() error during setfield()");
            goto out;
        }
    }

    /* If we are running encrypted, save the persistent data now */
    if (encrypted) {
        if (save_persistent_data()) {
            SLOGE("Setfield error, cannot save persistent data");
            goto out;
        }
    }

    rc = CRYPTO_SETFIELD_OK;

out:
    return rc;
}

/* Checks userdata. Attempt to mount the volume if default-
 * encrypted.
 * On success trigger next init phase and return 0.
 * Currently do not handle failure - see TODO below.
 */
int cryptfs_mount_default_encrypted(void) {
    int crypt_type = cryptfs_get_password_type();
    if (crypt_type < 0 || crypt_type > CRYPT_TYPE_MAX_TYPE) {
        SLOGE("Bad crypt type - error");
    } else if (crypt_type != CRYPT_TYPE_DEFAULT) {
        SLOGD(
            "Password is not default - "
            "starting min framework to prompt");
        property_set("vold.decrypt", "trigger_restart_min_framework");
        return 0;
    } else if (cryptfs_check_passwd(DEFAULT_PASSWORD) == 0) {
        SLOGD("Password is default - restarting filesystem");
        cryptfs_restart_internal(0);
        return 0;
    } else {
        SLOGE("Encrypted, default crypt type but can't decrypt");
    }

    /** Corrupt. Allow us to boot into framework, which will detect bad
        crypto when it calls do_crypto_complete, then do a factory reset
     */
    property_set("vold.decrypt", "trigger_restart_min_framework");
    return 0;
}

/* Returns type of the password, default, pattern, pin or password.
 */
int cryptfs_get_password_type(void) {
    if (fscrypt_is_native()) {
        SLOGE("cryptfs_get_password_type not valid for file encryption");
        return -1;
    }

    struct crypt_mnt_ftr crypt_ftr;

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        SLOGE("Error getting crypt footer and key\n");
        return -1;
    }

    if (crypt_ftr.flags & CRYPT_INCONSISTENT_STATE) {
        return -1;
    }

    return crypt_ftr.crypt_type;
}

const char* cryptfs_get_password() {
    if (fscrypt_is_native()) {
        SLOGE("cryptfs_get_password not valid for file encryption");
        return 0;
    }

    struct timespec now;
    clock_gettime(CLOCK_BOOTTIME, &now);
    if (now.tv_sec < password_expiry_time) {
        return password;
    } else {
        cryptfs_clear_password();
        return 0;
    }
}

void cryptfs_clear_password() {
    if (password) {
        size_t len = strlen(password);
        memset(password, 0, len);
        free(password);
        password = 0;
        password_expiry_time = 0;
    }
}

int cryptfs_isConvertibleToFBE() {
    auto entry = GetEntryForMountPoint(&fstab_default, DATA_MNT_POINT);
    return entry && entry->fs_mgr_flags.force_fde_or_fbe;
}
