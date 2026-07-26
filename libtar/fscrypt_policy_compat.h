/*
 * fscrypt_policy_compat.h
 *
 * Compatibility shim for libtar's fscrypt policy backup/restore support.
 *
 * libtar's block.c, append.c, extract.c, and output.c call a set of
 * helper functions (get_policy, get_policy_descriptor, get_policy_size,
 * fscrypt_policy_size, get_policy_content) that historically wrapped the
 * raw kernel fscrypt policy struct so TWRP could preserve a directory's
 * FBE policy across tar backup/restore. That wrapper header no longer
 * exists upstream (AOSP's kernel uapi header only exposes the raw
 * struct fscrypt_policy_v1/v2 with no convenience helpers), and the
 * call sites were never fully reconciled with each other (block.c used
 * a 1-arg fscrypt_policy_size(), while append.c/extract.c used a 2-arg
 * get_policy_size() with a hex-vs-raw flag; some also hardcoded
 * fscrypt_policy_v1 even though this device's build actually defines
 * USE_FSCRYPT_POLICY_V2).
 *
 * This header restores all five functions with consistent, well-defined
 * semantics, generically supporting whichever policy version this
 * build's USE_FSCRYPT_POLICY_V1 flag selects - matching the same
 * convention system/vold/fscrypt_policy.h already uses:
 *
 *   struct fscrypt_policy_v1 { version, contents_encryption_mode,
 *     filenames_encryption_mode, flags, master_key_descriptor[8] }
 *   struct fscrypt_policy_v2 { version, contents_encryption_mode,
 *     filenames_encryption_mode, flags, __reserved[4],
 *     master_key_identifier[16] }
 */

#ifndef _LIBTAR_FSCRYPT_POLICY_COMPAT_H_
#define _LIBTAR_FSCRYPT_POLICY_COMPAT_H_

#include <linux/fscrypt.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libtar is built against whichever fscrypt policy version this device's
 * build defines (USE_FSCRYPT_POLICY_V1 selects v1's 8-byte
 * master_key_descriptor; otherwise v2's 16-byte master_key_identifier is
 * used, matching system/vold/fscrypt_policy.h's own convention). Define a
 * single generic type/field pair so libtar's code doesn't need to
 * hardcode either version.
 */
#ifdef USE_FSCRYPT_POLICY_V1
typedef struct fscrypt_policy_v1 libtar_fscrypt_policy_t;
#define LIBTAR_FSCRYPT_KEY_FIELD master_key_descriptor
#else
typedef struct fscrypt_policy_v2 libtar_fscrypt_policy_t;
#define LIBTAR_FSCRYPT_KEY_FIELD master_key_identifier
#endif

/* Returns a pointer to the raw bytes of the whole policy struct. */
static inline uint8_t* get_policy(libtar_fscrypt_policy_t *fep) {
	return (uint8_t*)fep;
}

/* Returns a pointer to the master key descriptor/identifier within the policy. */
static inline uint8_t* get_policy_descriptor(libtar_fscrypt_policy_t *fep) {
	return (uint8_t*)fep->LIBTAR_FSCRYPT_KEY_FIELD;
}

/*
 * Returns the size of the raw policy struct in bytes if hex is false,
 * or the size needed for its NUL-terminated hex string representation
 * if hex is true (2 hex chars per byte, plus 1 for the terminator).
 */
static inline size_t get_policy_size(libtar_fscrypt_policy_t *fep, bool hex) {
	size_t raw_size = sizeof(*fep);
	if (hex)
		return (raw_size * 2) + 1;
	return raw_size;
}

/* block.c's single-argument alias for get_policy_size(fep, false). */
static inline size_t fscrypt_policy_size(libtar_fscrypt_policy_t *fep) {
	return get_policy_size(fep, false);
}

/*
 * Writes a NUL-terminated hex-string representation of the policy's
 * master key descriptor/identifier into content. Caller must ensure
 * content is at least get_policy_size(fep, true) bytes.
 */
static inline void get_policy_content(libtar_fscrypt_policy_t *fep, char *content) {
	static const char hex_lookup[] = "0123456789abcdef";
	uint8_t *descriptor = get_policy_descriptor(fep);
	size_t descriptor_len = sizeof(fep->LIBTAR_FSCRYPT_KEY_FIELD);
	size_t i;
	for (i = 0; i < descriptor_len; i++) {
		content[i * 2]     = hex_lookup[(descriptor[i] >> 4) & 0xF];
		content[i * 2 + 1] = hex_lookup[descriptor[i] & 0xF];
	}
	content[descriptor_len * 2] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif // _LIBTAR_FSCRYPT_POLICY_COMPAT_H_
