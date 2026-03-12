#ifndef SERVER_H
#define SERVER_H
#include "threadpool.h"
#include <unordered_map>
#include <mutex>
#include <ctime>
class Server {
private:
    int port_;          // 监听端口
    int server_fd_;     // 监听 socket
    int epfd_;          // epoll 实例 fd
    ThreadPool pool_;   // 线程池
    std::string www_root_; // 网站根目录
    
    std::unordered_map<int, time_t> last_active_;
    std::mutex conn_mtx_;
    
    void check_timeout_connections();
    // 读取文件内容
    bool read_file(const std::string& filename, std::string& content);
    bool read_http_request(int client_fd, std::string& raw_request);

    // 把 fd 设置为非阻塞
    bool set_nonblocking(int fd);
    bool set_blocking(int fd);
    // 处理一个客户端连接
    static void handle_client(Server* server, int client_fd);
    void handle_client_impl(int client_fd);

public:
    Server(int port, int thread_count, const std::string& www_root);

    // 初始化服务器
    bool init();

    // 启动事件循环
    void run();

    ~Server();
};

#endif
