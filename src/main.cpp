#include "TcpServer.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include <pthread.h>
#include <signal.h>

namespace {

constexpr std::uint16_t kDefaultPort = 8080;
constexpr unsigned long kMaxPort = 65535;

bool parsePort(const char* text, std::uint16_t& port) {
    try {
        const std::string value(text);
        std::size_t parsed = 0;
        const unsigned long number = std::stoul(value, &parsed);
        if (parsed != value.size() || number == 0 || number > kMaxPort) {
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
    std::uint16_t port = kDefaultPort;
    if (argc > 2 || (argc == 2 && !parsePort(argv[1], port))) {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 1;
    }

    sigset_t signal_mask;
    if (::sigemptyset(&signal_mask) < 0 ||
        ::sigaddset(&signal_mask, SIGINT) < 0 ||
        ::sigaddset(&signal_mask, SIGTERM) < 0) {
        std::cerr << "failed to prepare signal mask: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    const int mask_error = ::pthread_sigmask(
        SIG_BLOCK, &signal_mask, nullptr);
    if (mask_error != 0) {
        std::cerr << "failed to block signals: "
                  << std::strerror(mask_error) << '\n';
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
