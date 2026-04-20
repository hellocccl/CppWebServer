#include "server.h"
#include "http_request.h"
#include "logger.h"
#include "security/password_hash.h"

#include <mysql/mysql.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <netinet/tcp.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace {
// 按用户要求固定数据库连接参数，方便在一个位置集中修改。
const char* kDbHost = "localhost";
const unsigned int kDbPort = 3306;
const char* kDbUser = "root";
const char* kDbPassword = "123456789";
const char* kDbName = "mydb";
const size_t kMaxCachedFileBytes = 1024 * 1024;
const size_t kMaxPrebuiltResponseBytes = 128 * 1024;
const size_t kMaxBufferedRequestBytes = 1024 * 1024;

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

std::string build_response_buffer(
    const std::string& status_line,
    const std::string& content_type,
    const std::string& body,
    bool keep_alive
) {
    std::string response;
    response.reserve(status_line.size() + content_type.size() + body.size() + 96);
    response += status_line;
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: ";
    response += keep_alive ? "keep-alive\r\n" : "close\r\n";
    response += "\r\n";
    response += body;
    return response;
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

} // namespace

class Server::RequestHandler {
public:
    virtual ~RequestHandler() {}

    virtual HandlerResponse handle(
        Server& server,
        const HttpRequest& request,
        const std::string& request_path
    ) const = 0;

protected:
    static HandlerResponse make_html_response(
        const std::string& status_line,
        const std::string& status_text,
        const std::string& title,
        const std::string& message
    ) {
        HandlerResponse response;
        response.status_line = status_line;
        response.status_text = status_text;
        response.body = build_html_message(title, message);
        return response;
    }

    static HandlerResponse make_not_found_response(Server& server) {
        HandlerResponse response;
        response.status_line = "HTTP/1.1 404 Not Found\r\n";
        response.status_text = "404 Not Found";

        std::string file_path = server.www_root_ + "/404.html";
        std::shared_ptr<const Server::StaticFileCacheEntry> static_file = server.get_static_file(file_path);
        if (static_file) {
            response.content_type = static_file->content_type;
            response.static_file = static_file;
            return response;
        }

        response.status_line = "HTTP/1.1 500 Internal Server Error\r\n";
        response.status_text = "500 Internal Server Error";
        response.body = build_html_message("500 Internal Server Error", "服务器读取页面文件失败");
        return response;
    }
};

class Server::StaticGetHandler : public Server::RequestHandler {
public:
    HandlerResponse handle(
        Server& server,
        const HttpRequest& request,
        const std::string& request_path
    ) const override {
        (void)request;

        std::string file_path;
        if (!server.resolve_static_path(request_path, file_path)) {
            return make_html_response(
                "HTTP/1.1 400 Bad Request\r\n",
                "400 Bad Request",
                "400 Bad Request",
                "非法路径"
            );
        }

        std::shared_ptr<const Server::StaticFileCacheEntry> static_file = server.get_static_file(file_path);
        if (!static_file) {
            return make_not_found_response(server);
        }

        HandlerResponse response;
        response.status_line = "HTTP/1.1 200 OK\r\n";
        response.status_text = "200 OK";
        response.content_type = static_file->content_type;
        response.static_file = static_file;
        return response;
    }
};

class Server::PostEchoHandler : public Server::RequestHandler {
public:
    HandlerResponse handle(
        Server& server,
        const HttpRequest& request,
        const std::string& request_path
    ) const override {
        (void)server;
        (void)request_path;

        HandlerResponse response;
        response.status_line = "HTTP/1.1 200 OK\r\n";
        response.status_text = "200 OK";
        response.content_type = "text/plain; charset=UTF-8";
        response.body = "POST OK\n";
        response.body += request.body();
        return response;
    }
};

class Server::RegisterHandler : public Server::RequestHandler {
public:
    HandlerResponse handle(
        Server& server,
        const HttpRequest& request,
        const std::string& request_path
    ) const override {
        (void)request_path;

        std::unordered_map<std::string, std::string> form = parse_form_urlencoded(request.body());
        std::string username = form["username"];
        std::string password = form["password"];

        if (username.empty() || password.empty()) {
            return make_html_response(
                "HTTP/1.1 400 Bad Request\r\n",
                "400 Bad Request",
                "注册失败",
                "username 或 password 不能为空"
            );
        }

        std::string error_message;
        if (server.register_user(username, password, error_message)) {
            return make_html_response(
                "HTTP/1.1 200 OK\r\n",
                "200 OK",
                "注册成功",
                "用户已写入 MySQL，可继续登录。"
            );
        }
        if (error_message == "用户名已存在") {
            return make_html_response(
                "HTTP/1.1 409 Conflict\r\n",
                "409 Conflict",
                "注册失败",
                error_message
            );
        }

        return make_html_response(
            "HTTP/1.1 500 Internal Server Error\r\n",
            "500 Internal Server Error",
            "注册失败",
            error_message
        );
    }
};

class Server::LoginHandler : public Server::RequestHandler {
public:
    HandlerResponse handle(
        Server& server,
        const HttpRequest& request,
        const std::string& request_path
    ) const override {
        (void)request_path;

        std::unordered_map<std::string, std::string> form = parse_form_urlencoded(request.body());
        std::string username = form["username"];
        std::string password = form["password"];

        if (username.empty() || password.empty()) {
            return make_html_response(
                "HTTP/1.1 400 Bad Request\r\n",
                "400 Bad Request",
                "登录失败",
                "username 或 password 不能为空"
            );
        }

        std::string error_message;
        if (server.verify_user(username, password, error_message)) {
            return make_html_response(
                "HTTP/1.1 200 OK\r\n",
                "200 OK",
                "登录成功",
                "用户名和密码校验通过。"
            );
        }
        if (error_message == "用户不存在" || error_message == "密码错误") {
            return make_html_response(
                "HTTP/1.1 401 Unauthorized\r\n",
                "401 Unauthorized",
                "登录失败",
                error_message
            );
        }

        return make_html_response(
            "HTTP/1.1 500 Internal Server Error\r\n",
            "500 Internal Server Error",
            "登录失败",
            error_message
        );
    }
};

class Server::NotFoundHandler : public Server::RequestHandler {
public:
    HandlerResponse handle(
        Server& server,
        const HttpRequest& request,
        const std::string& request_path
    ) const override {
        (void)request;
        (void)request_path;
        return make_not_found_response(server);
    }
};

class Server::MethodNotAllowedHandler : public Server::RequestHandler {
public:
    HandlerResponse handle(
        Server& server,
        const HttpRequest& request,
        const std::string& request_path
    ) const override {
        (void)server;
        (void)request;
        (void)request_path;

        HandlerResponse response;
        response.status_line = "HTTP/1.1 405 Method Not Allowed\r\n";
        response.status_text = "405 Method Not Allowed";
        response.content_type = "text/plain; charset=UTF-8";
        response.body = "Method Not Allowed";
        return response;
    }
};

class Server::RequestHandlerFactory {
public:
    typedef std::function<std::unique_ptr<RequestHandler>()> Creator;

    RequestHandlerFactory() {
        register_method_handler("GET", []() {
            return std::unique_ptr<RequestHandler>(new StaticGetHandler());
        });
        register_route("POST", "/post", []() {
            return std::unique_ptr<RequestHandler>(new PostEchoHandler());
        });
        register_route("POST", "/register", []() {
            return std::unique_ptr<RequestHandler>(new RegisterHandler());
        });
        register_route("POST", "/login", []() {
            return std::unique_ptr<RequestHandler>(new LoginHandler());
        });
        register_method_handler("POST", []() {
            return std::unique_ptr<RequestHandler>(new NotFoundHandler());
        });
        default_creator_ = []() {
            return std::unique_ptr<RequestHandler>(new MethodNotAllowedHandler());
        };
    }

    std::unique_ptr<RequestHandler> create(
        const HttpRequest& request,
        const std::string& request_path
    ) const {
        std::unordered_map<std::string, Creator>::const_iterator route_it =
            route_creators_.find(route_key(request.method(), request_path));
        if (route_it != route_creators_.end()) {
            return route_it->second();
        }

        std::unordered_map<std::string, Creator>::const_iterator method_it =
            method_creators_.find(request.method());
        if (method_it != method_creators_.end()) {
            return method_it->second();
        }

        return default_creator_();
    }

private:
    std::unordered_map<std::string, Creator> route_creators_;
    std::unordered_map<std::string, Creator> method_creators_;
    Creator default_creator_;

    static std::string route_key(const std::string& method, const std::string& path) {
        return method + " " + path;
    }

    void register_route(const std::string& method, const std::string& path, const Creator& creator) {
        route_creators_[route_key(method, path)] = creator;
    }

    void register_method_handler(const std::string& method, const Creator& creator) {
        method_creators_[method] = creator;
    }
};

const int Server::kConnectionTimeoutSeconds;

Server::Server(int port, int thread_count, const std::string& www_root, int actor_model, int trig_mode)
    : actor_model_(actor_model == 1 ? 1 : 0),
      listen_trig_mode_(0),
      conn_trig_mode_(0),
      port_(port),
      server_fd_(-1),
      epfd_(-1),
      pool_(thread_count),
      handler_factory_(new RequestHandlerFactory()),
      www_root_(www_root) {
    // trig_mode 编码：
    // 0 -> LT + LT
    // 1 -> LT + ET
    // 2 -> ET + LT
    // 3 -> ET + ET
    if (trig_mode == 1) {
        listen_trig_mode_ = 0;
        conn_trig_mode_ = 1;
    } else if (trig_mode == 2) {
        listen_trig_mode_ = 1;
        conn_trig_mode_ = 0;
    } else if (trig_mode == 3) {
        listen_trig_mode_ = 1;
        conn_trig_mode_ = 1;
    }
}

uint32_t Server::listen_epoll_events() const {
    uint32_t events = EPOLLIN;
    if (listen_trig_mode_ == 1) {
        events |= EPOLLET;
    }
    return events;
}

uint32_t Server::conn_epoll_events() const {
    uint32_t events = EPOLLIN | EPOLLONESHOT;
    if (conn_trig_mode_ == 1) {
        events |= EPOLLET;
    }
    return events;
}

void Server::erase_conn_activity(int fd) {
    std::lock_guard<std::mutex> lock(conn_mtx_);
    active_timers_.erase(fd);
}

void Server::erase_conn_buffer(int fd) {
    std::lock_guard<std::mutex> lock(conn_mtx_);
    conn_read_buffers_.erase(fd);
}

void Server::close_client_connection(int fd) {
    close(fd);
    erase_conn_activity(fd);
    erase_conn_buffer(fd);
}

void Server::refresh_conn_timer(int fd) {
    std::lock_guard<std::mutex> lock(conn_mtx_);
    active_timers_[fd] =
        std::chrono::steady_clock::now() + std::chrono::seconds(kConnectionTimeoutSeconds);
}

int Server::next_timeout_ms() {
    // 在高频 keep-alive 场景里，按请求维护小根堆的成本比定期扫一次连接表更高。
    return 1000;
}

bool Server::add_conn_fd_to_epoll(int client_fd) {
    epoll_event client_ev;
    client_ev.events = conn_epoll_events();
    client_ev.data.fd = client_fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
        Logger::instance().error(
            "epoll_ctl ADD client_fd 失败, fd = " + std::to_string(client_fd) +
            ", error = " + std::string(strerror(errno))
        );
        return false;
    }
    return true;
}

bool Server::rearm_conn_fd_in_epoll(int client_fd) {
    epoll_event client_ev;
    client_ev.events = conn_epoll_events();
    client_ev.data.fd = client_fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, client_fd, &client_ev) == -1) {
        Logger::instance().error(
            "epoll_ctl MOD client_fd 失败, fd = " + std::to_string(client_fd) +
            ", error = " + std::string(strerror(errno))
        );
        return false;
    }
    return true;
}

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
    if (listen(server_fd_, SOMAXCONN) == -1) {
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
    ev.events = listen_epoll_events();
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

    Logger::instance().info(
        "并发模型 = " + std::string(actor_model_ == 1 ? "Reactor(1)" : "模拟Proactor(0)") +
        ", listen触发 = " + std::string(listen_trig_mode_ == 1 ? "ET" : "LT") +
        ", conn触发 = " + std::string(conn_trig_mode_ == 1 ? "ET" : "LT")
    );
    Logger::instance().info("已将监听 fd 加入 epoll, server_fd = " + std::to_string(server_fd_));
    return true;
}

bool Server::read_file(const std::string& filename, std::string& content) {
    Logger::instance().debug("尝试读取文件: " + filename);

    std::ifstream ifs(filename.c_str(), std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        Logger::instance().error("无法打开文件: " + filename);
        return false;
    }

    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    if (size < 0) {
        Logger::instance().error("读取文件大小失败: " + filename);
        return false;
    }

    content.resize(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    if (size > 0) {
        ifs.read(&content[0], static_cast<std::streamsize>(size));
        if (!ifs) {
            Logger::instance().error("读取文件内容失败: " + filename);
            return false;
        }
    }

    return true;
}

std::shared_ptr<const Server::StaticFileCacheEntry> Server::get_static_file(const std::string& file_path) {
    {
        std::lock_guard<std::mutex> lock(static_file_cache_mtx_);
        auto it = static_file_cache_.find(file_path);
        if (it != static_file_cache_.end()) {
            return it->second;
        }
    }

    std::string content;
    if (!read_file(file_path, content)) {
        return std::shared_ptr<const StaticFileCacheEntry>();
    }

    std::shared_ptr<StaticFileCacheEntry> entry(new StaticFileCacheEntry());
    entry->body.swap(content);
    entry->content_type = content_type_from_path(file_path);
    if (entry->body.size() <= kMaxPrebuiltResponseBytes) {
        entry->keep_alive_response = build_response_buffer(
            "HTTP/1.1 200 OK\r\n",
            entry->content_type,
            entry->body,
            true
        );
        entry->close_response = build_response_buffer(
            "HTTP/1.1 200 OK\r\n",
            entry->content_type,
            entry->body,
            false
        );
    }

    if (entry->body.size() <= kMaxCachedFileBytes) {
        std::lock_guard<std::mutex> lock(static_file_cache_mtx_);
        std::shared_ptr<const StaticFileCacheEntry>& slot = static_file_cache_[file_path];
        if (!slot) {
            slot = entry;
        }
        return slot;
    }

    return entry;
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
    if (ext == ".mp4") {
        // 让浏览器按视频资源处理 mp4，而不是当作普通二进制下载。
        return "video/mp4";
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
        "password_hash VARCHAR(64) NOT NULL,"
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

bool Server::verify_user(const std::string& username, const std::string& password, std::string& error_message) {
    std::string db_error;
    MYSQL* conn = create_mysql_connection(db_error);
    if (!conn) {
        error_message = "数据库连接失败: " + db_error;
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

Server::ReadHttpRequestResult Server::pop_buffered_http_request(int client_fd, std::string& raw_request) {
    raw_request.clear();

    std::lock_guard<std::mutex> lock(conn_mtx_);
    std::unordered_map<int, std::string>::iterator it = conn_read_buffers_.find(client_fd);
    if (it == conn_read_buffers_.end() || it->second.empty()) {
        return kReadHttpRequestIncomplete;
    }

    size_t request_size = 0;
    HttpRequest::ParseState parse_state = HttpRequest::parse_request_size(it->second, request_size);
    if (parse_state == HttpRequest::kInvalid) {
        return kReadHttpRequestError;
    }
    if (parse_state == HttpRequest::kIncomplete) {
        return kReadHttpRequestIncomplete;
    }

    raw_request = it->second.substr(0, request_size);
    it->second.erase(0, request_size);
    if (it->second.empty()) {
        conn_read_buffers_.erase(it);
    }

    return kReadHttpRequestReady;
}

Server::ReadHttpRequestResult Server::read_http_request(
    int client_fd,
    std::string& raw_request,
    bool& peer_closed
) {
    peer_closed = false;

    ReadHttpRequestResult buffered_result = pop_buffered_http_request(client_fd, raw_request);
    if (buffered_result != kReadHttpRequestIncomplete) {
        return buffered_result;
    }

    while (true) {
        char buf[4096] = {0};
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(conn_mtx_);
            // 每个连接保留自己的读缓冲区，用来承接半包和同一批次读到的多条请求。
            std::string& pending = conn_read_buffers_[client_fd];
            pending.append(buf, static_cast<size_t>(n));
            if (pending.size() > kMaxBufferedRequestBytes) {
                Logger::instance().error(
                    "连接缓冲区超过上限，关闭连接, fd = " + std::to_string(client_fd)
                );
                conn_read_buffers_.erase(client_fd);
                return kReadHttpRequestError;
            }
            continue;
        }

        if (n == 0) {
            peer_closed = true;
            break;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        Logger::instance().error(
            "recv 失败, fd = " + std::to_string(client_fd) +
            ", errno = " + std::to_string(errno) +
            ", error = " + std::string(strerror(errno))
        );
        return kReadHttpRequestError;
    }

    buffered_result = pop_buffered_http_request(client_fd, raw_request);
    if (buffered_result == kReadHttpRequestReady) {
        return buffered_result;
    }
    if (buffered_result == kReadHttpRequestError) {
        Logger::instance().error("检测到非法 HTTP 请求, fd = " + std::to_string(client_fd));
        return buffered_result;
    }

    if (peer_closed) {
        std::lock_guard<std::mutex> lock(conn_mtx_);
        std::unordered_map<int, std::string>::iterator it = conn_read_buffers_.find(client_fd);
        if (it != conn_read_buffers_.end() && !it->second.empty()) {
            Logger::instance().error("客户端关闭连接时仍有不完整请求, fd = " + std::to_string(client_fd));
            return kReadHttpRequestError;
        }
        return kReadHttpRequestClosed;
    }

    return kReadHttpRequestIncomplete;
}

bool Server::send_buffer(int client_fd, const std::string& buffer, size_t& sent_bytes) const {
    sent_bytes = 0;
    while (sent_bytes < buffer.size()) {
        ssize_t n = send(client_fd, buffer.data() + sent_bytes, buffer.size() - sent_bytes, 0);
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

bool Server::send_response_parts(
    int client_fd,
    const std::string& headers,
    const std::string& body,
    size_t& sent_bytes
) const {
    sent_bytes = 0;
    size_t headers_sent = 0;
    size_t body_sent = 0;

    while (headers_sent < headers.size() || body_sent < body.size()) {
        iovec iov[2];
        int iov_count = 0;
        if (headers_sent < headers.size()) {
            iov[iov_count].iov_base = const_cast<char*>(headers.data() + headers_sent);
            iov[iov_count].iov_len = headers.size() - headers_sent;
            ++iov_count;
        }
        if (body_sent < body.size()) {
            iov[iov_count].iov_base = const_cast<char*>(body.data() + body_sent);
            iov[iov_count].iov_len = body.size() - body_sent;
            ++iov_count;
        }

        ssize_t n = writev(client_fd, iov, iov_count);
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
        size_t remaining = static_cast<size_t>(n);

        if (headers_sent < headers.size()) {
            size_t headers_remaining = headers.size() - headers_sent;
            size_t consumed_headers = std::min(remaining, headers_remaining);
            headers_sent += consumed_headers;
            remaining -= consumed_headers;
        }
        if (remaining > 0) {
            body_sent += remaining;
        }
    }

    return true;
}

bool Server::process_request_and_respond(
    int client_fd,
    const std::string& raw_request,
    bool& keep_alive
) {
    keep_alive = false;

    HttpRequest request;
    if (!request.parse(raw_request)) {
        Logger::instance().error("HTTP 请求解析失败, fd = " + std::to_string(client_fd));
        return false;
    }

    std::string request_path = url_decode(strip_query_and_fragment(request.path()));
    Logger::instance().debug(
        "解析请求成功, fd = " + std::to_string(client_fd) +
        ", method = " + request.method() +
        ", path = " + request_path +
        ", version = " + request.version()
    );

    std::unique_ptr<RequestHandler> handler = handler_factory_->create(request, request_path);
    if (!handler) {
        handler.reset(new MethodNotAllowedHandler());
    }
    HandlerResponse response = handler->handle(*this, request, request_path);
    const std::string& response_body = response.payload();

    bool request_keep_alive = false;
    std::string conn_header = to_lower_copy(request.header("Connection"));
    if (request.version() == "HTTP/1.1") {
        request_keep_alive = (conn_header != "close");
    } else if (request.version() == "HTTP/1.0") {
        request_keep_alive = (conn_header == "keep-alive");
    }

    std::string response_headers;
    response_headers.reserve(128 + response.content_type.size());
    response_headers += response.status_line;
    response_headers += "Content-Type: " + response.content_type + "\r\n";
    response_headers += "Content-Length: " + std::to_string(response_body.size()) + "\r\n";
    response_headers += "Connection: " + std::string(request_keep_alive ? "keep-alive" : "close") + "\r\n";
    response_headers += "\r\n";

    size_t sent = 0;
    bool can_use_prebuilt_response =
        request.method() == "GET" &&
        response.status_text == "200 OK" &&
        response.static_file &&
        !(request_keep_alive ? response.static_file->keep_alive_response.empty() : response.static_file->close_response.empty());
    bool send_ok = false;
    if (can_use_prebuilt_response) {
        const std::string& prebuilt_response =
            request_keep_alive ? response.static_file->keep_alive_response : response.static_file->close_response;
        send_ok = send_buffer(client_fd, prebuilt_response, sent);
        if (!send_ok) {
            Logger::instance().error(
                "send 失败, fd = " + std::to_string(client_fd) +
                ", errno = " + std::to_string(errno) +
                ", error = " + std::string(strerror(errno))
            );
        }
    } else {
        send_ok = send_response_parts(client_fd, response_headers, response_body, sent);
        if (!send_ok) {
            Logger::instance().error(
                "send 失败, fd = " + std::to_string(client_fd) +
                ", errno = " + std::to_string(errno) +
                ", error = " + std::string(strerror(errno))
            );
        }
    }

    if (send_ok) {
        Logger::instance().debug(
            "响应发送成功, fd = " + std::to_string(client_fd) +
            ", status = " + response.status_text +
            ", bytes = " + std::to_string(sent)
        );
    }

    keep_alive = send_ok && request_keep_alive && sent > 0;
    return send_ok;
}

void Server::process_ready_requests(int client_fd, const std::string& first_request, bool peer_closed) {
    std::string raw_request = first_request;
    bool should_close_after_buffer_drain = peer_closed;

    while (true) {
        // 同一个连接里如果已经缓冲了多条完整请求，就顺序拆包并连续处理。
        bool keep_alive = false;
        if (!process_request_and_respond(client_fd, raw_request, keep_alive)) {
            close_client_connection(client_fd);
            Logger::instance().debug("连接关闭, fd = " + std::to_string(client_fd));
            return;
        }

        if (!keep_alive) {
            close_client_connection(client_fd);
            Logger::instance().debug("连接关闭, fd = " + std::to_string(client_fd));
            return;
        }

        ReadHttpRequestResult next_result = pop_buffered_http_request(client_fd, raw_request);
        if (next_result == kReadHttpRequestReady) {
            Logger::instance().debug(
                "连接缓冲区中还有完整请求，继续处理, fd = " + std::to_string(client_fd)
            );
            continue;
        }
        if (next_result == kReadHttpRequestError) {
            Logger::instance().error("连接缓冲区中的后续请求非法, fd = " + std::to_string(client_fd));
            close_client_connection(client_fd);
            return;
        }

        if (should_close_after_buffer_drain) {
            close_client_connection(client_fd);
            Logger::instance().debug("对端已关闭输入，处理完缓冲区后关闭连接, fd = " + std::to_string(client_fd));
            return;
        }

        if (!rearm_conn_fd_in_epoll(client_fd)) {
            close_client_connection(client_fd);
            return;
        }
        refresh_conn_timer(client_fd);
        Logger::instance().debug("保持连接, fd = " + std::to_string(client_fd));
        return;
    }
}

void Server::handle_client_impl(int client_fd) {
    std::string raw_request;
    bool peer_closed = false;
    ReadHttpRequestResult read_result = read_http_request(client_fd, raw_request, peer_closed);
    if (read_result == kReadHttpRequestError) {
        Logger::instance().error("读取请求失败或客户端已关闭连接, fd = " + std::to_string(client_fd));
        close_client_connection(client_fd);
        return;
    }
    if (read_result == kReadHttpRequestClosed) {
        Logger::instance().debug("客户端主动关闭连接, fd = " + std::to_string(client_fd));
        close_client_connection(client_fd);
        return;
    }
    if (read_result == kReadHttpRequestIncomplete) {
        if (!rearm_conn_fd_in_epoll(client_fd)) {
            close_client_connection(client_fd);
            return;
        }
        refresh_conn_timer(client_fd);
        Logger::instance().debug("请求未接收完整，等待后续数据, fd = " + std::to_string(client_fd));
        return;
    }

    process_ready_requests(client_fd, raw_request, peer_closed);
}

void Server::handle_client(Server* server, int client_fd) {
    server->handle_client_impl(client_fd);
}

void Server::check_timeout_connections() {
    std::vector<int> expired_fds;
    {
        std::lock_guard<std::mutex> lock(conn_mtx_);
        auto now = std::chrono::steady_clock::now();
        for (std::unordered_map<int, std::chrono::steady_clock::time_point>::iterator it = active_timers_.begin();
             it != active_timers_.end();) {
            if (it->second <= now) {
                expired_fds.push_back(it->first);
                it = active_timers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (size_t i = 0; i < expired_fds.size(); ++i) {
        int fd = expired_fds[i];
        Logger::instance().debug("连接超时，关闭 fd = " + std::to_string(fd));
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        close_client_connection(fd);
    }
}

void Server::run() {
    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (true) {
        // 让 epoll 直接睡到“最近一个超时点”，避免固定周期扫描全部连接。
        int nfds = epoll_wait(epfd_, events, MAX_EVENTS, next_timeout_ms());
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
                // listenfd:
                // LT: 每次事件取一个连接即可（剩余连接下轮仍会触发）。
                // ET: 必须循环 accept 到 EAGAIN 为止，否则可能漏连接事件。
                bool continue_accept = true;
                while (continue_accept) {
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

                    Logger::instance().debug(
                        "有客户端连接, fd = " + std::to_string(client_fd) +
                        ", ip = " + std::string(ip) +
                        ", port = " + std::to_string(port)
                    );

                    if (!set_nonblocking(client_fd)) {
                        Logger::instance().error(
                            "设置 client_fd 非阻塞失败, fd = " + std::to_string(client_fd)
                        );
                        close_client_connection(client_fd);
                        continue;
                    }

                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                    if (!add_conn_fd_to_epoll(client_fd)) {
                        close_client_connection(client_fd);
                        continue;
                    }
                    refresh_conn_timer(client_fd);
                    Logger::instance().debug(
                        "客户端 fd 已加入 epoll, client_fd = " + std::to_string(client_fd)
                    );

                    // LT 模式下，本轮只处理一个新连接。
                    if (listen_trig_mode_ == 0) {
                        continue_accept = false;
                    }
                }
            }
            // 客户端可读
            else if (events[i].events & EPOLLIN) {
                int client_fd = fd;

                Logger::instance().debug(
                    "客户端 fd 可读，准备交给线程池处理, client_fd = " + std::to_string(client_fd)
                );
                // ONESHOT 模式下本轮事件处理完成前不会再次触发，这里只需要移除空闲计时。
                erase_conn_activity(client_fd);

                if (actor_model_ == 1) {
                    // Reactor：业务线程负责读 + 业务 + 写。
                    pool_.enqueue([this, client_fd]() {
                        Server::handle_client(this, client_fd);
                    });
                    Logger::instance().debug(
                        "Reactor 任务已加入线程池, client_fd = " + std::to_string(client_fd)
                    );
                } else {
                    // 模拟 Proactor：主线程先完成读，再把“已读请求”交给线程池处理业务。
                    std::string raw_request;
                    bool peer_closed = false;
                    ReadHttpRequestResult read_result = read_http_request(client_fd, raw_request, peer_closed);
                    if (read_result == kReadHttpRequestError) {
                        Logger::instance().error("主线程读取请求失败, fd = " + std::to_string(client_fd));
                        close_client_connection(client_fd);
                        continue;
                    }
                    if (read_result == kReadHttpRequestClosed) {
                        Logger::instance().debug("客户端主动关闭连接, fd = " + std::to_string(client_fd));
                        close_client_connection(client_fd);
                        continue;
                    }
                    if (read_result == kReadHttpRequestIncomplete) {
                        if (!rearm_conn_fd_in_epoll(client_fd)) {
                            close_client_connection(client_fd);
                            continue;
                        }
                        refresh_conn_timer(client_fd);
                        Logger::instance().debug("主线程读到半包，等待后续数据, fd = " + std::to_string(client_fd));
                        continue;
                    }

                    pool_.enqueue([this, client_fd, raw_request, peer_closed]() {
                        process_ready_requests(client_fd, raw_request, peer_closed);
                    });
                    Logger::instance().debug(
                        "模拟Proactor任务已加入线程池, client_fd = " + std::to_string(client_fd)
                    );
                }
            }
            // 其它异常
            else {
                Logger::instance().error(
                    "fd = " + std::to_string(fd) +
                    " 发生异常事件, events = " + std::to_string(events[i].events) +
                    "，关闭连接"
                );
                epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
                close_client_connection(fd);
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
