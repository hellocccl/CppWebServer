#ifndef SERVER_H
#define SERVER_H
#include "threadpool.h"
#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>
class Server {
private:
    struct TimerNode {
        int fd;
        std::chrono::steady_clock::time_point expires_at;
    };

    struct TimerNodeCompare {
        bool operator()(const TimerNode& lhs, const TimerNode& rhs) const {
            return lhs.expires_at > rhs.expires_at;
        }
    };

    struct StaticFileCacheEntry {
        std::string body;
        std::string content_type;
    };

    // 反应堆模型：
    // 0 -> 模拟 Proactor（主线程读，线程池处理业务和写）
    // 1 -> Reactor（线程池负责读 + 业务 + 写）
    int actor_model_;

    // 触发模式：
    // 0 -> LT
    // 1 -> ET
    int listen_trig_mode_;
    int conn_trig_mode_;

    int port_;          // 监听端口
    int server_fd_;     // 监听 socket
    int epfd_;          // epoll 实例 fd
    ThreadPool pool_;   // 线程池
    std::string www_root_; // 网站根目录

    // 小根堆保存“最早过期”的连接，便于 O(log n) 找到超时连接。
    std::priority_queue<TimerNode, std::vector<TimerNode>, TimerNodeCompare> timer_heap_;
    // 记录每个连接当前有效的过期时间，配合堆做懒删除。
    std::unordered_map<int, std::chrono::steady_clock::time_point> active_timers_;
    std::mutex conn_mtx_;
    std::unordered_map<std::string, std::shared_ptr<const StaticFileCacheEntry>> static_file_cache_;
    std::mutex static_file_cache_mtx_;
    static const int kConnectionTimeoutSeconds = 30;

    void check_timeout_connections();
    int next_timeout_ms();
    void refresh_conn_timer(int fd);
    // 读取文件内容
    bool read_file(const std::string& filename, std::string& content);
    std::shared_ptr<const StaticFileCacheEntry> get_static_file(const std::string& file_path);
    bool read_http_request(int client_fd, std::string& raw_request);
    bool resolve_static_path(const std::string& url_path, std::string& file_path) const;
    std::string content_type_from_path(const std::string& file_path) const;
    bool init_database();
    bool register_user(const std::string& username, const std::string& password, std::string& error_message);
    bool verify_user(const std::string& username, const std::string& password, std::string& error_message);
    void process_request_and_respond(int client_fd, const std::string& raw_request);
    bool send_response_parts(int client_fd, const std::string& headers, const std::string& body, size_t& sent_bytes) const;
    uint32_t listen_epoll_events() const;
    uint32_t conn_epoll_events() const;
    bool add_conn_fd_to_epoll(int client_fd);
    void erase_conn_activity(int fd);

    // 把 fd 设置为非阻塞
    bool set_nonblocking(int fd);
    bool set_blocking(int fd);
    // 处理一个客户端连接
    static void handle_client(Server* server, int client_fd);
    void handle_client_impl(int client_fd);

public:
    Server(int port, int thread_count, const std::string& www_root, int actor_model, int trig_mode);

    // 初始化服务器
    bool init();

    // 启动事件循环
    void run();

    ~Server();
};

#endif
