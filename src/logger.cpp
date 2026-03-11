#include "logger.h"

#include <ctime>
#include <iostream>

Logger::Logger() : to_file_(false) {}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

std::string Logger::get_time_string() {
    std::time_t now = std::time(nullptr);
    std::tm* tm_info = std::localtime(&now);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buffer);
}

void Logger::log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx_);

    std::string line = "[" + get_time_string() + "][" + level + "] " + message;

    // 输出到控制台
    if (level == "ERROR") {
        std::cerr << line << std::endl;
    } else {
        std::cout << line << std::endl;
    }

    // 如果开启了文件输出，也写入文件
    if (to_file_ && file_.is_open()) {
        file_ << line << std::endl;
        file_.flush();
    }
}

bool Logger::init(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mtx_);
    file_.open(filename, std::ios::app);
    if (!file_.is_open()) {
        return false;
    }
    to_file_ = true;
    return true;
}

void Logger::info(const std::string& message) {
    log("INFO", message);
}

void Logger::error(const std::string& message) {
    log("ERROR", message);
}

void Logger::debug(const std::string& message) {
    log("DEBUG", message);
}