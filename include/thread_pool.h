#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// 线程池的作用：
// 主线程专门负责“接收连接 + epoll事件分发”
// 工作线程负责“真正处理业务逻辑”
// 这样可以避免主线程被单个请求卡住
class ThreadPool {
public:
    explicit ThreadPool(size_t thread_count);
    ~ThreadPool();

    // 往任务队列里塞一个任务
    void enqueue(std::function<void()> task);

private:
    void worker();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};
