#ifndef LOGGER_H
#define LOGGER_H

#include <fstream>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <thread>

class Logger {
private:
    std::mutex mtx_;          // 保证多线程写日志安全
    std::ofstream file_;      // 日志文件输出流
    bool to_file_;            // 是否写入文件
    int write_mode_;          // 0: 同步写日志 1: 异步写日志

    // 仅在异步模式下使用：业务线程把日志行放入队列，由后台线程统一刷盘。
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::queue<std::string> async_queue_;
    std::thread worker_;
    bool stop_worker_;

    // 私有构造，单例模式
    Logger();

    // 获取当前时间字符串
    std::string get_time_string();

    // 真正写日志的函数
    void log(const std::string& level, const std::string& message);
    void async_write_loop();
    void stop_async_worker();

public:
    // 删除拷贝构造和赋值，保证单例
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 获取全局唯一实例
    static Logger& instance();

    // 初始化日志文件，write_mode:
    // 0 -> 同步写入（默认）
    // 1 -> 异步写入（后台线程写文件）
    bool init(const std::string& filename, int write_mode = 0);
    ~Logger();

    void info(const std::string& message);
    void error(const std::string& message);
    void debug(const std::string& message);
};

#endif
