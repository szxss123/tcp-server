#pragma once

#include "AsyncLogger.h"
#include "SocketFd.h"
#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

class TcpServer {
public:
    explicit TcpServer(std::uint16_t port);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start();
    void run();

private:
    static constexpr std::size_t kThreadCount = 4;

    enum class ConnectionState {
        Reading,
        Writing,
    };

    struct Connection {
        Connection(SocketFd socket_fd, std::string remote_ip)
            : socket(std::move(socket_fd)),
              client_ip(std::move(remote_ip)) {}

        SocketFd socket;
        std::string client_ip;
        std::string input_buffer;
        std::string output_buffer;
        std::size_t bytes_sent{0};
        ConnectionState state{ConnectionState::Reading};
        bool keep_alive{false};
        std::size_t requests_served{0};
        std::chrono::steady_clock::time_point last_active{
            std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point request_started{};
        std::string log_method{"-"};
        std::string log_path{"-"};
        int response_status_code{0};
        std::size_t response_bytes{0};
        bool request_started_set{false};
        bool processing{false};
    };

    void handleReadable(int epoll_fd, int fd);
    void handleWritable(int epoll_fd, int fd);
    void queueResponse(int epoll_fd,
                       int fd,
                       std::string response,
                       bool keep_alive,
                       int status_code,
                       std::string method,
                       std::string path);
    void rearmRead(int epoll_fd, int fd);
    void rearmWrite(int epoll_fd, int fd);
    void closeIdleConnections(int epoll_fd,
                              std::chrono::seconds timeout);
    void closeConnection(int epoll_fd, int fd);
    void closeAllConnections(int epoll_fd);
    void shutdown();

    bool createSocket();
    bool bindPort() const;
    bool listenConnections() const;

    static void printError(const char* operation);

    std::uint16_t port_;
    SocketFd listen_socket_;
    SocketFd epoll_socket_;
    std::unordered_map<int, Connection> connections_;
    std::mutex connections_mutex_;
    std::once_flag shutdown_once_;
    std::atomic<bool> stopping_{false};
    AsyncLogger access_logger_{"logs/access.log", 4096};
    ThreadPool thread_pool_{kThreadCount};
};
