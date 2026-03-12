#include "security/password_hash.h"

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>

namespace security {
namespace {
std::string sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::setw(2) << static_cast<int>(digest[i]);
    }
    return oss.str();
}
} // namespace

bool hash_password(
    const std::string& password,
    std::string& password_hash,
    std::string& error_message
) {
    (void)error_message;
    password_hash = sha256_hex(password);
    return true;
}

bool verify_password(
    const std::string& password,
    const std::string& password_hash,
    std::string& error_message
) {
    (void)error_message;
    return sha256_hex(password) == password_hash;
}
} // namespace security
