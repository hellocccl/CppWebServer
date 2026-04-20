#include "http_request.h"

#include <algorithm>
#include <cctype>
#include <limits>

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

bool parse_size_value(const std::string& value, size_t& parsed) {
    std::string trimmed = trim(value);
    if (trimmed.empty()) {
        return false;
    }

    size_t result = 0;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(trimmed[i]);
        if (!std::isdigit(c)) {
            return false;
        }

        size_t digit = static_cast<size_t>(trimmed[i] - '0');
        if (result > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }

    parsed = result;
    return true;
}
} // namespace

HttpRequest::ParseState HttpRequest::parse_request_size(const std::string& raw_buffer, size_t& request_size) {
    request_size = 0;

    size_t line_end = raw_buffer.find("\r\n");
    if (line_end == std::string::npos) {
        return kIncomplete;
    }

    size_t method_end = raw_buffer.find(' ');
    if (method_end == std::string::npos || method_end >= line_end) {
        return kInvalid;
    }

    size_t path_end = raw_buffer.find(' ', method_end + 1);
    if (path_end == std::string::npos || path_end >= line_end) {
        return kInvalid;
    }

    size_t header_end = raw_buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return kIncomplete;
    }

    size_t content_length = 0;
    bool saw_content_length = false;
    size_t cursor = line_end + 2;
    while (cursor < header_end) {
        size_t next = raw_buffer.find("\r\n", cursor);
        if (next == std::string::npos || next > header_end) {
            next = header_end;
        }

        if (next == cursor) {
            cursor += 2;
            continue;
        }

        size_t colon = raw_buffer.find(':', cursor);
        if (colon != std::string::npos && colon < next) {
            std::string key = to_lower(trim(raw_buffer.substr(cursor, colon - cursor)));
            if (key == "content-length") {
                size_t parsed_length = 0;
                if (!parse_size_value(raw_buffer.substr(colon + 1, next - colon - 1), parsed_length)) {
                    return kInvalid;
                }

                if (saw_content_length && content_length != parsed_length) {
                    return kInvalid;
                }

                content_length = parsed_length;
                saw_content_length = true;
            }
        }

        cursor = next + 2;
    }

    request_size = header_end + 4 + content_length;
    if (raw_buffer.size() < request_size) {
        return kIncomplete;
    }

    return kComplete;
}

bool HttpRequest::parse(const std::string& raw_request) {
    method_.clear();
    path_.clear();
    version_.clear();
    headers_.clear();
    body_.clear();

    size_t line_end = raw_request.find("\r\n");
    if (line_end == std::string::npos) {
        return false;
    }

    size_t method_end = raw_request.find(' ');
    if (method_end == std::string::npos || method_end >= line_end) {
        return false;
    }

    size_t path_end = raw_request.find(' ', method_end + 1);
    if (path_end == std::string::npos || path_end >= line_end) {
        return false;
    }

    method_ = raw_request.substr(0, method_end);
    path_ = raw_request.substr(method_end + 1, path_end - method_end - 1);
    version_ = raw_request.substr(path_end + 1, line_end - path_end - 1);
    if (method_.empty() || path_.empty() || version_.empty()) {
        return false;
    }

    size_t header_end = raw_request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    size_t cursor = line_end + 2;
    while (cursor < header_end) {
        size_t next = raw_request.find("\r\n", cursor);
        if (next == std::string::npos || next > header_end) {
            next = header_end;
        }

        if (next == cursor) {
            cursor += 2;
            continue;
        }

        size_t colon = raw_request.find(':', cursor);
        if (colon != std::string::npos && colon < next) {
            std::string key = to_lower(trim(raw_request.substr(cursor, colon - cursor)));
            std::string value = trim(raw_request.substr(colon + 1, next - colon - 1));
            headers_[key] = value;
        }

        cursor = next + 2;
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
