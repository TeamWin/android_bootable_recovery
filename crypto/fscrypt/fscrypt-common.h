#include <map>

#define CRYPT_TYPE_DEFAULT  1

// Store main DE/CE policy
extern std::map<userid_t, android::fscrypt::EncryptionPolicy> s_de_policies;
extern std::map<userid_t, android::fscrypt::EncryptionPolicy> s_ce_policies;
extern std::string de_key_raw_ref;