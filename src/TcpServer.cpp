#include "TcpServer.h"

#include "HttpRequest.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxHeaderSize = 8192;
constexpr std::size_t kReadBufferSize = 2048;
constexpr std::chrono::seconds kIdleTimeout{30};
constexpr std::chrono::seconds kTimeoutScanInterval{1};

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
    if (::sigemptyset(&action.sa_mask) < 0) {
        return false;
    }
    action.sa_flags = 0;
    return ::sigaction(signal_number, &action, nullptr) == 0;
}

std::string buildResponse(int status_code,
                          const std::string& reason,
                          const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n"
           "Content-Type: text/html; charset=UTF-8\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" +
           body;
}

std::string buildResponse(const HttpRequest& request) {
    if (request.method != "GET") {
        return buildResponse(405, "Method Not Allowed",
                             "Method Not Allowed");
    }
    if (request.path == "/large") {
        constexpr std::size_t kLargeResponseSize = 1024 * 1024;
        return buildResponse(200, "OK",
                             std::string(kLargeResponseSize, 'A'));
    }
    if (request.path == "/") {
        return buildResponse(200, "OK",
                             "<h1>Hello from C++ HTTP Server</h1>");
    }
    return buildResponse(404, "Not Found", "Not Found");
}}  // namespace

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
    epoll_socket_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_socket_.valid()) {
        printError("epoll_create1");
        return;
    }
    const int epoll_fd = epoll_socket_.get();

    SocketFd timer_socket(
        ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    if (!timer_socket.valid()) {
        printError("timerfd_create");
        return;
    }

    itimerspec timer_spec{};
    timer_spec.it_value.tv_sec = kTimeoutScanInterval.count();
    timer_spec.it_interval.tv_sec = kTimeoutScanInterval.count();
    if (::timerfd_settime(timer_socket.get(), 0, &timer_spec, nullptr) < 0) {
        printError("timerfd_settime");
        return;
    }

    epoll_event listen_event{};
    listen_event.events = EPOLLIN | EPOLLET;
    listen_event.data.fd = listen_socket_.get();

    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_socket_.get(),
                    &listen_event) < 0) {
        printError("epoll_ctl ADD listen");
        return;
    }

    epoll_event timer_event{};
    timer_event.events = EPOLLIN;
    timer_event.data.fd = timer_socket.get();
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_socket.get(),
                    &timer_event) < 0) {
        printError("epoll_ctl ADD timer");
        return;
    }

    constexpr int kMaxEvents = 64;
    std::vector<epoll_event> events(kMaxEvents);

    while (g_running) {
        const int ready_count =
            ::epoll_wait(epoll_fd, events.data(), kMaxEvents, -1);

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

            if (fd == timer_socket.get()) {
                std::uint64_t expirations = 0;
                while (true) {
                    const ssize_t count = ::read(
                        timer_socket.get(), &expirations,
                        sizeof(expirations));
                    if (count == static_cast<ssize_t>(sizeof(expirations))) {
                        closeIdleConnections(epoll_fd, kIdleTimeout);
                        continue;
                    }
                    if (count < 0 && errno == EINTR) {
                        continue;
                    }
                    if (count < 0 &&
                        (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }

                    if (count < 0) {
                        printError("read timerfd");
                    } else {
                        std::cerr << "timerfd returned an invalid read size\n";
                    }
                    return;
                }
                continue;
            }

            if (fd == listen_socket_.get()) {
                if ((active_events & (EPOLLERR | EPOLLHUP)) != 0) {
                    std::cerr
                        << "listening socket reported an epoll error\n";
                    return;
                }

                while (true) {
                    SocketFd client_socket(::accept4(
                        listen_socket_.get(), nullptr, nullptr,
                        SOCK_NONBLOCK | SOCK_CLOEXEC));
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
                    client_event.events =
                        EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
                    client_event.data.fd = client_fd;

                    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd,
                                    &client_event) < 0) {
                        printError("epoll_ctl ADD client");
                        continue;
                    }

                    bool inserted = false;
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        const auto result = connections_.try_emplace(
                            client_fd, std::move(client_socket));
                        inserted = result.second;
                        if (inserted) {
                            result.first->second.last_active =
                                std::chrono::steady_clock::now();
                        }
                    }

                    if (!inserted) {
                        std::cerr << "client fd is already registered: "
                                  << client_fd << '\n';
                        if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd,
                                        nullptr) < 0 &&
                            errno != ENOENT) {
                            printError("epoll_ctl DEL duplicate client");
                        }
                        continue;
                    }

                    std::cout << "client connected, fd=" << client_fd << '\n';
                }
                continue;
            }

            enum class PendingTask {
                None,
                Read,
                Write,
            };

            bool registered = false;
            bool processing = false;
            ConnectionState state = ConnectionState::Reading;
            PendingTask pending_task = PendingTask::None;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                const auto connection = connections_.find(fd);
                if (connection != connections_.end()) {
                    registered = true;
                    state = connection->second.state;
                    processing = connection->second.processing;

                    if (!processing &&
                        (active_events & EPOLLIN) != 0 &&
                        state == ConnectionState::Reading) {
                        connection->second.processing = true;
                        pending_task = PendingTask::Read;
                    } else if (!processing &&
                               (active_events & EPOLLOUT) != 0 &&
                               state == ConnectionState::Writing) {
                        connection->second.processing = true;
                        pending_task = PendingTask::Write;
                    }
                }
            }

            if (!registered) {
                if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0 &&
                    errno != ENOENT) {
                    printError("epoll_ctl DEL stale client");
                }
                continue;
            }

            if (pending_task == PendingTask::Read) {
                thread_pool_.submit([this, epoll_fd, fd] {
                    handleReadable(epoll_fd, fd);
                });
                continue;
            }

            if (pending_task == PendingTask::Write) {
                thread_pool_.submit([this, epoll_fd, fd] {
                    handleWritable(epoll_fd, fd);
                });
                continue;
            }

            if (processing) {
                continue;
            }

            if ((active_events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                closeConnection(epoll_fd, fd);
                continue;
            }

            if (state == ConnectionState::Reading) {
                rearmRead(epoll_fd, fd);
            } else {
                rearmWrite(epoll_fd, fd);
            }
        }
    }
}
void TcpServer::handleReadable(int epoll_fd, int fd) {
    char buffer[kReadBufferSize];
    bool peer_closed = false;
    bool read_failed = false;

    while (true) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);

        if (count > 0) {
            bool request_ready = false;
            bool header_too_large = false;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                const auto connection = connections_.find(fd);
                if (connection == connections_.end() ||
                    connection->second.state != ConnectionState::Reading) {
                    return;
                }

                std::string& input_buffer = connection->second.input_buffer;
                input_buffer.append(buffer, static_cast<std::size_t>(count));
                connection->second.last_active =
                    std::chrono::steady_clock::now();

                const std::size_t header_end =
                    input_buffer.find("\r\n\r\n");
                request_ready = header_end != std::string::npos;
                header_too_large =
                    (request_ready && header_end + 4 > kMaxHeaderSize) ||
                    (!request_ready &&
                     input_buffer.size() > kMaxHeaderSize);
            }

            if (request_ready || header_too_large) {
                break;
            }
            continue;
        }

        if (count == 0) {
            peer_closed = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        printError("recv");
        read_failed = true;
        break;
    }

    std::string raw_request;
    bool request_ready = false;
    bool header_too_large = false;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        const auto connection = connections_.find(fd);
        if (connection == connections_.end() ||
            connection->second.state != ConnectionState::Reading) {
            return;
        }

        const std::string& input_buffer = connection->second.input_buffer;
        const std::size_t header_end = input_buffer.find("\r\n\r\n");
        request_ready = header_end != std::string::npos;
        header_too_large =
            (request_ready && header_end + 4 > kMaxHeaderSize) ||
            (!request_ready && input_buffer.size() > kMaxHeaderSize);

        if (request_ready && !header_too_large) {
            raw_request = input_buffer;
        }
    }

    if (header_too_large) {
        queueResponse(
            epoll_fd, fd,
            buildResponse(400, "Bad Request",
                          "Request header too large"));
        return;
    }

    if (request_ready) {
        std::cout << "Request:\n" << raw_request << '\n';

        HttpRequest request;
        const ParseResult result = parseRequest(raw_request, request);
        std::string response;
        if (result != ParseResult::Complete) {
            response = buildResponse(400, "Bad Request", "Bad Request");
        } else {
            response = buildResponse(request);
        }

        queueResponse(epoll_fd, fd, std::move(response));
        return;
    }

    if (peer_closed || read_failed) {
        closeConnection(epoll_fd, fd);
        return;
    }

    rearmRead(epoll_fd, fd);
}

void TcpServer::handleWritable(int epoll_fd, int fd) {
    while (true) {
        ssize_t count = 0;
        int send_error = 0;
        bool finished = false;
        bool invalid_state = false;

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            const auto connection = connections_.find(fd);
            if (connection == connections_.end()) {
                return;
            }

            Connection& state = connection->second;
            if (state.state != ConnectionState::Writing ||
                state.bytes_sent > state.output_buffer.size()) {
                invalid_state = true;
            } else {
                const std::size_t remaining =
                    state.output_buffer.size() - state.bytes_sent;
                if (remaining == 0) {
                    finished = true;
                } else {
                    count = ::send(
                        fd,
                        state.output_buffer.data() + state.bytes_sent,
                        remaining,
                        MSG_NOSIGNAL);
                    if (count > 0) {
                        state.bytes_sent +=
                            static_cast<std::size_t>(count);
                        state.last_active =
                            std::chrono::steady_clock::now();
                    } else if (count < 0) {
                        send_error = errno;
                    }
                }
            }
        }

        if (invalid_state || finished || count == 0) {
            closeConnection(epoll_fd, fd);
            return;
        }
        if (count > 0) {
            continue;
        }
        if (send_error == EINTR) {
            continue;
        }
        if (send_error == EAGAIN || send_error == EWOULDBLOCK) {
            rearmWrite(epoll_fd, fd);
            return;
        }

        if (send_error != EPIPE && send_error != ECONNRESET) {
            errno = send_error;
            printError("send");
        }
        closeConnection(epoll_fd, fd);
        return;
    }
}

void TcpServer::queueResponse(int epoll_fd,
                              int fd,
                              std::string response) {
    bool found = false;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        const auto connection = connections_.find(fd);
        if (connection != connections_.end()) {
            found = true;
            Connection& state = connection->second;
            if (state.state == ConnectionState::Reading) {
                state.output_buffer = std::move(response);
                state.bytes_sent = 0;
                state.state = ConnectionState::Writing;
                queued = true;
            }
        }
    }

    if (!found) {
        return;
    }
    if (!queued) {
        closeConnection(epoll_fd, fd);
        return;
    }
    rearmWrite(epoll_fd, fd);
}

void TcpServer::rearmRead(int epoll_fd, int fd) {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
    event.data.fd = fd;

    int control_error = 0;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        const auto connection = connections_.find(fd);
        if (connection == connections_.end()) {
            return;
        }

        connection->second.processing = false;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
            control_error = errno;
        }
    }

    if (control_error != 0) {
        errno = control_error;
        printError("epoll_ctl MOD read");
        closeConnection(epoll_fd, fd);
    }
}

void TcpServer::rearmWrite(int epoll_fd, int fd) {
    epoll_event event{};
    event.events = EPOLLOUT | EPOLLRDHUP | EPOLLONESHOT;
    event.data.fd = fd;

    int control_error = 0;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        const auto connection = connections_.find(fd);
        if (connection == connections_.end()) {
            return;
        }

        connection->second.processing = false;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
            control_error = errno;
        }
    }

    if (control_error != 0) {
        errno = control_error;
        printError("epoll_ctl MOD write");
        closeConnection(epoll_fd, fd);
    }
}

void TcpServer::closeIdleConnections(int epoll_fd,
                                     std::chrono::seconds timeout) {
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> expired_fds;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [fd, connection] : connections_) {
            if (!connection.processing &&
                now - connection.last_active >= timeout) {
                expired_fds.push_back(fd);
            }
        }
    }

    for (const int fd : expired_fds) {
        bool should_close = false;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            const auto connection = connections_.find(fd);
            if (connection != connections_.end() &&
                !connection->second.processing &&
                now - connection->second.last_active >= timeout) {
                connection->second.processing = true;
                should_close = true;
            }
        }

        if (should_close) {
            closeConnection(epoll_fd, fd);
        }
    }
}

void TcpServer::closeConnection(int epoll_fd, int fd) {
    SocketFd client_socket;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        const auto connection = connections_.find(fd);
        if (connection == connections_.end()) {
            return;
        }

        client_socket = std::move(connection->second.socket);
        connections_.erase(connection);
    }

    if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0 &&
        errno != ENOENT) {
        printError("epoll_ctl DEL client");
    }
}
void TcpServer::printError(const char* operation) {
    std::cerr << operation << " failed: " << std::strerror(errno) << '\n';
}
