#pragma once

#include <string>
#include <unordered_map>

enum class ParseResult {
    Complete,
    Incomplete,
    Invalid,
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
};

ParseResult parseRequest(const std::string& raw_request, HttpRequest& request);
