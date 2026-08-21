#include "TcpServer.h"

#include "HttpRequest.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <signal.h>

#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxHeaderSize = 8192;
constexpr std::size_t kReadBufferSize = 2048;
constexpr std::chrono::seconds kIdleTimeout{30};
constexpr std::chrono::seconds kTimeoutScanInterval{1};
constexpr std::size_t kMaxRequestsPerConnection = 100;
constexpr std::size_t kSendfileThreshold = 256 * 1024;
constexpr std::size_t kMaxStaticFileSize = 16 * 1024 * 1024;

namespace fs = std::filesystem;

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }

    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

std::string currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    if (::localtime_r(&time, &local_time) == nullptr) {
        return "unknown";
    }

    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) %
        1000;
    std::ostringstream timestamp;
    timestamp << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3)
              << milliseconds.count();
    return timestamp.str();
}

std::string clientIpAddress(const sockaddr_storage& address) {
    char text[INET6_ADDRSTRLEN]{};
    const void* source = nullptr;

    if (address.ss_family == AF_INET) {
        source = &reinterpret_cast<const sockaddr_in*>(&address)->sin_addr;
    } else if (address.ss_family == AF_INET6) {
        source = &reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr;
    } else {
        return "unknown";
    }

    if (::inet_ntop(address.ss_family, source, text, sizeof(text)) == nullptr) {
        return "unknown";
    }
    return text;
}

std::string buildResponseHeader(int status_code,
                                const std::string& reason,
                                const std::string& content_type,
                                std::size_t content_length,
                                bool keep_alive) {
    return "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n"
           "Content-Type: " + content_type + "\r\n"
           "Content-Length: " + std::to_string(content_length) + "\r\n"
           "Connection: " +
           std::string(keep_alive ? "keep-alive" : "close") + "\r\n"
           "\r\n";
}

std::string buildResponse(int status_code,
                          const std::string& reason,
                          const std::string& content_type,
                          const std::string& body,
                          bool keep_alive) {
    return buildResponseHeader(status_code, reason, content_type,
                               body.size(), keep_alive) +
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

std::optional<std::string> readFile(int fd, std::size_t file_size) {
    std::string content(file_size, '\0');
    std::size_t total_read = 0;

    while (total_read < content.size()) {
        const ssize_t count = ::read(
            fd, content.data() + total_read, content.size() - total_read);
        if (count > 0) {
            total_read += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return std::nullopt;
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

std::string buildResponse(const HttpRequest& request,
                          bool& keep_alive,
                          int& status_code,
                          std::optional<SocketFd>& response_file,
                          std::size_t& response_file_size) {
    response_file.reset();
    response_file_size = 0;

    if (request.method != "GET") {
        status_code = 405;
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
        status_code = 400;
        return buildErrorResponse(400, "Bad Request", keep_alive);
    }

    std::error_code error;
    const fs::path public_root = fs::weakly_canonical("public", error);
    if (error) {
        status_code = 500;
        keep_alive = false;
        return buildErrorResponse(500, "Internal Server Error", false);
    }

    const fs::path relative_path = request_path.substr(1);
    const fs::path file_path =
        fs::weakly_canonical(public_root / relative_path, error);
    if (error || !isWithinPublicRoot(public_root, file_path)) {
        status_code = 403;
        return buildErrorResponse(403, "Forbidden", keep_alive);
    }

    SocketFd opened_file(
        ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC));
    struct stat file_stat{};
    if (!opened_file.valid() ||
        ::fstat(opened_file.get(), &file_stat) < 0 ||
        !S_ISREG(file_stat.st_mode) ||
        file_stat.st_size < 0) {
        status_code = 404;
        return buildErrorResponse(404, "Not Found", keep_alive);
    }

    const auto unsigned_file_size =
        static_cast<std::uintmax_t>(file_stat.st_size);
    if (unsigned_file_size > kMaxStaticFileSize) {
        status_code = 404;
        return buildErrorResponse(404, "Not Found", keep_alive);
    }
    const std::size_t file_size =
        static_cast<std::size_t>(unsigned_file_size);

    status_code = 200;
    if (file_size >= kSendfileThreshold) {
        response_file = std::move(opened_file);
        response_file_size = file_size;
        return buildResponseHeader(
            200, "OK", contentTypeFor(file_path),
            file_size, keep_alive);
    }

    std::optional<std::string> content =
        readFile(opened_file.get(), file_size);
    if (!content) {
        status_code = 500;
        keep_alive = false;
        return buildErrorResponse(
            500, "Internal Server Error", false);
    }

    return buildResponse(200, "OK", contentTypeFor(file_path),
                         *content, keep_alive);
}

}  // namespace

TcpServer::TcpServer(std::uint16_t port) : port_(port) {}

TcpServer::~TcpServer() {
    shutdown();
}

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
    if (!createSocket() || !bindPort() || !listenConnections()) {
        return false;
    }
    if (!access_logger_.start()) {
        std::cerr << "failed to start access logger\n";
        return false;
    }
    return true;
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

    sigset_t signal_mask;
    if (::sigemptyset(&signal_mask) < 0 ||
        ::sigaddset(&signal_mask, SIGINT) < 0 ||
        ::sigaddset(&signal_mask, SIGTERM) < 0) {
        printError("prepare signalfd mask");
        return;
    }

    SocketFd signal_socket(::signalfd(
        -1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!signal_socket.valid()) {
        printError("signalfd");
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

    epoll_event signal_event{};
    signal_event.events = EPOLLIN;
    signal_event.data.fd = signal_socket.get();
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_socket.get(),
                    &signal_event) < 0) {
        printError("epoll_ctl ADD signalfd");
        return;
    }

    constexpr int kMaxEvents = 64;
    std::vector<epoll_event> events(kMaxEvents);

    bool running = true;
    while (running) {
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

            if (fd == signal_socket.get()) {
                bool stop_requested = false;
                while (true) {
                    signalfd_siginfo signal_info{};
                    const ssize_t count = ::read(
                        signal_socket.get(), &signal_info,
                        sizeof(signal_info));
                    if (count ==
                        static_cast<ssize_t>(sizeof(signal_info))) {
                        if (signal_info.ssi_signo == SIGINT ||
                            signal_info.ssi_signo == SIGTERM) {
                            stop_requested = true;
                        }
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
                        printError("read signalfd");
                    } else {
                        std::cerr
                            << "signalfd returned an invalid read size\n";
                    }
                    stop_requested = true;
                    break;
                }

                if (stop_requested) {
                    stopping_.store(true, std::memory_order_release);
                    if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
                                    listen_socket_.get(), nullptr) < 0 &&
                        errno != ENOENT) {
                        printError("epoll_ctl DEL listen");
                    }
                    running = false;
                    break;
                }
                continue;
            }

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
                    sockaddr_storage client_address{};
                    socklen_t client_address_length = sizeof(client_address);
                    SocketFd client_socket(::accept4(
                        listen_socket_.get(),
                        reinterpret_cast<sockaddr*>(&client_address),
                        &client_address_length,
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
                            client_fd, std::move(client_socket),
                            clientIpAddress(client_address));
                        inserted = result.second;
                        if (inserted) {
                            result.first->second.last_active =
                                std::chrono::steady_clock::now();
                            metrics_.total_connections.fetch_add(
                                1, std::memory_order_relaxed);
                            metrics_.active_connections.fetch_add(
                                1, std::memory_order_relaxed);
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
    shutdown();
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
                const auto now = std::chrono::steady_clock::now();
                connection->second.last_active = now;
                if (!connection->second.request_started_set) {
                    connection->second.request_started = now;
                    connection->second.request_started_set = true;
                }

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
            false, 400, "-", "-");
        return;
    }

    if (request_ready) {
        std::cout << "Request:\n" << raw_request << '\n';

        HttpRequest request;
        const ParseResult result = parseRequest(raw_request, request);
        bool keep_alive = false;
        int status_code = 400;
        std::string method = "-";
        std::string path = "-";
        std::string response;
        std::optional<SocketFd> response_file;
        std::size_t response_file_size = 0;
        if (result != ParseResult::Complete) {
            response = buildErrorResponse(400, "Bad Request", false);
        } else {
            metrics_.total_requests.fetch_add(
                1, std::memory_order_relaxed);
            keep_alive = request.keep_alive && !pipelined_request &&
                         requests_served + 1 < kMaxRequestsPerConnection;
            method = request.method;
            path = request.path;

            std::string route_path = request.path;
            const std::size_t query_position = route_path.find('?');
            if (query_position != std::string::npos) {
                route_path.erase(query_position);
            }

            if (request.method == "GET" && route_path == "/metrics") {
                status_code = 200;
                response = buildResponse(
                    200, "OK", "text/plain; charset=UTF-8",
                    buildMetricsBody(), keep_alive);
            } else {
                response = buildResponse(
                    request, keep_alive, status_code,
                    response_file, response_file_size);
            }
        }

        queueResponse(epoll_fd, fd, std::move(response), keep_alive,
                      status_code, std::move(method), std::move(path),
                      std::move(response_file), response_file_size);
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
        ssize_t buffer_count = 0;
        int write_error = 0;
        bool header_pending = false;
        bool send_file = false;
        bool finished = false;
        bool invalid_state = false;
        int input_fd = -1;
        off_t file_offset = 0;
        std::size_t file_remaining = 0;

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
            } else if (state.bytes_sent < state.output_buffer.size()) {
                header_pending = true;
                const std::size_t remaining =
                    state.output_buffer.size() - state.bytes_sent;
                buffer_count = ::send(
                    fd,
                    state.output_buffer.data() + state.bytes_sent,
                    remaining,
                    MSG_NOSIGNAL);
                if (buffer_count > 0) {
                    state.bytes_sent +=
                        static_cast<std::size_t>(buffer_count);
                    state.last_active =
                        std::chrono::steady_clock::now();
                } else if (buffer_count < 0) {
                    write_error = errno;
                }
            } else if (state.write_mode == WriteMode::File) {
                if (!state.file_fd || !state.file_fd->valid()) {
                    invalid_state = true;
                } else if (state.file_remaining == 0) {
                    state.file_fd.reset();
                    state.file_offset = 0;
                    state.write_mode = WriteMode::Buffer;
                    finished = true;
                } else {
                    send_file = true;
                    input_fd = state.file_fd->get();
                    file_offset = state.file_offset;
                    file_remaining = state.file_remaining;
                }
            } else {
                finished = true;
            }
        }

        if (buffer_count > 0) {
            metrics_.total_bytes_sent.fetch_add(
                static_cast<std::uint64_t>(buffer_count),
                std::memory_order_relaxed);
            continue;
        }

        if (invalid_state) {
            closeConnection(epoll_fd, fd);
            return;
        }

        if (header_pending) {
            if (buffer_count == 0) {
                closeConnection(epoll_fd, fd);
                return;
            }
            if (write_error == EINTR) {
                continue;
            }
            if (write_error == EAGAIN || write_error == EWOULDBLOCK) {
                rearmWrite(epoll_fd, fd);
                return;
            }
            if (write_error != EPIPE && write_error != ECONNRESET) {
                errno = write_error;
                printError("send");
            }
            closeConnection(epoll_fd, fd);
            return;
        }

        if (send_file) {
            off_t updated_offset = file_offset;
            const ssize_t file_count = ::sendfile(
                fd, input_fd, &updated_offset, file_remaining);

            if (file_count > 0) {
                bool update_failed = false;
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    const auto connection = connections_.find(fd);
                    if (connection == connections_.end()) {
                        return;
                    }

                    Connection& state = connection->second;
                    const std::size_t sent =
                        static_cast<std::size_t>(file_count);
                    if (state.state != ConnectionState::Writing ||
                        state.write_mode != WriteMode::File ||
                        !state.file_fd ||
                        state.file_fd->get() != input_fd ||
                        sent > state.file_remaining) {
                        update_failed = true;
                    } else {
                        state.file_offset = updated_offset;
                        state.file_remaining -= sent;
                        state.last_active =
                            std::chrono::steady_clock::now();
                    }
                }

                if (update_failed) {
                    closeConnection(epoll_fd, fd);
                    return;
                }
                metrics_.total_bytes_sent.fetch_add(
                    static_cast<std::uint64_t>(file_count),
                    std::memory_order_relaxed);
                continue;
            }

            if (file_count == 0) {
                std::cerr
                    << "sendfile reached EOF before the expected file size\n";
                metrics_.server_errors.fetch_add(
                    1, std::memory_order_relaxed);
                closeConnection(epoll_fd, fd);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                rearmWrite(epoll_fd, fd);
                return;
            }
            if (errno != EPIPE && errno != ECONNRESET) {
                printError("sendfile");
            }
            closeConnection(epoll_fd, fd);
            return;
        }

        if (!finished) {
            closeConnection(epoll_fd, fd);
            return;
        }

        bool reuse_connection = false;
        bool log_ready = false;
        AccessLog access_log;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            const auto connection = connections_.find(fd);
            if (connection == connections_.end()) {
                return;
            }

            Connection& state = connection->second;
            const auto completed_at = std::chrono::steady_clock::now();
            if (state.response_status_code != 0) {
                access_log.client_ip = state.client_ip;
                access_log.method = state.log_method;
                access_log.path = state.log_path;
                access_log.status_code = state.response_status_code;
                access_log.response_bytes = state.response_bytes;
                if (state.request_started_set) {
                    access_log.duration_us =
                        std::chrono::duration_cast<
                            std::chrono::microseconds>(
                            completed_at - state.request_started)
                            .count();
                }
                state.response_status_code = 0;
                log_ready = true;
            }

            ++state.requests_served;
            reuse_connection =
                state.keep_alive &&
                state.requests_served < kMaxRequestsPerConnection;

            if (reuse_connection) {
                state.input_buffer.clear();
                state.output_buffer.clear();
                state.bytes_sent = 0;
                state.write_mode = WriteMode::Buffer;
                state.file_fd.reset();
                state.file_offset = 0;
                state.file_remaining = 0;
                state.state = ConnectionState::Reading;
                state.keep_alive = false;
                state.log_method = "-";
                state.log_path = "-";
                state.response_bytes = 0;
                state.request_started_set = false;
                state.last_active = completed_at;
            }
        }

        if (log_ready) {
            access_log.timestamp = currentTimestamp();
            access_logger_.log(std::move(access_log));
        }

        if (reuse_connection) {
            rearmRead(epoll_fd, fd);
        } else {
            closeConnection(epoll_fd, fd);
        }
        return;
    }
}

void TcpServer::queueResponse(int epoll_fd,
                              int fd,
                              std::string response,
                              bool keep_alive,
                              int status_code,
                              std::string method,
                              std::string path,
                              std::optional<SocketFd> file_fd,
                              std::size_t file_size) {
    bool found = false;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        const auto connection = connections_.find(fd);
        if (connection != connections_.end()) {
            found = true;
            Connection& state = connection->second;
            const bool valid_file_state =
                !file_fd || (file_fd->valid() && file_size > 0);
            if (state.state == ConnectionState::Reading &&
                valid_file_state) {
                state.output_buffer = std::move(response);
                state.bytes_sent = 0;
                state.write_mode =
                    file_fd ? WriteMode::File : WriteMode::Buffer;
                state.file_fd = std::move(file_fd);
                state.file_offset = 0;
                state.file_remaining = file_size;
                state.state = ConnectionState::Writing;
                state.keep_alive = keep_alive;
                state.log_method = std::move(method);
                state.log_path = std::move(path);
                state.response_status_code = status_code;
                state.response_bytes =
                    state.output_buffer.size() + file_size;
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

    if (status_code == 400 || status_code == 404 ||
        status_code == 405) {
        metrics_.client_errors.fetch_add(
            1, std::memory_order_relaxed);
    } else if (status_code >= 500) {
        metrics_.server_errors.fetch_add(
            1, std::memory_order_relaxed);
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

        if (stopping_.load(std::memory_order_acquire)) {
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

        if (stopping_.load(std::memory_order_acquire)) {
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

    metrics_.active_connections.fetch_sub(
        1, std::memory_order_relaxed);

    if (epoll_fd >= 0 &&
        ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0 &&
        errno != ENOENT) {
        printError("epoll_ctl DEL client");
    }
}

void TcpServer::closeAllConnections(int epoll_fd) {
    std::vector<int> client_fds;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        client_fds.reserve(connections_.size());
        for (const auto& connection : connections_) {
            client_fds.push_back(connection.first);
        }
    }

    for (const int fd : client_fds) {
        closeConnection(epoll_fd, fd);
    }
}

std::string TcpServer::buildMetricsBody() const {
    std::ostringstream output;
    output
        << "tcp_server_total_requests "
        << metrics_.total_requests.load(std::memory_order_relaxed)
        << '\n'
        << "tcp_server_active_connections "
        << metrics_.active_connections.load(std::memory_order_relaxed)
        << '\n'
        << "tcp_server_total_connections "
        << metrics_.total_connections.load(std::memory_order_relaxed)
        << '\n'
        << "tcp_server_bytes_sent_total "
        << metrics_.total_bytes_sent.load(std::memory_order_relaxed)
        << '\n'
        << "tcp_server_client_errors_total "
        << metrics_.client_errors.load(std::memory_order_relaxed)
        << '\n'
        << "tcp_server_server_errors_total "
        << metrics_.server_errors.load(std::memory_order_relaxed)
        << '\n';
    return output.str();
}

void TcpServer::shutdown() {
    std::call_once(shutdown_once_, [this] {
        stopping_.store(true, std::memory_order_release);

        thread_pool_.shutdown();
        closeAllConnections(epoll_socket_.get());
        access_logger_.stop();

        epoll_socket_.reset();
        listen_socket_.reset();
    });
}

void TcpServer::printError(const char* operation) {
    std::cerr << operation << " failed: " << std::strerror(errno) << '\n';
}
