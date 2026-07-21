#pragma once

#include "ThreadPool.h"

#include <cstddef>
#include <cstdint>

bool installSignalHandlers();

class TcpServer {
public:
    explicit TcpServer(std::uint16_t port);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start();
    void run();

private:
    void handleClient(int client_fd);

    bool createSocket();
    bool bindPort() const;
    bool listenConnections() const;
    int acceptClient() const;

    static void closeSocket(int& fd);
    static void printError(const char* operation);

    std::uint16_t port_;
    int server_fd_ = -1;
    ThreadPool thread_pool_{4};
};