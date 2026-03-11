#ifndef LOGGER_H
#define LOGGER_H
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <ctime>
class Logger {
private:
    std::mutex mtx_;          // 保证多线程写日志安全
    std::ofstream file_;      // 日志文件输出流
    bool to_file_;            // 是否写入文件

    // 私有构造，单例模式
    Logger() : to_file_(false) {}

    // 获取当前时间字符串
    std::string get_time_string() {
        std::time_t now = std::time(nullptr);
        std::tm* tm_info = std::localtime(&now);

        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
        return std::string(buffer);
    }

    // 真正写日志的函数
    void log(const std::string& level, const std::string& message) {
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

public:
    // 删除拷贝构造和赋值，保证单例
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 获取全局唯一实例
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // 初始化日志文件
    bool init(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mtx_);
        file_.open(filename, std::ios::app);
        if (!file_.is_open()) {
            return false;
        }
        to_file_ = true;
        return true;
    }

    void info(const std::string& message) {
        log("INFO", message);
    }

    void error(const std::string& message) {
        log("ERROR", message);
    }

    void debug(const std::string& message) {
        log("DEBUG", message);
    }
};

#endif