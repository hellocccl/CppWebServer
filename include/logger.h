#pragma once

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

// 一个非常简单的线程安全日志类
// 目标：先帮你理解“日志系统”的基本职责：
// 1. 记录程序运行过程
// 2. 记录错误信息
// 3. 多线程下避免日志写乱
class Logger {
public:
    static Logger& instance();

    bool init(const std::string& file_name);
    void info(const std::string& message);
    void error(const std::string& message);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write(const std::string& level, const std::string& message);
    std::string now_time();

private:
    std::ofstream file_;
    std::mutex mutex_;
};
