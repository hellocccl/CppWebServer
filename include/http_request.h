#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <string>

class HttpRequest {
private:
    std::string method_;
    std::string path_;
    std::string version_;

public:
    // 解析原始请求报文
    bool parse(const std::string& raw_request);

    const std::string& method() const;
    const std::string& path() const;
    const std::string& version() const;
};

#endif