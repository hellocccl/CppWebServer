#ifndef MYSQL_USER_STORE_H
#define MYSQL_USER_STORE_H

#include <string>

namespace db {
// 统一封装 MySQL 连接参数，便于服务器层按需传入配置。
struct MySqlConfig {
    std::string host;
    unsigned int port;
    std::string user;
    std::string password;
    std::string database;
};

// 初始化数据库与 users 表（不存在则自动创建）。
bool init_database(const MySqlConfig& config, std::string& error_message);

// 用户注册：插入 users(username, password_hash)。
bool register_user(
    const MySqlConfig& config,
    const std::string& username,
    const std::string& password,
    std::string& error_message
);

// 用户登录：读取 password_hash 并校验 username/password 是否匹配。
bool verify_user(
    const MySqlConfig& config,
    const std::string& username,
    const std::string& password,
    std::string& error_message
);
} // namespace db

#endif
