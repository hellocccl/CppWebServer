#ifndef SERVER_H
#define SERVER_H
#include "threadpool.h"
#include <unordered_map>
#include <mutex>
#include <ctime>
#include <cstdint>
#include <string>
class Server {
private:
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
    
    std::unordered_map<int, time_t> last_active_;
    std::mutex conn_mtx_;
    
    void check_timeout_connections();
    // 读取文件内容
    bool read_file(const std::string& filename, std::string& content, bool binary = false);
    bool read_http_request(int client_fd, std::string& raw_request);
    bool resolve_static_path(const std::string& url_path, std::string& file_path) const;
    std::string content_type_from_path(const std::string& file_path) const;
    bool init_database();
    bool register_user(const std::string& username, const std::string& password, std::string& error_message);
    bool verify_user(const std::string& username, const std::string& password, std::string& error_message);
    void process_request_and_respond(int client_fd, const std::string& raw_request);
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
