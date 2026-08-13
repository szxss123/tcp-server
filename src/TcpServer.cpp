#include "TcpServer.h"

#include "HttpRequest.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
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
constexpr std::size_t kMaxRequestsPerConnection = 100;
constexpr std::uintmax_t kMaxStaticFileSize = 1024 * 1024;

namespace fs = std::filesystem;

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
                          const std::string& content_type,
                          const std::string& body,
                          bool keep_alive) {
    return "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n"
           "Content-Type: " + content_type + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: " +
           std::string(keep_alive ? "keep-alive" : "close") + "\r\n"
           "\r\n" +
           body;
}

std::string buildErrorResponse(int status_code,
                               const std::string& reason,
                               bool keep_alive) {
    return buildResponse(status_code, reason,
                         "text/plain; charset=UTF-8", reason,
                         keep_alive);
}

std::string contentTypeFor(const fs::path& file_path) {
    const std::string extension = file_path.extension().string();
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=UTF-8";
    }
    if (extension == ".css") {
        return "text/css; charset=UTF-8";
    }
    if (extension == ".js") {
        return "application/javascript; charset=UTF-8";
    }
    if (extension == ".json") {
        return "application/json; charset=UTF-8";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

std::optional<std::string> readFile(const fs::path& file_path,
                                    std::uintmax_t max_size) {
    std::error_code error;
    if (!fs::is_regular_file(file_path, error) || error) {
        return std::nullopt;
    }

    const std::uintmax_t file_size = fs::file_size(file_path, error);
    if (error || file_size > max_size) {
        return std::nullopt;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    std::string content(static_cast<std::size_t>(file_size), '\0');
    if (!content.empty()) {
        file.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file || file.gcount() !=
                         static_cast<std::streamsize>(content.size())) {
            return std::nullopt;
        }
    }
    return content;
}

bool isWithinPublicRoot(const fs::path& public_root,
                        const fs::path& file_path) {
    std::error_code error;
    const fs::path relative = fs::relative(file_path, public_root, error);
    if (error || relative.empty() || relative.is_absolute()) {
        return false;
    }

    for (const fs::path& component : relative) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

std::string buildResponse(const HttpRequest& request, bool keep_alive) {
    if (request.method != "GET") {
        return buildErrorResponse(405, "Method Not Allowed", keep_alive);
    }

    std::string request_path = request.path;
    const std::size_t query_position = request_path.find('?');
    if (query_position != std::string::npos) {
        request_path.erase(query_position);
    }
    if (request_path == "/") {
        request_path = "/index.html";
    }

    if (request_path.empty() || request_path.front() != '/' ||
        request_path.find("..") != std::string::npos ||
        request_path.find('%') != std::string::npos ||
        request_path.find('\0') != std::string::npos) {
        return buildErrorResponse(400, "Bad Request", keep_alive);
    }

    std::error_code error;
    const fs::path public_root = fs::weakly_canonical("public", error);
    if (error) {
        return buildErrorResponse(500, "Internal Server Error", false);
    }

    const fs::path relative_path = request_path.substr(1);
    const fs::path file_path =
        fs::weakly_canonical(public_root / relative_path, error);
    if (error || !isWithinPublicRoot(public_root, file_path)) {
        return buildErrorResponse(403, "Forbidden", keep_alive);
    }

    std::optional<std::string> content =
        readFile(file_path, kMaxStaticFileSize);
    if (!content) {
        return buildErrorResponse(404, "Not Found", keep_alive);
    }

    return buildResponse(200, "OK", contentTypeFor(file_path),
                         *content, keep_alive);
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
    bool pipelined_request = false;
    std::size_t requests_served = 0;
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
            pipelined_request = header_end + 4 < input_buffer.size();
            requests_served = connection->second.requests_served;
        }
    }

    if (header_too_large) {
        queueResponse(
            epoll_fd, fd,
            buildResponse(400, "Bad Request",
                          "text/plain; charset=UTF-8",
                          "Request header too large", false),
            false);
        return;
    }

    if (request_ready) {
        std::cout << "Request:\n" << raw_request << '\n';

        HttpRequest request;
        const ParseResult result = parseRequest(raw_request, request);
        bool keep_alive = false;
        std::string response;
        if (result != ParseResult::Complete) {
            response = buildErrorResponse(400, "Bad Request", false);
        } else {
            keep_alive = request.keep_alive && !pipelined_request &&
                         requests_served + 1 < kMaxRequestsPerConnection;
            response = buildResponse(request, keep_alive);
        }

        queueResponse(epoll_fd, fd, std::move(response), keep_alive);
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

        if (invalid_state) {
            closeConnection(epoll_fd, fd);
            return;
        }
        if (finished) {
            bool reuse_connection = false;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                const auto connection = connections_.find(fd);
                if (connection == connections_.end()) {
                    return;
                }

                Connection& state = connection->second;
                ++state.requests_served;
                reuse_connection =
                    state.keep_alive &&
                    state.requests_served < kMaxRequestsPerConnection;

                if (reuse_connection) {
                    state.input_buffer.clear();
                    state.output_buffer.clear();
                    state.bytes_sent = 0;
                    state.state = ConnectionState::Reading;
                    state.keep_alive = false;
                    state.last_active =
                        std::chrono::steady_clock::now();
                }
            }

            if (reuse_connection) {
                rearmRead(epoll_fd, fd);
            } else {
                closeConnection(epoll_fd, fd);
            }
            return;
        }
        if (count == 0) {
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
                              std::string response,
                              bool keep_alive) {
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
                state.keep_alive = keep_alive;
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
