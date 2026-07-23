#pragma once

#include "SocketFd.h"
#include "ThreadPool.h"

#include <cstddef>
#include <cstdint>

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

    void handleClient(int client_fd);

    bool createSocket();
    bool bindPort() const;
    bool listenConnections() const;
    int acceptClient() const;

    static void printError(const char* operation);

    std::uint16_t port_;
    SocketFd listen_socket_;
    ThreadPool thread_pool_{kThreadCount};
};
