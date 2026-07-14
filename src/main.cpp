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

void handleSignal(int) {
    g_running = 0;
}

bool installSignalHandler(int signalNumber) {
    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return ::sigaction(signalNumber, &action, nullptr) == 0;
}

class TcpServer {
public:
    explicit TcpServer(std::uint16_t port) : port_(port) {}

    ~TcpServer() {
        closeSocket(serverFd_);
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 1. 创建 Socket。
    bool createSocket() {
        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ < 0) {
            printError("socket");
            return false;
        }

        int reuseAddress = 1;
        if (::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR,
                         &reuseAddress, sizeof(reuseAddress)) < 0) {
            printError("setsockopt");
            closeSocket(serverFd_);
            return false;
        }
        return true;
    }

    // 2. 将 Socket 绑定到本机端口。
    bool bindPort() const {
        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        serverAddress.sin_port = htons(port_);

        if (::bind(serverFd_, reinterpret_cast<sockaddr*>(&serverAddress),
                   sizeof(serverAddress)) < 0) {
            printError("bind");
            return false;
        }
        return true;
    }

    // 3. 开始监听连接请求。
    bool listenConnections() const {
        if (::listen(serverFd_, SOMAXCONN) < 0) {
            printError("listen");
            return false;
        }

        std::cout << "TCP server is listening on 0.0.0.0:" << port_
                  << " (press Ctrl+C to stop)" << std::endl;
        return true;
    }

    // 4. 接收一个客户端连接，返回客户端文件描述符。
    int acceptClient() const {
        sockaddr_in clientAddress{};
        socklen_t addressLength = sizeof(clientAddress);

        const int clientFd = ::accept(
            serverFd_, reinterpret_cast<sockaddr*>(&clientAddress),
            &addressLength);

        if (clientFd < 0) {
            if (errno != EINTR) {
                printError("accept");
            }
            return -1;
        }

        char clientIp[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &clientAddress.sin_addr,
                    clientIp, sizeof(clientIp));
        std::cout << "Accepted connection from " << clientIp << ':'
                  << ntohs(clientAddress.sin_port) << std::endl;
        return clientFd;
    }

    // 5. 从客户端读取一次消息。
    std::string readClientMessage(int clientFd) const {
        char buffer[1024]{};
        const ssize_t bytesRead = ::recv(clientFd, buffer,
                                         sizeof(buffer) - 1, 0);

        if (bytesRead < 0) {
            printError("recv");
            return {};
        }
        if (bytesRead == 0) {
            std::cout << "Client closed the connection." << std::endl;
            return {};
        }

        return std::string(buffer, static_cast<std::size_t>(bytesRead));
    }

    // 6. 关闭文件描述符。传引用可避免重复关闭同一个描述符。
    static void closeSocket(int& fd) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    bool start() {
        return createSocket() && bindPort() && listenConnections();
    }

    void run() const {
        while (g_running) {
            int clientFd = acceptClient();
            if (clientFd < 0) {
                continue;
            }

            const std::string message = readClientMessage(clientFd);
            if (!message.empty()) {
                std::cout << "Received: " << message;

                const std::string reply = "Server received: " + message;
                if (::send(clientFd, reply.data(), reply.size(), 0) < 0) {
                    printError("send");
                }
            }

            closeSocket(clientFd);
        }
    }

private:
    static void printError(const char* operation) {
        std::cerr << operation << " failed: " << std::strerror(errno) << '\n';
    }

    std::uint16_t port_;
    int serverFd_ = -1;
};

bool parsePort(const char* text, std::uint16_t& port) {
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
    if (argc > 2 || (argc == 2 && !parsePort(argv[1], port))) {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 1;
    }

    if (!installSignalHandler(SIGINT) || !installSignalHandler(SIGTERM)) {
        std::cerr << "sigaction failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    TcpServer server(port);
    if (!server.start()) {
        return 1;
    }

    server.run();
    std::cout << "Server stopped." << std::endl;
    return 0;
}
