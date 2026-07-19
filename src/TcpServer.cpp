#include "TcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>

#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_running = 1;

void handleSignal(int) {
    g_running = 0;
}

bool installSignalHandler(int signal_number) {
    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return ::sigaction(signal_number, &action, nullptr) == 0;
}

}  // namespace

bool installSignalHandlers() {
    return installSignalHandler(SIGINT) && installSignalHandler(SIGTERM);
}

TcpServer::TcpServer(std::uint16_t port) : port_(port) {}

TcpServer::~TcpServer() {
    closeSocket(server_fd_);
}

bool TcpServer::createSocket() {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        printError("socket");
        return false;
    }

    int reuse_address = 1;
    if (::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR,
                     &reuse_address, sizeof(reuse_address)) < 0) {
        printError("setsockopt");
        closeSocket(server_fd_);
        return false;
    }
    return true;
}

bool TcpServer::bindPort() const {
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port_);

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&server_address),
               sizeof(server_address)) < 0) {
        printError("bind");
        return false;
    }
    return true;
}

bool TcpServer::listenConnections() const {
    if (::listen(server_fd_, SOMAXCONN) < 0) {
        printError("listen");
        return false;
    }

    std::cout << "TCP server is listening on 0.0.0.0:" << port_
              << " (press Ctrl+C to stop)" << std::endl;
    return true;
}

int TcpServer::acceptClient() const {
    sockaddr_in client_address{};
    socklen_t address_length = sizeof(client_address);

    const int client_fd = ::accept(
        server_fd_, reinterpret_cast<sockaddr*>(&client_address),
        &address_length);

    if (client_fd < 0) {
        if (errno != EINTR) {
            printError("accept");
        }
        return -1;
    }

    char client_ip[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, &client_address.sin_addr,
                client_ip, sizeof(client_ip));
    std::cout << "Accepted connection from " << client_ip << ':'
              << ntohs(client_address.sin_port) << std::endl;
    return client_fd;
}

void TcpServer::closeSocket(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool TcpServer::start() {
    return createSocket() && bindPort() && listenConnections();
}

void TcpServer::run() {
    while (g_running) {
        int client_fd = acceptClient();
        if (client_fd < 0) {
            continue;
        }

        thread_pool_.submit([this, client_fd] {
            handleClient(client_fd);
        });
    }
}

void TcpServer::handleClient(int client_fd) {
    echoClient(client_fd);
    closeSocket(client_fd);
}

bool TcpServer::sendAll(int client_fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t result = ::send(client_fd, data + sent,
                                      size - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result < 0) {
            printError("send");
        }
        return false;
    }
    return true;
}

void TcpServer::echoClient(int client_fd) {
    char buffer[4096];

    while (g_running) {
        const ssize_t bytes_read = ::recv(client_fd, buffer,
                                          sizeof(buffer), 0);
        if (bytes_read > 0) {
            if (!sendAll(client_fd, buffer,
                         static_cast<std::size_t>(bytes_read))) {
                break;
            }
            continue;
        }
        if (bytes_read == 0) {
            std::cout << "Client closed the connection." << std::endl;
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        printError("recv");
        break;
    }
}

void TcpServer::printError(const char* operation) {
    std::cerr << operation << " failed: " << std::strerror(errno) << '\n';
}