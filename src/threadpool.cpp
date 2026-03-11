#include "threadpool.h"

ThreadPool::ThreadPool(int thread_count) : stop(false) {
    for (int i = 0; i < thread_count; i++) {
        workers.emplace_back([this]() {
            while (true) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(this->mtx);

                    // 队列为空就等待
                    this->cv.wait(lock, [this]() {
                        return this->stop || !this->tasks.empty();
                    });

                    // 如果线程池停止，并且任务也已经做完，就退出线程
                    if (this->stop && this->tasks.empty()) {
                        return;
                    }

                    // 取任务
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }

                // 执行任务
                task();
            }
        });
    }
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(mtx);
        tasks.push(std::move(task));
    }
    cv.notify_one();
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop = true;
    }

    cv.notify_all();

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}