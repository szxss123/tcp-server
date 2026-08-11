#pragma once

#include "SocketFd.h"
#include "ThreadPool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

bool installSignalHandlers();

class TcpServer {
public:
    explicit TcpServer(std::uint16_t port);
    ~TcpServer() = default;

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
        explicit Connection(SocketFd socket_fd)
            : socket(std::move(socket_fd)) {}

        SocketFd socket;
        std::string input_buffer;
        std::string output_buffer;
        std::size_t bytes_sent{0};
        ConnectionState state{ConnectionState::Reading};
        std::chrono::steady_clock::time_point last_active{
            std::chrono::steady_clock::now()};
        bool processing{false};
    };

    void handleReadable(int epoll_fd, int fd);
    void handleWritable(int epoll_fd, int fd);
    void queueResponse(int epoll_fd, int fd, std::string response);
    void rearmRead(int epoll_fd, int fd);
    void rearmWrite(int epoll_fd, int fd);
    void closeIdleConnections(int epoll_fd,
                              std::chrono::seconds timeout);
    void closeConnection(int epoll_fd, int fd);

    bool createSocket();
    bool bindPort() const;
    bool listenConnections() const;

    static void printError(const char* operation);

    std::uint16_t port_;
    SocketFd listen_socket_;
    SocketFd epoll_socket_;
    std::unordered_map<int, Connection> connections_;
    std::mutex connections_mutex_;
    ThreadPool thread_pool_{kThreadCount};
};
