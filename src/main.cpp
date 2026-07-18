#include "TcpServer.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

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

    if (!installSignalHandlers()) {
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