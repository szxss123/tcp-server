#pragma once

#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

struct AccessLog {
    std::string timestamp;
    std::string client_ip;
    std::string method;
    std::string path;
    int status_code{0};
    std::size_t response_bytes{0};
    long long duration_us{0};
};

class AsyncLogger {
public:
    AsyncLogger(std::string file_path, std::size_t max_queue_size);
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    bool start();
    bool log(AccessLog entry);
    void stop();

private:
    void run();

    std::string file_path_;
    std::ofstream output_;
    std::queue<AccessLog> queue_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;

    std::size_t max_queue_size_;
    std::size_t dropped_logs_{0};
    bool started_{false};
    bool stopping_{false};
};