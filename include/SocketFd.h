#pragma once

class SocketFd {
public:
    SocketFd() = default;
    explicit SocketFd(int fd);
    ~SocketFd();

    SocketFd(const SocketFd&) = delete;
    SocketFd& operator=(const SocketFd&) = delete;

    SocketFd(SocketFd&& other) noexcept;
    SocketFd& operator=(SocketFd&& other) noexcept;

    int get() const;
    bool valid() const;
    int release();
    void reset(int new_fd = -1);

private:
    int fd_{-1};
};
