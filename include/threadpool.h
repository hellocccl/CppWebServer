#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
private:
    std::vector<std::thread> workers;           // 工作线程
    std::queue<std::function<void()>> tasks;    // 任务队列
    std::mutex mtx;                             // 保护任务队列
    std::condition_variable cv;                 // 通知线程有任务可做
    bool stop;                                  // 是否停止线程池

public:
    explicit ThreadPool(int thread_count);

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void enqueue(std::function<void()> task);

    ~ThreadPool();
};

#endif