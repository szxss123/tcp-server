#include "HttpRequest.h"

#include <cctype>
#include <string_view>
#include <utility>

namespace {

std::string trim(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string normalizeHeader(std::string_view value) {
    std::string normalized = trim(value);
    for (char& character : normalized) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return normalized;
}

}  // namespace

ParseResult parseRequest(const std::string& raw_request,
                         HttpRequest& request) {
    const std::size_t header_end = raw_request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return ParseResult::Incomplete;
    }

    const std::size_t request_line_end = raw_request.find("\r\n");
    if (request_line_end == std::string::npos || request_line_end == 0) {
        return ParseResult::Invalid;
    }

    const std::string_view request_line(raw_request.data(), request_line_end);
    const std::size_t first_space = request_line.find(' ');
    const std::size_t second_space =
        first_space == std::string_view::npos
            ? std::string_view::npos
            : request_line.find(' ', first_space + 1);

    if (first_space == std::string_view::npos ||
        second_space == std::string_view::npos ||
        request_line.find(' ', second_space + 1) != std::string_view::npos) {
        return ParseResult::Invalid;
    }

    HttpRequest parsed;
    parsed.method = std::string(request_line.substr(0, first_space));
    parsed.path = std::string(request_line.substr(
        first_space + 1, second_space - first_space - 1));
    parsed.version = std::string(request_line.substr(second_space + 1));

    if (parsed.method.empty() || parsed.path.empty() ||
        parsed.path.front() != '/' ||
        (parsed.version != "HTTP/1.1" && parsed.version != "HTTP/1.0")) {
        return ParseResult::Invalid;
    }

    parsed.keep_alive = parsed.version == "HTTP/1.1";

    std::size_t line_start = request_line_end + 2;
    while (line_start < header_end) {
        const std::size_t line_end = raw_request.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end > header_end) {
            return ParseResult::Invalid;
        }

        const std::string_view line(raw_request.data() + line_start,
                                    line_end - line_start);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return ParseResult::Invalid;
        }

        const std::string name = normalizeHeader(line.substr(0, colon));
        if (name.empty()) {
            return ParseResult::Invalid;
        }
        const std::string value = normalizeHeader(line.substr(colon + 1));
        parsed.headers[name] = value;

        if (name == "connection") {
            if (value == "close") {
                parsed.keep_alive = false;
            } else if (value == "keep-alive") {
                parsed.keep_alive = true;
            }
        }
        line_start = line_end + 2;
    }

    request = std::move(parsed);
    return ParseResult::Complete;
}
