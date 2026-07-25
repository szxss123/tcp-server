#include "TcpServer.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <sys/select.h>
#include <sys/socket.h>

namespace {

constexpr std::size_t kRequestBufferSize = 4096;

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

bool TcpServer::createSocket() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printError("socket");
        return false;
    }
    listen_socket_.reset(fd);

    int reuse_address = 1;
    if (::setsockopt(listen_socket_.get(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse_address, sizeof(reuse_address)) < 0) {
        printError("setsockopt");
        listen_socket_.reset();
        return false;
    }
    return true;
}
bool TcpServer::bindPort() const {
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port_);

    if (::bind(listen_socket_.get(),
               reinterpret_cast<sockaddr*>(&server_address),
               sizeof(server_address)) < 0) {
        printError("bind");
        return false;
    }
    return true;
}

bool TcpServer::listenConnections() const {
    if (::listen(listen_socket_.get(), SOMAXCONN) < 0) {
        printError("listen");
        return false;
    }

    std::cout << "TCP server is listening on 0.0.0.0:" << port_
              << " (press Ctrl+C to stop)" << std::endl;
    return true;
}

bool TcpServer::start() {
    return createSocket() && bindPort() && listenConnections();
}

void TcpServer::run() {
    fd_set master_set;
    FD_ZERO(&master_set);

    const int listen_fd = listen_socket_.get();
    if (listen_fd < 0 || listen_fd >= FD_SETSIZE) {
        std::cerr << "listen socket is outside select() range\n";
        return;
    }

    FD_SET(listen_fd, &master_set);
    int max_fd = listen_fd;
    std::unordered_map<int, SocketFd> waiting_clients;

    while (g_running) {
        fd_set ready_set = master_set;

        const int ready_count = ::select(
            max_fd + 1, &ready_set, nullptr, nullptr, nullptr);

        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            printError("select");
            break;
        }

        for (int fd = 0; fd <= max_fd; ++fd) {
            if (!FD_ISSET(fd, &ready_set)) {
                continue;
            }

            if (fd == listen_fd) {
                SocketFd client_socket(::accept(listen_fd, nullptr, nullptr));
                if (!client_socket.valid()) {
                    printError("accept");
                    continue;
                }

                const int client_fd = client_socket.get();
                if (client_fd >= FD_SETSIZE) {
                    std::cerr << "client fd exceeds FD_SETSIZE: "
                              << client_fd << '\n';
                    continue;
                }

                const auto [position, inserted] = waiting_clients.try_emplace(
                    client_fd, std::move(client_socket));
                if (!inserted) {
                    std::cerr << "client fd is already registered: "
                              << client_fd << '\n';
                    continue;
                }

                FD_SET(position->first, &master_set);
                max_fd = std::max(max_fd, position->first);
                std::cout << "client connected, fd="
                          << position->first << '\n';
                continue;
            }

            FD_CLR(fd, &master_set);

            auto client = waiting_clients.find(fd);
            if (client == waiting_clients.end()) {
                std::cerr << "ready client fd is not registered: "
                          << fd << '\n';
                continue;
            }

            const auto client_socket = std::make_shared<SocketFd>(
                std::move(client->second));
            waiting_clients.erase(client);

            thread_pool_.submit([this, client_socket] {
                handleClient(client_socket->release());
            });
        }
    }
}
void TcpServer::handleClient(int client_fd) {
    SocketFd client_socket(client_fd);
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
