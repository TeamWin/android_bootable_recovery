#include <map>
// Store main DE/CE policy
extern std::map<userid_t, EncryptionPolicy> s_de_policies;
extern std::map<userid_t, EncryptionPolicy> s_ce_policies;
extern std::string de_key_raw_ref;