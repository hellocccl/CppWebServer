#pragma once

#include <string>

class HttpConnection {
public:
    HttpConnection(int fd, const std::string& root_dir);
    ~HttpConnection();

    // 处理一次客户端请求
    void process();

private:
    bool read_request(std::string& request);
    std::string build_response(const std::string& request);
    std::string handle_get(const std::string& path);
    std::string get_mime_type(const std::string& path);
    bool send_all(const std::string& response);

private:
    int fd_;
    std::string root_dir_;
};
