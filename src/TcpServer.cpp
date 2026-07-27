#include "TcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

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
    std::vector<pollfd> fds{
        {listen_socket_.get(), POLLIN, 0},
    };
    std::unordered_map<int, SocketFd> waiting_clients;

    while (g_running) {
        const int ready_count = ::poll(
            fds.data(), static_cast<nfds_t>(fds.size()), -1);

        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            printError("poll");
            break;
        }

        if ((fds.front().revents & POLLIN) != 0) {
            SocketFd client_socket(
                ::accept(listen_socket_.get(), nullptr, nullptr));
            if (!client_socket.valid()) {
                if (errno != EINTR) {
                    printError("accept");
                }
            } else {
                const int client_fd = client_socket.get();
                const auto [position, inserted] = waiting_clients.try_emplace(
                    client_fd, std::move(client_socket));
                if (inserted) {
                    fds.push_back({position->first, POLLIN, 0});
                    std::cout << "client connected, fd="
                              << position->first << '\n';
                } else {
                    std::cerr << "client fd is already registered: "
                              << client_fd << '\n';
                }
            }
        }

        if ((fds.front().revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::cerr << "listening socket reported a poll error\n";
            break;
        }

        for (std::size_t i = 1; i < fds.size();) {
            const short events = fds[i].revents;
            const int client_fd = fds[i].fd;

            if ((events & POLLIN) != 0) {
                fds.erase(fds.begin() + i);

                auto client = waiting_clients.find(client_fd);
                if (client == waiting_clients.end()) {
                    std::cerr << "ready client fd is not registered: "
                              << client_fd << '\n';
                    continue;
                }

                const auto client_socket = std::make_shared<SocketFd>(
                    std::move(client->second));
                waiting_clients.erase(client);

                thread_pool_.submit([this, client_socket] {
                    handleClient(client_socket->release());
                });
                continue;
            }

            if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                waiting_clients.erase(client_fd);
                fds.erase(fds.begin() + i);
                continue;
            }

            ++i;
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
