#include "server.h"
#include "http_request.h"
#include "logger.h"

#include <mysql/mysql.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
// 按用户要求固定数据库连接参数，方便在一个位置集中修改。
const char* kDbHost = "127.0.0.1";
const unsigned int kDbPort = 3306;
const char* kDbUser = "root";
const char* kDbPassword = "123456789";
const char* kDbName = "mydb";

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string strip_query_and_fragment(std::string path) {
    size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    size_t fragment_pos = path.find('#');
    if (fragment_pos != std::string::npos) {
        path = path.substr(0, fragment_pos);
    }

    return path;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string url_decode(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+' ) {
            output.push_back(' ');
            continue;
        }

        if (input[i] == '%' && i + 2 < input.size()) {
            int hi = hex_value(input[i + 1]);
            int lo = hex_value(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                output.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        output.push_back(input[i]);
    }

    return output;
}

std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string& body) {
    std::unordered_map<std::string, std::string> params;

    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        std::string pair = (end == std::string::npos) ? body.substr(start) : body.substr(start, end - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string key = (eq == std::string::npos) ? pair : pair.substr(0, eq);
            std::string value = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
            key = url_decode(key);
            value = url_decode(value);
            if (!key.empty()) {
                params[key] = value;
            }
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return params;
}

std::string build_html_message(const std::string& title, const std::string& message) {
    return "<html>"
           "<body>"
           "<h1>" + title + "</h1>"
           "<p>" + message + "</p>"
           "<p><a href=\"/\">返回首页</a></p>"
           "</body>"
           "</html>";
}

MYSQL* create_mysql_connection(std::string& error_message) {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        error_message = "mysql_init 失败";
        return nullptr;
    }

    if (!mysql_real_connect(conn, kDbHost, kDbUser, kDbPassword, kDbName, kDbPort, nullptr, 0)) {
        error_message = mysql_error(conn);
        mysql_close(conn);
        return nullptr;
    }

    // 统一使用 utf8mb4，避免中文字段乱码。
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

bool send_all(int fd, const std::string& data, size_t& sent_bytes) {
    sent_bytes = 0;
    while (sent_bytes < data.size()) {
        ssize_t n = send(fd, data.data() + sent_bytes, data.size() - sent_bytes, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent_bytes += static_cast<size_t>(n);
    }
    return true;
}
} // namespace

Server::Server(int port, int thread_count, const std::string& www_root)
    : port_(port), server_fd_(-1), epfd_(-1), pool_(thread_count), www_root_(www_root) {}

bool Server::set_nonblocking(int fd) {
    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags == -1) {
        return false;
    }

    if (fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) == -1) {
        return false;
    }

    return true;
}

bool Server::set_blocking(int fd) {
    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags == -1) {
        return false;
    }

    if (fcntl(fd, F_SETFL, old_flags & ~O_NONBLOCK) == -1) {
        return false;
    }

    return true;
}

bool Server::init() {
    Logger::instance().info("服务器准备启动");

    // 1. 创建 socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        Logger::instance().error("创建 socket 失败: " + std::string(strerror(errno)));
        return false;
    }

    // 允许端口复用
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        Logger::instance().error("setsockopt SO_REUSEADDR 失败: " + std::string(strerror(errno)));
        return false;
    }

    // 设置监听 socket 为非阻塞
    if (!set_nonblocking(server_fd_)) {
        Logger::instance().error("设置 server_fd 非阻塞失败");
        return false;
    }

    // 2. 准备地址
    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    // 3. bind
    if (bind(server_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        Logger::instance().error("bind 失败: " + std::string(strerror(errno)));
        return false;
    }

    // 4. listen
    if (listen(server_fd_, 10) == -1) {
        Logger::instance().error("listen 失败: " + std::string(strerror(errno)));
        return false;
    }

    Logger::instance().info("服务器启动成功，监听端口 = " + std::to_string(port_));

    // 5. 创建 epoll
    epfd_ = epoll_create1(0);
    if (epfd_ == -1) {
        Logger::instance().error("epoll_create1 失败: " + std::string(strerror(errno)));
        return false;
    }

    Logger::instance().info("epoll 实例创建成功");

    // 6. 把监听 fd 加入 epoll
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd_;

    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &ev) == -1) {
        Logger::instance().error("epoll_ctl ADD server_fd 失败: " + std::string(strerror(errno)));
        return false;
    }

    // 启动时完成数据库和用户表初始化，避免运行期间才暴露配置错误。
    if (!init_database()) {
        Logger::instance().error("数据库初始化失败");
        return false;
    }

    Logger::instance().info("已将监听 fd 加入 epoll, server_fd = " + std::to_string(server_fd_));
    return true;
}

bool Server::read_file(const std::string& filename, std::string& content, bool binary) {
    Logger::instance().debug("尝试读取文件: " + filename);

    std::ios::openmode mode = std::ios::in;
    if (binary) {
        mode |= std::ios::binary;
    }

    std::ifstream ifs(filename.c_str(), mode);
    if (!ifs.is_open()) {
        Logger::instance().error("无法打开文件: " + filename);
        return false;
    }

    std::ostringstream buffer;
    buffer << ifs.rdbuf();
    content = buffer.str();
    return true;
}

bool Server::resolve_static_path(const std::string& url_path, std::string& file_path) const {
    std::string clean_path = url_decode(strip_query_and_fragment(url_path));
    if (clean_path.empty()) {
        clean_path = "/";
    }

    // 保持原有 /hello 路径兼容，同时把其它路径作为静态文件直接映射。
    if (clean_path == "/") {
        clean_path = "/index.html";
    } else if (clean_path == "/hello") {
        clean_path = "/hello.html";
    }

    std::replace(clean_path.begin(), clean_path.end(), '\\', '/');

    // 阻断目录穿越，避免访问 www_root_ 之外的文件。
    if (clean_path.empty() || clean_path[0] != '/' || clean_path.find("..") != std::string::npos) {
        return false;
    }

    file_path = www_root_ + clean_path;
    return true;
}

std::string Server::content_type_from_path(const std::string& file_path) const {
    size_t pos = file_path.find_last_of('.');
    if (pos == std::string::npos) {
        return "application/octet-stream";
    }

    std::string ext = to_lower_copy(file_path.substr(pos));
    if (ext == ".html" || ext == ".htm") {
        return "text/html; charset=UTF-8";
    }
    if (ext == ".txt") {
        return "text/plain; charset=UTF-8";
    }
    if (ext == ".css") {
        return "text/css; charset=UTF-8";
    }
    if (ext == ".js") {
        return "application/javascript; charset=UTF-8";
    }
    if (ext == ".json") {
        return "application/json; charset=UTF-8";
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        return "image/jpeg";
    }
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".gif") {
        return "image/gif";
    }
    if (ext == ".bmp") {
        return "image/bmp";
    }
    if (ext == ".webp") {
        return "image/webp";
    }
    if (ext == ".svg") {
        return "image/svg+xml";
    }
    if (ext == ".ico") {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

bool Server::init_database() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        Logger::instance().error("mysql_init 失败");
        return false;
    }

    if (!mysql_real_connect(conn, kDbHost, kDbUser, kDbPassword, nullptr, kDbPort, nullptr, 0)) {
        Logger::instance().error("连接 MySQL 失败: " + std::string(mysql_error(conn)));
        mysql_close(conn);
        return false;
    }

    mysql_query(conn, "SET NAMES utf8mb4");

    std::string create_db_sql =
        "CREATE DATABASE IF NOT EXISTS `" + std::string(kDbName) + "` DEFAULT CHARACTER SET utf8mb4";
    if (mysql_query(conn, create_db_sql.c_str()) != 0) {
        Logger::instance().error("创建数据库失败: " + std::string(mysql_error(conn)));
        mysql_close(conn);
        return false;
    }

    if (mysql_select_db(conn, kDbName) != 0) {
        Logger::instance().error("切换数据库失败: " + std::string(mysql_error(conn)));
        mysql_close(conn);
        return false;
    }

    const char* create_table_sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "id INT PRIMARY KEY AUTO_INCREMENT,"
        "username VARCHAR(64) NOT NULL UNIQUE,"
        "passwd VARCHAR(128) NOT NULL,"
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (mysql_query(conn, create_table_sql) != 0) {
        Logger::instance().error("创建 users 表失败: " + std::string(mysql_error(conn)));
        mysql_close(conn);
        return false;
    }

    mysql_close(conn);
    Logger::instance().info("MySQL 初始化成功，数据库 = " + std::string(kDbName));
    return true;
}

bool Server::register_user(const std::string& username, const std::string& password, std::string& error_message) {
    std::string db_error;
    MYSQL* conn = create_mysql_connection(db_error);
    if (!conn) {
        error_message = "数据库连接失败: " + db_error;
        return false;
    }

    std::string escaped_username = escape_mysql_string(conn, username);
    std::string escaped_password = escape_mysql_string(conn, password);

    std::string sql =
        "INSERT INTO users (username, passwd) VALUES ('" + escaped_username + "', '" + escaped_password + "')";

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

bool Server::verify_user(const std::string& username, const std::string& password, std::string& error_message) {
    std::string db_error;
    MYSQL* conn = create_mysql_connection(db_error);
    if (!conn) {
        error_message = "数据库连接失败: " + db_error;
        return false;
    }

    std::string escaped_username = escape_mysql_string(conn, username);
    std::string sql =
        "SELECT passwd FROM users WHERE username = '" + escaped_username + "' LIMIT 1";

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

    std::string stored_password = row[0];
    mysql_free_result(result);
    mysql_close(conn);

    if (stored_password != password) {
        error_message = "密码错误";
        return false;
    }

    return true;
}

bool Server::read_http_request(int client_fd, std::string& raw_request) {
    const size_t MAX_REQUEST_SIZE = 1024 * 1024; // 1MB 上限
    raw_request.clear();

    size_t header_end = std::string::npos;
    size_t expected_total = 0;

    while (true) {
        char buf[1024] = {0};
        int n = recv(client_fd, buf, sizeof(buf), 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        raw_request.append(buf, n);
        if (raw_request.size() > MAX_REQUEST_SIZE) {
            Logger::instance().error("请求过大，超过上限");
            return false;
        }

        if (header_end == std::string::npos) {
            header_end = raw_request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                expected_total = header_end + 4;
                std::string headers = raw_request.substr(0, header_end);
                std::string headers_lower = headers;
                for (char& c : headers_lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                size_t pos = headers_lower.find("content-length:");
                if (pos != std::string::npos) {
                    size_t line_end = headers_lower.find("\r\n", pos);
                    std::string line = headers_lower.substr(pos, line_end - pos);
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string value = line.substr(colon + 1);
                        std::stringstream ss(value);
                        size_t len = 0;
                        ss >> len;
                        expected_total += len;
                    }
                }
            }
        }

        if (header_end != std::string::npos && raw_request.size() >= expected_total) {
            return true;
        }
    }
}

void Server::handle_client_impl(int client_fd) {
    if (!set_blocking(client_fd)) {
        Logger::instance().error("设置 client_fd 阻塞失败, fd = " + std::to_string(client_fd));
        close(client_fd);
        return;
    }

    std::string raw_request;
    if (!read_http_request(client_fd, raw_request)) {
        Logger::instance().error("读取请求失败或客户端已关闭连接, fd = " + std::to_string(client_fd));
        close(client_fd);
        {
            std::lock_guard<std::mutex> lock(conn_mtx_);
            last_active_.erase(client_fd);
        }
        return;
    }

    HttpRequest request;
    if (!request.parse(raw_request)) {
        Logger::instance().error("HTTP 请求解析失败, fd = " + std::to_string(client_fd));
        close(client_fd);
        return;
    }

    std::string request_path = url_decode(strip_query_and_fragment(request.path()));
    Logger::instance().info(
        "解析请求成功, fd = " + std::to_string(client_fd) +
        ", method = " + request.method() +
        ", path = " + request_path +
        ", version = " + request.version()
    );

    std::string file_path;
    std::string status_line;
    std::string status_text;
    std::string body;
    std::string content_type = "text/html; charset=UTF-8";

    if (request.method() == "GET") {
        if (!resolve_static_path(request_path, file_path)) {
            status_line = "HTTP/1.1 400 Bad Request\r\n";
            status_text = "400 Bad Request";
            body = build_html_message("400 Bad Request", "非法路径");
        } else if (read_file(file_path, body, true)) {
            status_line = "HTTP/1.1 200 OK\r\n";
            status_text = "200 OK";
            content_type = content_type_from_path(file_path);
        } else {
            file_path = www_root_ + "/404.html";
            status_line = "HTTP/1.1 404 Not Found\r\n";
            status_text = "404 Not Found";
            content_type = "text/html; charset=UTF-8";

            if (!read_file(file_path, body, false)) {
                status_line = "HTTP/1.1 500 Internal Server Error\r\n";
                status_text = "500 Internal Server Error";
                body = build_html_message("500 Internal Server Error", "服务器读取页面文件失败");
            }
        }
    } else if (request.method() == "POST") {
        if (request_path == "/post") {
            status_line = "HTTP/1.1 200 OK\r\n";
            status_text = "200 OK";
            content_type = "text/plain; charset=UTF-8";
            body = "POST OK\n";
            body += request.body();
        } else if (request_path == "/register") {
            auto form = parse_form_urlencoded(request.body());
            std::string username = form["username"];
            std::string password = form["password"];

            if (username.empty() || password.empty()) {
                status_line = "HTTP/1.1 400 Bad Request\r\n";
                status_text = "400 Bad Request";
                body = build_html_message("注册失败", "username 或 password 不能为空");
            } else {
                std::string error_message;
                if (register_user(username, password, error_message)) {
                    status_line = "HTTP/1.1 200 OK\r\n";
                    status_text = "200 OK";
                    body = build_html_message("注册成功", "用户已写入 MySQL，可继续登录。");
                } else if (error_message == "用户名已存在") {
                    status_line = "HTTP/1.1 409 Conflict\r\n";
                    status_text = "409 Conflict";
                    body = build_html_message("注册失败", error_message);
                } else {
                    status_line = "HTTP/1.1 500 Internal Server Error\r\n";
                    status_text = "500 Internal Server Error";
                    body = build_html_message("注册失败", error_message);
                }
            }
        } else if (request_path == "/login") {
            auto form = parse_form_urlencoded(request.body());
            std::string username = form["username"];
            std::string password = form["password"];

            if (username.empty() || password.empty()) {
                status_line = "HTTP/1.1 400 Bad Request\r\n";
                status_text = "400 Bad Request";
                body = build_html_message("登录失败", "username 或 password 不能为空");
            } else {
                std::string error_message;
                if (verify_user(username, password, error_message)) {
                    status_line = "HTTP/1.1 200 OK\r\n";
                    status_text = "200 OK";
                    body = build_html_message("登录成功", "用户名和密码校验通过。");
                } else if (error_message == "用户不存在" || error_message == "密码错误") {
                    status_line = "HTTP/1.1 401 Unauthorized\r\n";
                    status_text = "401 Unauthorized";
                    body = build_html_message("登录失败", error_message);
                } else {
                    status_line = "HTTP/1.1 500 Internal Server Error\r\n";
                    status_text = "500 Internal Server Error";
                    body = build_html_message("登录失败", error_message);
                }
            }
        } else {
            file_path = www_root_ + "/404.html";
            status_line = "HTTP/1.1 404 Not Found\r\n";
            status_text = "404 Not Found";
            if (!read_file(file_path, body, false)) {
                status_line = "HTTP/1.1 500 Internal Server Error\r\n";
                status_text = "500 Internal Server Error";
                body = build_html_message("500 Internal Server Error", "服务器读取页面文件失败");
            }
        }
    } else {
        status_line = "HTTP/1.1 405 Method Not Allowed\r\n";
        status_text = "405 Method Not Allowed";
        content_type = "text/plain; charset=UTF-8";
        body = "Method Not Allowed";
    }

    bool keep_alive = false;
    std::string conn_header = to_lower_copy(request.header("Connection"));
    if (request.version() == "HTTP/1.1") {
        keep_alive = (conn_header != "close");
    } else if (request.version() == "HTTP/1.0") {
        keep_alive = (conn_header == "keep-alive");
    }

    std::string response =
        status_line +
        "Content-Type: " + content_type + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n"
        "\r\n" +
        body;

    size_t sent = 0;
    if (!send_all(client_fd, response, sent)) {
        Logger::instance().error(
            "send 失败, fd = " + std::to_string(client_fd) +
            ", errno = " + std::to_string(errno) +
            ", error = " + std::string(strerror(errno))
        );
    } else {
        Logger::instance().info(
            "响应发送成功, fd = " + std::to_string(client_fd) +
            ", status = " + status_text +
            ", bytes = " + std::to_string(sent)
        );
    }

    if (keep_alive && sent > 0) {
        if (!set_nonblocking(client_fd)) {
            Logger::instance().error("设置 client_fd 非阻塞失败, fd = " + std::to_string(client_fd));
            close(client_fd);
            {
                std::lock_guard<std::mutex> lock(conn_mtx_);
                last_active_.erase(client_fd);
            }
            return;
        }

        epoll_event client_ev;
        client_ev.events = EPOLLIN;
        client_ev.data.fd = client_fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
            Logger::instance().error(
                "epoll_ctl ADD client_fd 失败, fd = " + std::to_string(client_fd) +
                ", error = " + std::string(strerror(errno))
            );
            close(client_fd);
            {
                std::lock_guard<std::mutex> lock(conn_mtx_);
                last_active_.erase(client_fd);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(conn_mtx_);
            last_active_[client_fd] = std::time(nullptr);
        }
        Logger::instance().info("保持连接, fd = " + std::to_string(client_fd));
    } else {
        close(client_fd);
        {
            std::lock_guard<std::mutex> lock(conn_mtx_);
            last_active_.erase(client_fd);
        }
        Logger::instance().info("连接关闭, fd = " + std::to_string(client_fd));
    }
}

void Server::handle_client(Server* server, int client_fd) {
    server->handle_client_impl(client_fd);
}

void Server::check_timeout_connections() {
    time_t now = std::time(nullptr);
    const int TIMEOUT = 30; // 30秒超时

    for (auto it = last_active_.begin(); it != last_active_.end(); ) {
        int fd = it->first;
        time_t last_time = it->second;

        if (now - last_time > TIMEOUT) {
            Logger::instance().info(
                "连接超时，关闭 fd = " + std::to_string(fd)
            );
            epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            it = last_active_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::run() {
    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (true) {
        int nfds = epoll_wait(epfd_, events, MAX_EVENTS, 10000);
        if (nfds == -1) {
            if (errno == EINTR) {
                check_timeout_connections();
                Logger::instance().debug("epoll_wait 被信号中断，继续等待");
                continue;
            }
            Logger::instance().error("epoll_wait 失败: " + std::string(strerror(errno)));
            break;
        }

        Logger::instance().debug("epoll_wait 返回，就绪 fd 数量 = " + std::to_string(nfds));
         // 每轮都检查一下超时连接
        check_timeout_connections();

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            // 有新连接到来
            if (fd == server_fd_) {
                while (true) {
                    sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);

                    int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            Logger::instance().error("accept 失败: " + std::string(strerror(errno)));
                            break;
                        }
                    }
                    
                    char ip[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    int port = ntohs(client_addr.sin_port);

                    Logger::instance().info(
                        "有客户端连接, fd = " + std::to_string(client_fd) +
                        ", ip = " + std::string(ip) +
                        ", port = " + std::to_string(port)
                    );

                    if (!set_nonblocking(client_fd)) {
                        Logger::instance().error(
                            "设置 client_fd 非阻塞失败, fd = " + std::to_string(client_fd)
                        );
                        close(client_fd);
                        continue;
                    }

                    epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = client_fd;

                    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
                        Logger::instance().error(
                            "epoll_ctl ADD client_fd 失败, fd = " + std::to_string(client_fd) +
                            ", error = " + std::string(strerror(errno))
                        );
                        close(client_fd);
                        continue;
                    }
                    {
                        std::lock_guard<std::mutex> lock(conn_mtx_);
                        last_active_[client_fd] = std::time(nullptr);
                    }
                    Logger::instance().debug(
                        "客户端 fd 已加入 epoll, client_fd = " + std::to_string(client_fd)
                    );
                }
            }
            // 客户端可读
            else if (events[i].events & EPOLLIN) {
                int client_fd = fd;

                Logger::instance().debug(
                    "客户端 fd 可读，准备交给线程池处理, client_fd = " + std::to_string(client_fd)
                );
                {
                    std::lock_guard<std::mutex> lock(conn_mtx_);
                    last_active_[client_fd] = std::time(nullptr);
                }
                if (epoll_ctl(epfd_, EPOLL_CTL_DEL, client_fd, nullptr) == -1) {
                    Logger::instance().error(
                        "epoll_ctl DEL client_fd 失败, fd = " + std::to_string(client_fd) +
                        ", error = " + std::string(strerror(errno))
                    );
                    close(client_fd);
                    {
                        std::lock_guard<std::mutex> lock(conn_mtx_);
                        last_active_.erase(client_fd);
                    }   
                    continue;
                }
                
                pool_.enqueue([this, client_fd]() {
                    Server::handle_client(this, client_fd);
                });

                Logger::instance().debug(
                    "任务已加入线程池, client_fd = " + std::to_string(client_fd)
                );
            }
            // 其它异常
            else {
                Logger::instance().error(
                    "fd = " + std::to_string(fd) +
                    " 发生异常事件, events = " + std::to_string(events[i].events) +
                    "，关闭连接"
                );
                epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                {
                    std::lock_guard<std::mutex> lock(conn_mtx_);
                    last_active_.erase(fd);
                }   
            }
        }
    }
}

Server::~Server() {
    if (epfd_ != -1) {
        close(epfd_);
    }
    if (server_fd_ != -1) {
        close(server_fd_);
    }
}
