#include "ai/http.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace culprit::ai {
namespace {

#ifndef _WIN32

// ---- libcurl, loaded at run time -----------------------------------------------------------------------
// Only the stable part of libcurl's ABI is used: these option numbers haven't changed in 20 years.

using CURL = void;
struct curl_slist;
constexpr int CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_TIMEOUT = 13, CURLOPT_POSTFIELDS = 10015,
              CURLOPT_HTTPHEADER = 10023, CURLOPT_CUSTOMREQUEST = 10036, CURLOPT_NOPROGRESS = 43,
              CURLOPT_FOLLOWLOCATION = 52, CURLOPT_XFERINFODATA = 10057, CURLOPT_CONNECTTIMEOUT = 78,
              CURLOPT_HTTPGET = 80, CURLOPT_NOSIGNAL = 99, CURLOPT_ACCEPT_ENCODING = 10102,
              CURLOPT_WRITEFUNCTION = 20011, CURLOPT_XFERINFOFUNCTION = 20219, CURLOPT_POSTFIELDSIZE_LARGE = 30120;
constexpr int CURLINFO_RESPONSE_CODE = 0x200000 + 2;
constexpr int CURLE_OK = 0, CURLE_ABORTED_BY_CALLBACK = 42, CURLE_OPERATION_TIMEDOUT = 28;
constexpr long CURL_GLOBAL_DEFAULT = 3;

struct Curl {
    void* lib = nullptr;
    int (*global_init)(long) = nullptr;
    CURL* (*easy_init)() = nullptr;
    int (*easy_setopt)(CURL*, int, ...) = nullptr;
    int (*easy_perform)(CURL*) = nullptr;
    int (*easy_getinfo)(CURL*, int, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;

    bool load() {
        for (const char* name : {"libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", "libcurl-nss.so.4",
                                 "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib"}) {
            lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (lib) break;
        }
        if (!lib) return false;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };
        bool ok = sym(global_init, "curl_global_init") && sym(easy_init, "curl_easy_init") &&
                  sym(easy_setopt, "curl_easy_setopt") && sym(easy_perform, "curl_easy_perform") &&
                  sym(easy_getinfo, "curl_easy_getinfo") && sym(easy_cleanup, "curl_easy_cleanup") &&
                  sym(easy_strerror, "curl_easy_strerror") && sym(slist_append, "curl_slist_append") &&
                  sym(slist_free_all, "curl_slist_free_all");
        if (ok) global_init(CURL_GLOBAL_DEFAULT);
        return ok;
    }
};

Curl* curl() {
    static Curl c;
    static bool loaded = c.load();
    return loaded ? &c : nullptr;
}

size_t on_write(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

int on_progress(void* user, int64_t, int64_t, int64_t, int64_t) {
    auto* cancel = static_cast<const std::atomic<bool>*>(user);
    return cancel && cancel->load() ? 1 : 0;  // non-zero aborts the transfer
}

class CurlTransport : public Transport {
public:
    HttpResponse send(const HttpRequest& r, const std::atomic<bool>* cancel) override {
        HttpResponse out;
        static std::mutex init;  // curl_global_init and the first dlopen aren't thread-safe
        Curl* c;
        {
            std::lock_guard<std::mutex> lock(init);
            c = curl();
        }
        if (!c) {
            out.error = "libcurl isn't installed; install curl (e.g. apt install libcurl4) to use AI providers";
            return out;
        }
        CURL* h = c->easy_init();
        if (!h) {
            out.error = "libcurl couldn't start a request";
            return out;
        }
        curl_slist* headers = nullptr;
        for (const auto& [k, v] : r.headers) headers = c->slist_append(headers, (k + ": " + v).c_str());
        headers = c->slist_append(headers, "Expect:");  // no 100-continue round trip for big bodies

        c->easy_setopt(h, CURLOPT_URL, r.url.c_str());
        c->easy_setopt(h, CURLOPT_HTTPHEADER, headers);
        c->easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        c->easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
        c->easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(r.timeout_seconds));
        c->easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        c->easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
        c->easy_setopt(h, CURLOPT_WRITEFUNCTION, &on_write);
        c->easy_setopt(h, CURLOPT_WRITEDATA, &out.body);
        c->easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        c->easy_setopt(h, CURLOPT_XFERINFOFUNCTION, &on_progress);
        c->easy_setopt(h, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
        if (r.method == "GET") {
            c->easy_setopt(h, CURLOPT_HTTPGET, 1L);
        } else {
            c->easy_setopt(h, CURLOPT_POSTFIELDS, r.body.data());
            c->easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<int64_t>(r.body.size()));
            if (r.method != "POST") c->easy_setopt(h, CURLOPT_CUSTOMREQUEST, r.method.c_str());
        }

        int rc = c->easy_perform(h);
        if (rc == CURLE_OK) {
            long status = 0;
            c->easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
            out.status = static_cast<int>(status);
        } else if (rc == CURLE_ABORTED_BY_CALLBACK) {
            out.error = "cancelled";
        } else if (rc == CURLE_OPERATION_TIMEDOUT) {
            out.error = "no answer within " + std::to_string(r.timeout_seconds) + " seconds";
        } else {
            out.error = c->easy_strerror(rc);
        }
        c->slist_free_all(headers);
        c->easy_cleanup(h);
        return out;
    }
};

#else

// ---- WinHTTP ---------------------------------------------------------------------------------------------

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string error_text(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: return "no answer in time";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "can't connect";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the host name can't be resolved";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was closed";
        case ERROR_WINHTTP_SECURE_FAILURE: return "the TLS certificate couldn't be verified";
        case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
        default: return "WinHTTP error " + std::to_string(code);
    }
}

class WinTransport : public Transport {
public:
    HttpResponse send(const HttpRequest& r, const std::atomic<bool>* cancel) override {
        HttpResponse out;
        std::wstring url = wide(r.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        wchar_t host[256] = {}, path[4096] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 255;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 4095;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
            out.error = "not a valid URL: " + r.url;
            return out;
        }
        HINTERNET session = WinHttpOpen(L"culprit", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            out.error = error_text(GetLastError());
            return out;
        }
        int ms = r.timeout_seconds * 1000;
        WinHttpSetTimeouts(session, 15000, 15000, ms, ms);
        HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, wide(r.method).c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr;
        if (!request) {
            out.error = error_text(GetLastError());
            if (connect) WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return out;
        }
        // cancelling closes the request handle, which makes the waiting call below return
        std::atomic<bool> done{false}, closed{false};
        std::thread watcher([&] {
            while (!done) {
                if (cancel && cancel->load()) {
                    closed = true;
                    WinHttpCloseHandle(request);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        std::wstring headers;
        for (const auto& [k, v] : r.headers) headers += wide(k + ": " + v + "\r\n");
        BOOL ok = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                     headers.empty() ? 0 : static_cast<DWORD>(-1L), const_cast<char*>(r.body.data()),
                                     static_cast<DWORD>(r.body.size()), static_cast<DWORD>(r.body.size()), 0) &&
                  WinHttpReceiveResponse(request, nullptr);
        if (ok) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            out.status = static_cast<int>(status);
            while (true) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                out.body.append(chunk.data(), read);
            }
        } else {
            out.error = cancel && cancel->load() ? "cancelled" : error_text(GetLastError());
        }
        done = true;
        watcher.join();
        if (!closed) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return out;
    }
};

#endif

}  // namespace

std::unique_ptr<Transport> system_transport() {
#ifdef _WIN32
    return std::make_unique<WinTransport>();
#else
    return std::make_unique<CurlTransport>();
#endif
}

}  // namespace culprit::ai
