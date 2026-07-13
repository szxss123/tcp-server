#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_running = 1;

void handle_signal(int) {
    g_running = 0;
}

bool install_signal_handler(int signal_number) {
    struct sigaction action {};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return ::sigaction(signal_number, &action, nullptr) == 0;
}

class Socket {
public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

bool parse_port(const char* text, std::uint16_t& port) {
    try {
        const std::string value(text);
        std::size_t parsed = 0;
        const unsigned long number = std::stoul(value, &parsed);
        if (parsed != value.size() || number == 0 || number > 65535) {
            return false;
        }
        port = static_cast<std::uint16_t>(number);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::uint16_t port = 8080;
    if (argc > 2 || (argc == 2 && !parse_port(argv[1], port))) {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 1;
    }

    if (!install_signal_handler(SIGINT) || !install_signal_handler(SIGTERM)) {
        std::cerr << "sigaction failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    Socket server(::socket(AF_INET, SOCK_STREAM, 0));
    if (server.get() < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    int reuse_address = 1;
    if (::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse_address, sizeof(reuse_address)) < 0) {
        std::cerr << "setsockopt failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(server.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    if (::listen(server.get(), SOMAXCONN) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    std::cout << "TCP server is listening on 0.0.0.0:" << port
              << " (press Ctrl+C to stop)" << std::endl;

    while (g_running) {
        sockaddr_in client_address{};
        socklen_t client_address_length = sizeof(client_address);
        Socket client(::accept(server.get(),
                               reinterpret_cast<sockaddr*>(&client_address),
                               &client_address_length));

        if (client.get() < 0) {
            if (errno == EINTR && !g_running) {
                break;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            continue;
        }

        char client_ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &client_address.sin_addr,
                    client_ip, sizeof(client_ip));
        std::cout << "Accepted connection from " << client_ip << ':'
                  << ntohs(client_address.sin_port) << std::endl;

        const std::string message = "Hello from TCP server!\n";
        const ssize_t sent = ::send(client.get(), message.data(), message.size(), 0);
        if (sent < 0) {
            std::cerr << "send failed: " << std::strerror(errno) << '\n';
        }
    }

    std::cout << "Server stopped." << std::endl;
    return 0;
}
