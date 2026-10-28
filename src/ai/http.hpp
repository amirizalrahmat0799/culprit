#pragma once
// HTTP requests to AI providers, through the system's own HTTP stack so culprit stays one file with
// nothing to install: libcurl on Linux and macOS (loaded when first needed; it's part of macOS and of
// almost every Linux system), and WinHTTP on Windows. Both handle HTTPS, proxies (HTTPS_PROXY on
// Linux/macOS, the system proxy on Windows) and certificates the way the system does.

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace culprit::ai {

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_seconds = 180;
};

struct HttpResponse {
    int status = 0;     // 0 when there was no response
    std::string body;
    std::string error;  // why there was no response: can't connect, timed out, cancelled
};

class Transport {
public:
    virtual ~Transport() = default;
    /// Sends the request and waits for the whole answer. Setting `*cancel` stops it early.
    virtual HttpResponse send(const HttpRequest& request, const std::atomic<bool>* cancel) = 0;
};

/// The real one. Never null; if the system has no HTTP library, every request fails with an explanation.
std::unique_ptr<Transport> system_transport();

}  // namespace culprit::ai
