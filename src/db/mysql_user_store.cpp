#include "db/mysql_user_store.h"
#include "security/password_hash.h"

#include <mysql/mysql.h>

#include <vector>

namespace db {
namespace {
MYSQL* connect_mysql(const MySqlConfig& config, const char* db_name, std::string& error_message) {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        error_message = "mysql_init 失败";
        return nullptr;
    }

    if (!mysql_real_connect(
            conn,
            config.host.c_str(),
            config.user.c_str(),
            config.password.c_str(),
            db_name,
            config.port,
            nullptr,
            0)) {
        error_message = mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }

    mysql_query(conn, "SET NAMES utf8mb4");
    return conn;
}

std::string escape_mysql_string(MYSQL* conn, const std::string& raw) {
    std::vector<char> escaped(raw.size() * 2 + 1, 0);
    unsigned long len = mysql_real_escape_string(
        conn,
        escaped.data(),
        raw.c_str(),
        static_cast<unsigned long>(raw.size())
    );
    return std::string(escaped.data(), len);
}
} // namespace

bool init_database(const MySqlConfig& config, std::string& error_message) {
    MYSQL* conn = connect_mysql(config, nullptr, error_message);
    if (!conn) {
        return false;
    }

    std::string create_db_sql =
        "CREATE DATABASE IF NOT EXISTS `" + config.database + "` DEFAULT CHARACTER SET utf8mb4";
    if (mysql_query(conn, create_db_sql.c_str()) != 0) {
        error_message = "创建数据库失败: " + std::string(mysql_error(conn));
        mysql_close(conn);
        return false;
    }

    if (mysql_select_db(conn, config.database.c_str()) != 0) {
        error_message = "切换数据库失败: " + std::string(mysql_error(conn));
        mysql_close(conn);
        return false;
    }

    const char* create_table_sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "id INT PRIMARY KEY AUTO_INCREMENT,"
        "username VARCHAR(64) NOT NULL UNIQUE,"
        "password_hash VARCHAR(64) NOT NULL,"
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (mysql_query(conn, create_table_sql) != 0) {
        error_message = "创建 users 表失败: " + std::string(mysql_error(conn));
        mysql_close(conn);
        return false;
    }

    mysql_close(conn);
    return true;
}

bool register_user(
    const MySqlConfig& config,
    const std::string& username,
    const std::string& password,
    std::string& error_message
) {
    MYSQL* conn = connect_mysql(config, config.database.c_str(), error_message);
    if (!conn) {
        error_message = "数据库连接失败: " + error_message;
        return false;
    }

    std::string escaped_username = escape_mysql_string(conn, username);
    std::string password_hash;
    if (!security::hash_password(password, password_hash, error_message)) {
        mysql_close(conn);
        return false;
    }
    std::string escaped_password_hash = escape_mysql_string(conn, password_hash);

    std::string sql =
        "INSERT INTO users (username, password_hash) VALUES ('" + escaped_username + "', '" + escaped_password_hash + "')";

    if (mysql_query(conn, sql.c_str()) != 0) {
        unsigned int err_no = mysql_errno(conn);
        if (err_no == 1062) {
            error_message = "用户名已存在";
        } else {
            error_message = "数据库写入失败: " + std::string(mysql_error(conn));
        }
        mysql_close(conn);
        return false;
    }

    mysql_close(conn);
    return true;
}

bool verify_user(
    const MySqlConfig& config,
    const std::string& username,
    const std::string& password,
    std::string& error_message
) {
    MYSQL* conn = connect_mysql(config, config.database.c_str(), error_message);
    if (!conn) {
        error_message = "数据库连接失败: " + error_message;
        return false;
    }

    std::string escaped_username = escape_mysql_string(conn, username);
    std::string sql =
        "SELECT password_hash FROM users WHERE username = '" + escaped_username + "' LIMIT 1";

    if (mysql_query(conn, sql.c_str()) != 0) {
        error_message = "数据库查询失败: " + std::string(mysql_error(conn));
        mysql_close(conn);
        return false;
    }

    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) {
        error_message = "数据库结果读取失败: " + std::string(mysql_error(conn));
        mysql_close(conn);
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row || !row[0]) {
        error_message = "用户不存在";
        mysql_free_result(result);
        mysql_close(conn);
        return false;
    }

    std::string stored_password_hash = row[0];
    mysql_free_result(result);
    mysql_close(conn);

    if (!security::verify_password(password, stored_password_hash, error_message)) {
        if (error_message.empty()) {
            error_message = "密码错误";
        }
        return false;
    }

    return true;
}
} // namespace db
