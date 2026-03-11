#ifndef LOGGER_H
#define LOGGER_H

#include <fstream>
#include <mutex>
#include <string>

class Logger {
private:
    std::mutex mtx_;          // 保证多线程写日志安全
    std::ofstream file_;      // 日志文件输出流
    bool to_file_;            // 是否写入文件

    // 私有构造，单例模式
    Logger();

    // 获取当前时间字符串
    std::string get_time_string();

    // 真正写日志的函数
    void log(const std::string& level, const std::string& message);

public:
    // 删除拷贝构造和赋值，保证单例
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 获取全局唯一实例
    static Logger& instance();

    // 初始化日志文件
    bool init(const std::string& filename);

    void info(const std::string& message);
    void error(const std::string& message);
    void debug(const std::string& message);
};

#endif