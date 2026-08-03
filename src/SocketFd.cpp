#include "SocketFd.h"

#include <cstdio>

#include <unistd.h>
#include <utility>

SocketFd::SocketFd(int fd) : fd_(fd) {}

SocketFd::~SocketFd() {
    reset();
}

SocketFd::SocketFd(SocketFd&& other) noexcept
    : fd_(other.release()) {}

SocketFd& SocketFd::operator=(SocketFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int SocketFd::get() const {
    return fd_;
}

bool SocketFd::valid() const {
    return fd_ >= 0;
}

int SocketFd::release() {
    return std::exchange(fd_, -1);
}

void SocketFd::reset(int new_fd) {
    if (fd_ == new_fd) {
        return;
    }

    const int old_fd = std::exchange(fd_, new_fd);
    if (old_fd >= 0 && ::close(old_fd) < 0) {
        std::perror("close");
    }
}
