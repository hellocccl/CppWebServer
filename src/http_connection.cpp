#include "http_connection.h"
#include "logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>

HttpConnection::HttpConnection(int fd, const std::string& root_dir)
    : fd_(fd), root_dir_(root_dir) {}

HttpConnection::~HttpConnection() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

void HttpConnection::process() {
    std::string request;
    if (!read_request(request)) {
        Logger::instance().error("read request failed, fd=" + std::to_string(fd_));
        return;
    }

    Logger::instance().info("request from fd=" + std::to_string(fd_) + ": " + request);

    std::string response = build_response(request);
    if (!send_all(response)) {
        Logger::instance().error("send response failed, fd=" + std::to_string(fd_));
        return;
    }
}

bool HttpConnection::read_request(std::string& request) {
    char buffer[4096] = {0};
    ssize_t n = recv(fd_, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        return false;
    }
    request.assign(buffer, n);
    return true;
}

std::string HttpConnection::build_response(const std::string& request) {
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;

    if (method != "GET") {
        std::string body = "Only GET is supported in this demo.\n";
        std::ostringstream oss;
        oss << "HTTP/1.1 405 Method Not Allowed\r\n"
            << "Content-Type: text/plain\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        return oss.str();
    }

    return handle_get(path);
}

std::string HttpConnection::handle_get(const std::string& path) {
    std::string real_path = path;
    if (real_path == "/") {
        real_path = "/index.html";
    }

    // 防止目录穿越攻击：比如访问 ../../etc/passwd
    if (real_path.find("..") != std::string::npos) {
        std::string body = "403 Forbidden\n";
        std::ostringstream oss;
        oss << "HTTP/1.1 403 Forbidden\r\n"
            << "Content-Type: text/plain\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        return oss.str();
    }

    std::string full_path = root_dir_ + real_path;
    std::ifstream ifs(full_path, std::ios::binary);
    if (!ifs.is_open()) {
        std::string body = "404 Not Found\n";
        std::ostringstream oss;
        oss << "HTTP/1.1 404 Not Found\r\n"
            << "Content-Type: text/plain\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        return oss.str();
    }

    std::ostringstream body_stream;
    body_stream << ifs.rdbuf();
    std::string body = body_stream.str();

    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << get_mime_type(full_path) << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return oss.str();
}

std::string HttpConnection::get_mime_type(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css";
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") return "image/png";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg") return "image/jpeg";
    return "text/plain";
}

bool HttpConnection::send_all(const std::string& response) {
    size_t total = 0;
    while (total < response.size()) {
        ssize_t n = send(fd_, response.data() + total, response.size() - total, 0);
        if (n <= 0) {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}
