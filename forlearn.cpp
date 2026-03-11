#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <queue>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cerrno>
using namespace std;

class ThreadPool {
private:
    vector<thread> workers;                  // 线程池中的工作线程
    queue<function<void()>> tasks;           // 任务队列，里面存放“要执行的函数”
    mutex mtx;                               // 保护任务队列的互斥锁
    condition_variable cv;                   // 条件变量，用来通知工作线程“有任务来了”
    bool stop;                               // 是否停止线程池
public:
    // 构造函数：创建 thread_count 个工作线程
    ThreadPool(int thread_count) : stop(false) {
        for (int i = 0; i < thread_count; i++) {
            workers.emplace_back([this]() {
                while (true) {
                    function<void()> task;

                    {
                        unique_lock<mutex> lock(this->mtx);
                        // 如果任务队列为空，就阻塞等待
                        this->cv.wait(lock, [this]() {
                            return this->stop || !this->tasks.empty();
                        });

                        // 如果线程池要停止，并且任务也没了，就退出线程
                        if (this->stop && this->tasks.empty()) {
                            return;
                        }
                        // 取出一个任务
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }

                    // 执行任务
                    task();
                }
            });
        }
    }

    // 往线程池里添加任务
    void enqueue(function<void()> task) {
        {
            unique_lock<mutex> lock(mtx);
            tasks.push(std::move(task));
        }
        cv.notify_one(); // 通知一个工作线程来干活
    }

    // 析构函数：关闭线程池
    ~ThreadPool() {
        {
            unique_lock<mutex> lock(mtx);
            stop = true;
        }

        cv.notify_all(); // 唤醒所有线程

        for (thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
};

/*
    把 fd 设置为非阻塞
    为什么要这样做？
    因为 epoll 常常配合非阻塞 socket 使用。
    尤其是 accept 时，我们通常会循环 accept，
    直到返回 EAGAIN，表示“现在没有更多连接了”。
*/
bool set_nonblocking(int fd) {
    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags == -1) return false;

    if (fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) == -1) {
        return false;
    }
    return true;
}

/*
    处理一个客户端连接
    这个函数会被线程池中的工作线程执行
*/
void handle_client(int client_fd) {
    char buffer[1024] = {0};
    // 收请求
    int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    cout << "收到请求:\n" << buffer << endl;
    // 解析 HTTP 请求行
    // 例如：
    // GET /hello HTTP/1.1
    string request(buffer);
    string method, path, version;
    stringstream ss(request);
    ss >> method >> path >> version;
    cout << "method = " << method << ", path = " << path << ", version = " << version << endl;
    // 根据路径构造不同页面
    string body;
    string status_line;
    if (path == "/") {
        status_line = "HTTP/1.1 200 OK\r\n";
        body =
            "<html>"
            "<body>"
            "<h1>首页</h1>"
            "<p>你访问的是 /</p>"
            "<p><a href=\"/hello\">访问 /hello</a></p>"
            "</body>"
            "</html>";
    }
    else if (path == "/hello") {
        status_line = "HTTP/1.1 200 OK\r\n";
        body =
            "<html>"
            "<body>"
            "<h1>Hello 页面</h1>"
            "<p>你访问的是 /hello</p>"
            "<p><a href=\"/\">返回首页</a></p>"
            "</body>"
            "</html>";
    }
    else {
        status_line = "HTTP/1.1 404 Not Found\r\n";
        body =
            "<html>"
            "<body>"
            "<h1>404 Not Found</h1>"
            "<p>你访问的路径不存在</p>"
            "<p><a href=\"/\">返回首页</a></p>"
            "</body>"
            "</html>";
    }
    // 拼接 HTTP 响应
    string response =
        status_line +
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: " + to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;

    // 发送响应
    send(client_fd, response.c_str(), response.size(), 0);
    // 关闭连接
    close(client_fd);
}

int main() {
    // 1. 创建 socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "创建 socket 失败: " << strerror(errno) << endl;
        return 1;
    }

    // 允许端口复用，避免程序重启时 bind 失败
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 设置监听 socket 为非阻塞
    if (!set_nonblocking(server_fd)) {
        cerr << "设置 server_fd 非阻塞失败" << endl;
        close(server_fd);
        return 1;
    }

    // 2. 准备服务器地址信息
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);

    // 3. 绑定 IP 和端口
    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        cerr << "bind 失败: " << strerror(errno) << endl;
        close(server_fd);
        return 1;
    }

    // 4. 开始监听
    if (listen(server_fd, 10) == -1) {
        cerr << "listen 失败: " << strerror(errno) << endl;
        close(server_fd);
        return 1;
    }

    cout << "服务器启动成功，监听 8080 端口..." << endl;

    // 创建线程池，比如 4 个工作线程
    ThreadPool pool(4);

    // 5. 创建 epoll 实例
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        cerr << "epoll_create1 失败: " << strerror(errno) << endl;
        close(server_fd);
        return 1;
    }

    // 6. 把监听 fd 加入 epoll
    epoll_event ev;
    ev.events = EPOLLIN;       // 关心“可读事件”
    ev.data.fd = server_fd;
    //epoll_ctl管理 epoll 监控的 fd。
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        cerr << "epoll_ctl ADD server_fd 失败: " << strerror(errno) << endl;
        close(epfd);
        close(server_fd);
        return 1;
    }

    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (true) {
        // 7. 等待事件发生
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);//nfds 这次有多少个 fd 就绪了。
        if (nfds == -1) {
            if (errno == EINTR) {
                continue; // 被信号中断，继续等
            }
            cerr << "epoll_wait 失败: " << strerror(errno) << endl;
            break;
        }

        // 8. 遍历所有就绪事件
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            // 情况1：监听 socket 可读，说明有新连接到来
            if (fd == server_fd) {
                while (true) {
                    sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
                    if (client_fd == -1) {
                        // 非阻塞 accept 下，没有更多连接了
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            cerr << "accept 失败: " << strerror(errno) << endl;
                            break;
                        }
                    }
                    cout << "有客户端连接, fd = " << client_fd << endl;
                    // 为了配合 epoll，客户端 fd 也设置成非阻塞
                    if (!set_nonblocking(client_fd)) {
                        cerr << "设置 client_fd 非阻塞失败, fd = " << client_fd << endl;
                        close(client_fd);
                        continue;
                    }

                    epoll_event client_ev;
                    client_ev.events = EPOLLIN;   // 关心客户端“可读”
                    client_ev.data.fd = client_fd; //哪个 fd 出事了
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
                        cerr << "epoll_ctl ADD client_fd 失败: " << strerror(errno) << endl;
                        close(client_fd);
                        continue;
                    }
                }
            }
            // 情况2：客户端 socket 可读，说明客户端发请求了
            //events[i].events 表示 这个 fd 出了什么事件
            //EPOLLIN：可读
            //EPOLLOUT：可写
            //EPOLLERR：出错
            //EPOLLHUP：挂断
            else if (events[i].events & EPOLLIN) {
                int client_fd = fd;

                // 这里做一个“最小改动”的关键处理：
                // 在把任务交给线程池之前，先把这个 fd 从 epoll 中删除。
                // 这样可以避免同一个 fd 被重复触发、重复入队。
                epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, nullptr);

                pool.enqueue([client_fd]() {
                    handle_client(client_fd);
                });
            }
            // 情况3：其它异常事件
            else {
                cerr << "fd = " << fd << " 发生异常事件，关闭连接" << endl;
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
            }
        }
    }

    close(epfd);
    close(server_fd);
    return 0;
}