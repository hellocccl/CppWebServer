#include "http_request.h"

#include <sstream>
#include <algorithm>
#include <cctype>

namespace {
std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}
} // namespace

bool HttpRequest::parse(const std::string& raw_request) {
    headers_.clear();
    body_.clear();

    size_t line_end = raw_request.find("\r\n");
    if (line_end == std::string::npos) {
        return false;
    }

    std::string request_line = raw_request.substr(0, line_end);
    std::stringstream ss(request_line);
    ss >> method_ >> path_ >> version_;
    if (method_.empty() || path_.empty() || version_.empty()) {
        return false;
    }

    size_t header_end = raw_request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    size_t headers_start = line_end + 2;
    std::string headers_block = raw_request.substr(headers_start, header_end - headers_start);
    std::istringstream hs(headers_block);
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = to_lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        headers_[key] = value;
    }

    body_ = raw_request.substr(header_end + 4);
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

std::string HttpRequest::header(const std::string& key) const {
    std::string lower = to_lower(key);
    auto it = headers_.find(lower);
    if (it == headers_.end()) {
        return "";
    }
    return it->second;
}

const std::string& HttpRequest::body() const {
    return body_;
}
