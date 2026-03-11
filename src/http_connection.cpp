#include "http_connection.h"
#include "logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>

// 构造函数：初始化 HTTP 连接，接收文件描述符和根目录路径
HttpConnection::HttpConnection(int fd, const std::string& root_dir)
    : fd_(fd), root_dir_(root_dir) {}

// 析构函数：关闭文件描述符（如果有效）
HttpConnection::~HttpConnection() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

// 处理 HTTP 请求并发送相应的响应
void HttpConnection::process() {
    std::string request;
    // 从客户端读取 HTTP 请求
    if (!read_request(request)) {
        Logger::instance().error("读取请求失败，fd=" + std::to_string(fd_));
        return;
    }

    Logger::instance().info("来自 fd=" + std::to_string(fd_) + " 的请求: " + request);

    // 构建并发送 HTTP 响应
    std::string response = build_response(request);
    if (!send_all(response)) {
        Logger::instance().error("发送响应失败，fd=" + std::to_string(fd_));
        return;
    }
}

// 从套接字读取 HTTP 请求
bool HttpConnection::read_request(std::string& request) {
    char buffer[4096] = {0}; // 用于存储接收数据的缓冲区
    ssize_t n = recv(fd_, buffer, sizeof(buffer) - 1, 0); // 从套接字接收数据
    if (n <= 0) {
        return false; // 如果未接收到数据或发生错误，返回 false
    }
    request.assign(buffer, n); // 将接收到的数据赋值给请求字符串
    return true;
}

// 根据请求构建 HTTP 响应
std::string HttpConnection::build_response(const std::string& request) {
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version; // 解析 HTTP 请求行

    // 仅支持 GET 方法
    if (method != "GET") {
        std::string body = "仅支持 GET 方法。\n";
        std::ostringstream oss;
        oss << "HTTP/1.1 405 Method Not Allowed\r\n"
            << "Content-Type: text/plain\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;    
        return oss.str();
    }

    // 处理 GET 请求
    return handle_get(path);
}

// 处理 GET 请求并返回相应的响应
std::string HttpConnection::handle_get(const std::string& path) {
    std::string real_path = path;
    if (real_path == "/") {
        real_path = "/index.html"; // 如果路径是根目录，默认返回 index.html
    }

    // 防止目录穿越攻击（例如访问 ../../etc/passwd）
    if (real_path.find("..") != std::string::npos) {
        std::string body = "403 禁止访问\n";
        std::ostringstream oss;
        oss << "HTTP/1.1 403 Forbidden\r\n"
            << "Content-Type: text/plain\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        return oss.str();
    }

    // 构造请求文件的完整路径
    std::string full_path = root_dir_ + real_path;
    std::ifstream ifs(full_path, std::ios::binary);
    if (!ifs.is_open()) {
        // 如果文件不存在，返回 404 Not Found
        std::string body = "404 未找到\n";
        std::ostringstream oss;
        oss << "HTTP/1.1 404 Not Found\r\n"
            << "Content-Type: text/plain\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        return oss.str();
    }

    // 读取文件内容
    std::ostringstream body_stream;
    body_stream << ifs.rdbuf();
    std::string body = body_stream.str();

    // 构建包含文件内容的 HTTP 响应
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << get_mime_type(full_path) << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return oss.str();
}

// 根据文件扩展名确定 MIME 类型
std::string HttpConnection::get_mime_type(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") return "image/png";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg") return "image/jpeg";
    return "text/plain"; // 如果没有匹配，默认返回 text/plain
}

// 将完整的 HTTP 响应发送给客户端
bool HttpConnection::send_all(const std::string& response) {
    size_t total = 0;
    while (total < response.size()) {
        ssize_t n = send(fd_, response.data() + total, response.size() - total, 0);
        if (n <= 0) {
            return false; // 如果发送失败，返回 false
        }
        total += static_cast<size_t>(n); // 更新已发送的字节数
    }
    return true;
}
