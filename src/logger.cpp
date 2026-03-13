#include "logger.h"

#include <ctime>
#include <iostream>

namespace {
int level_to_value(const std::string& level) {
    if (level == "ERROR") {
        return 0;
    }
    if (level == "DEBUG") {
        return 2;
    }
    return 1;
}
} // namespace

Logger::Logger() : to_file_(false), write_mode_(0), min_level_(kInfo), stop_worker_(false) {}

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

bool Logger::should_log(const std::string& level) const {
    return level_to_value(level) <= min_level_;
}

void Logger::log(const std::string& level, const std::string& message) {
    if (!should_log(level)) {
        return;
    }

    std::string line;
    bool need_async_enqueue = false;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        line = "[" + get_time_string() + "][" + level + "] " + message;

        // 控制台仍然实时输出，便于调试。
        if (level == "ERROR") {
            std::cerr << line << '\n';
        } else {
            std::cout << line << '\n';
        }

        if (!to_file_ || !file_.is_open()) {
            return;
        }

        if (write_mode_ == 0) {
            // 同步模式：业务线程直接写文件，避免每行 flush 放大 I/O 开销。
            file_ << line << '\n';
        } else {
            // 异步模式：业务线程只负责入队，不直接刷盘。
            need_async_enqueue = true;
        }
    }

    if (need_async_enqueue) {
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            async_queue_.push(line);
        }
        queue_cv_.notify_one();
    }
}

void Logger::async_write_loop() {
    while (true) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait(lock, [this]() { return stop_worker_ || !async_queue_.empty(); });

            if (stop_worker_ && async_queue_.empty()) {
                break;
            }

            line = async_queue_.front();
            async_queue_.pop();
        }

        // 只有后台线程写文件，减少业务线程 I/O 阻塞。
        std::lock_guard<std::mutex> lock(mtx_);
        if (to_file_ && file_.is_open()) {
            file_ << line << '\n';
        }
    }
}

void Logger::stop_async_worker() {
    bool joinable = false;
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (worker_.joinable()) {
            stop_worker_ = true;
            joinable = true;
        }
    }

    if (joinable) {
        queue_cv_.notify_all();
        worker_.join();
    }
}

bool Logger::init(const std::string& filename, int write_mode) {
    stop_async_worker();

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (file_.is_open()) {
            file_.close();
        }

        file_.open(filename, std::ios::app);
        if (!file_.is_open()) {
            to_file_ = false;
            return false;
        }

        to_file_ = true;
        write_mode_ = (write_mode == 1) ? 1 : 0;
    }

    if (write_mode_ == 1) {
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            stop_worker_ = false;
            std::queue<std::string> empty;
            std::swap(async_queue_, empty);
        }
        worker_ = std::thread(&Logger::async_write_loop, this);
    }
    
    return true;
}

void Logger::set_level(int level) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (level <= kError) {
        min_level_ = kError;
    } else if (level >= kDebug) {
        min_level_ = kDebug;
    } else {
        min_level_ = kInfo;
    }
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

Logger::~Logger() {
    stop_async_worker();
    std::lock_guard<std::mutex> lock(mtx_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}
