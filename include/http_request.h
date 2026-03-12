#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <string>
#include <unordered_map>

class HttpRequest {
private:
    std::string method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;

public:
    // 解析原始请求报文
    bool parse(const std::string& raw_request);

    const std::string& method() const;
    const std::string& path() const;
    const std::string& version() const;
    std::string header(const std::string& key) const;
    const std::string& body() const;
};

#endif
