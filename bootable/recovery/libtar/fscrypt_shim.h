#ifndef _FSCRYPT_SHIM_H
#define _FSCRYPT_SHIM_H

#include <linux/fscrypt.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* 
 * Shim for older TWRP libtar code expecting 'fscrypt_policy' helpers.
 * Modern kernel headers use 'struct fscrypt_policy_v1' and 'struct fscrypt_policy_v2'.
 * We alias to v1 as that is what this legacy libtar code expects.
 */
typedef struct fscrypt_policy_v1 fscrypt_policy;

static inline void* get_policy(fscrypt_policy *p) {
    return p;
}

static inline size_t fscrypt_policy_size(fscrypt_policy *p) {
    (void)p;
    return sizeof(struct fscrypt_policy_v1);
}

static inline void* get_policy_descriptor(fscrypt_policy *p) {
    return p ? (void*)p->master_key_descriptor : NULL;
}

static inline void get_policy_content(fscrypt_policy *p, char *content) {
    if (p) {
        snprintf(content, 50, "v1: %02x%02x%02x%02x%02x%02x%02x%02x",
                p->master_key_descriptor[0], p->master_key_descriptor[1],
                p->master_key_descriptor[2], p->master_key_descriptor[3],
                p->master_key_descriptor[4], p->master_key_descriptor[5],
                p->master_key_descriptor[6], p->master_key_descriptor[7]);
    } else {
        snprintf(content, 50, "null");
    }
}

#endif // _FSCRYPT_SHIM_H