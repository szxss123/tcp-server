#include "AsyncLogger.h"

#include <system_error>
#include <utility>

AsyncLogger::AsyncLogger(std::string file_path,
                         std::size_t max_queue_size)
    : file_path_(std::move(file_path)),
      max_queue_size_(max_queue_size) {}

AsyncLogger::~AsyncLogger() {
    stop();
}

bool AsyncLogger::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return true;
    }
    if (stopping_) {
        return false;
    }

    output_.open(file_path_, std::ios::app);
    if (!output_) {
        return false;
    }

    try {
        worker_ = std::thread(&AsyncLogger::run, this);
        started_ = true;
    } catch (const std::system_error&) {
        output_.close();
        return false;
    }
    return true;
}

bool AsyncLogger::log(AccessLog entry) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopping_ ||
            queue_.size() >= max_queue_size_) {
            ++dropped_logs_;
            return false;
        }
        queue_.push(std::move(entry));
    }

    condition_.notify_one();
    return true;
}

void AsyncLogger::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stopping_ = true;
    }

    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    output_.flush();
    output_.close();

    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void AsyncLogger::run() {
    while (true) {
        std::queue<AccessLog> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });

            if (stopping_ && queue_.empty()) {
                break;
            }
            batch.swap(queue_);
        }

        while (!batch.empty()) {
            const AccessLog& entry = batch.front();
            output_ << entry.timestamp << '\t'
                    << entry.client_ip << '\t'
                    << entry.method << '\t'
                    << entry.path << '\t'
                    << entry.status_code << '\t'
                    << entry.response_bytes << '\t'
                    << entry.duration_us << '\n';
            batch.pop();
        }
        output_.flush();
    }
}