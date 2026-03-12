#ifndef PASSWORD_HASH_H
#define PASSWORD_HASH_H

#include <string>

namespace security {
// 生成密码哈希。当前实现为 SHA-256 十六进制字符串，便于学习和调试。
bool hash_password(
    const std::string& password,
    std::string& password_hash,
    std::string& error_message
);

// 校验明文密码是否与数据库中的 password_hash 匹配。
bool verify_password(
    const std::string& password,
    const std::string& password_hash,
    std::string& error_message
);
} // namespace security

#endif
