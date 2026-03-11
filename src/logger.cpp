#include "logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

bool Logger::init(const std::string& file_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.open(file_name, std::ios::app);
    return file_.is_open();
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.close();
    }
}

void Logger::info(const std::string& message) {
    write("INFO", message);
}

void Logger::error(const std::string& message) {
    write("ERROR", message);
}

void Logger::write(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "[" << now_time() << "]"
        << "[" << level << "] " << message << '\n';

    std::cout << oss.str();
    if (file_.is_open()) {
        file_ << oss.str();
        file_.flush();
    }
}

std::string Logger::now_time() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}
