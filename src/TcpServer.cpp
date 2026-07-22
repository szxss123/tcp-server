#include "TcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr std::size_t kRequestBufferSize = 4096;

class SocketHandle {
public:
    explicit SocketHandle(int fd) : fd_(fd) {}

    ~SocketHandle() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    int get() const {
        return fd_;
    }

private:
    int fd_;
};

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
    const char* converted_ip = ::inet_ntop(
        AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip));
    if (converted_ip == nullptr) {
        printError("inet_ntop");
        converted_ip = "<unknown>";
    }
    std::cout << "Accepted connection from " << converted_ip << ':'
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
    const SocketHandle client_socket(client_fd);
    char buffer[kRequestBufferSize];

    ssize_t n;
    do {
        n = ::recv(client_socket.get(), buffer, sizeof(buffer) - 1, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        printError("recv");
        return;
    }
    if (n == 0) {
        return;
    }

    buffer[n] = '\0';
    const std::string request(buffer, static_cast<std::size_t>(n));

    std::cout << "Request:\n" << request << '\n';

    const bool is_get = request.rfind("GET ", 0) == 0;

    std::string body;
    std::string response;

    if (is_get) {
        body =
            "<!DOCTYPE html>"
            "<html>"
            "<head><meta charset=\"UTF-8\"><title>C++ Server</title></head>"
            "<body>"
            "<h1>Hello from C++ HTTP Server</h1>"
            "<p>The server is running successfully.</p>"
            "</body>"
            "</html>";

        response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=UTF-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" +
            body;
    } else {
        body = "Method Not Allowed";

        response =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" +
            body;
    }

    std::size_t total_sent = 0;
    while (total_sent < response.size()) {
        const ssize_t sent = ::send(
            client_socket.get(),
            response.data() + total_sent,
            response.size() - total_sent,
            MSG_NOSIGNAL);

        if (sent > 0) {
            total_sent += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0) {
            printError("send");
        } else {
            std::cerr << "send returned zero bytes\n";
        }
        break;
    }
}

void TcpServer::printError(const char* operation) {
    std::cerr << operation << " failed: " << std::strerror(errno) << '\n';
}
