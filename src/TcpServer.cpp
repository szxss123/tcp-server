#include "TcpServer.h"

#include "HttpRequest.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxHeaderSize = 8192;
constexpr std::size_t kReadBufferSize = 2048;

volatile std::sig_atomic_t g_running = 1;

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }

    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

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

void sendResponse(int fd,
                  int status_code,
                  const std::string& reason,
                  const std::string& body) {
    const std::string response =
        "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;

    std::size_t total_sent = 0;
    while (total_sent < response.size()) {
        const ssize_t sent =
            ::send(fd, response.data() + total_sent,
                   response.size() - total_sent, MSG_NOSIGNAL);

        if (sent > 0) {
            total_sent += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0) {
            std::cerr << "send failed: " << std::strerror(errno) << '\n';
        } else {
            std::cerr << "send returned zero bytes\n";
        }
        return;
    }
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

    if (!setNonBlocking(listen_socket_.get())) {
        printError("setNonBlocking");
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
    SocketFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd.valid()) {
        printError("epoll_create1");
        return;
    }

    epoll_event listen_event{};
    listen_event.events = EPOLLIN | EPOLLET;
    listen_event.data.fd = listen_socket_.get();

    if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, listen_socket_.get(),
                    &listen_event) < 0) {
        printError("epoll_ctl ADD listen");
        return;
    }

    constexpr int kMaxEvents = 64;
    std::vector<epoll_event> events(kMaxEvents);
    std::unordered_map<int, SocketFd> waiting_clients;

    while (g_running) {
        const int ready_count = ::epoll_wait(
            epoll_fd.get(), events.data(), kMaxEvents, -1);

        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            printError("epoll_wait");
            break;
        }

        for (int i = 0; i < ready_count; ++i) {
            const int fd = events[i].data.fd;
            const std::uint32_t active_events = events[i].events;

            if (fd == listen_socket_.get()) {
                if ((active_events & (EPOLLERR | EPOLLHUP)) != 0) {
                    std::cerr
                        << "listening socket reported an epoll error\n";
                    return;
                }

                while (true) {
                    SocketFd client_socket(::accept4(
                        listen_socket_.get(), nullptr, nullptr, SOCK_CLOEXEC));
                    if (!client_socket.valid()) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        printError("accept4");
                        break;
                    }

                    const int client_fd = client_socket.get();
                    epoll_event client_event{};
                    client_event.events = EPOLLIN | EPOLLRDHUP;
                    client_event.data.fd = client_fd;

                    if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, client_fd,
                                    &client_event) < 0) {
                        printError("epoll_ctl ADD client");
                        continue;
                    }

                    const auto [position, inserted] =
                        waiting_clients.try_emplace(
                            client_fd, std::move(client_socket));
                    if (!inserted) {
                        std::cerr << "client fd is already registered: "
                                  << client_fd << '\n';
                        ::epoll_ctl(epoll_fd.get(), EPOLL_CTL_DEL, client_fd,
                                    nullptr);
                        continue;
                    }

                    std::cout << "client connected, fd="
                              << position->first << '\n';
                }
                continue;
            }

            if ((active_events & EPOLLIN) != 0) {
                if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_DEL, fd, nullptr) <
                    0) {
                    printError("epoll_ctl DEL client");
                }

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
                continue;
            }

            if ((active_events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_DEL, fd, nullptr) <
                        0 &&
                    errno != ENOENT) {
                    printError("epoll_ctl DEL client");
                }
                waiting_clients.erase(fd);
            }
        }
    }
}
void TcpServer::handleClient(int client_fd) {
    SocketFd client_socket(client_fd);
    std::string raw_request;
    char buffer[kReadBufferSize];

    while (raw_request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t count =
            ::recv(client_socket.get(), buffer, sizeof(buffer), 0);

        if (count > 0) {
            raw_request.append(buffer, static_cast<std::size_t>(count));

            const std::size_t header_end = raw_request.find("\r\n\r\n");
            if ((header_end == std::string::npos &&
                 raw_request.size() > kMaxHeaderSize) ||
                (header_end != std::string::npos &&
                 header_end + 4 > kMaxHeaderSize)) {
                sendResponse(client_socket.get(), 400, "Bad Request",
                             "Request header too large");
                return;
            }
            continue;
        }

        if (count == 0) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }

        printError("recv");
        return;
    }

    std::cout << "Request:\n" << raw_request << '\n';

    HttpRequest request;
    const ParseResult result = parseRequest(raw_request, request);
    if (result != ParseResult::Complete) {
        sendResponse(client_socket.get(), 400, "Bad Request", "Bad Request");
        return;
    }

    if (request.method != "GET") {
        sendResponse(client_socket.get(), 405, "Method Not Allowed",
                     "Method Not Allowed");
    } else if (request.path == "/") {
        sendResponse(client_socket.get(), 200, "OK",
                     "<h1>Hello from C++ HTTP Server</h1>");
    } else {
        sendResponse(client_socket.get(), 404, "Not Found", "Not Found");
    }
}
void TcpServer::printError(const char* operation) {
    std::cerr << operation << " failed: " << std::strerror(errno) << '\n';
}
