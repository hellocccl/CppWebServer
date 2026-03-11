#include "http_request.h"

#include <sstream>

bool HttpRequest::parse(const std::string& raw_request) {
    std::stringstream ss(raw_request);

    // 只先解析第一行中的 3 个核心字段
    ss >> method_ >> path_ >> version_;

    if (method_.empty() || path_.empty() || version_.empty()) {
        return false;
    }

    return true;
}

const std::string& HttpRequest::method() const {
    return method_;
}

const std::string& HttpRequest::path() const {
    return path_;
}

const std::string& HttpRequest::version() const {
    return version_;
}