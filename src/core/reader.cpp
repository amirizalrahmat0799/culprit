#include "core/reader.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/parser.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace culprit {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr auto kPoll = 200ms;   // how often a followed file is checked for new lines
constexpr size_t kBatch = 2000; // entries handed over at once while reading a big file

// Splits incoming bytes into lines, keeping an unfinished last line for later.
class LineSplitter {
public:
    template <typename F>
    void push(const char* data, size_t n, F&& on_line) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (data[i] != '\n') continue;
            if (partial_.empty()) {
                on_line(std::string_view(data + start, i - start));
            } else {
                partial_.append(data + start, i - start);
                on_line(std::string_view(partial_));
                partial_.clear();
            }
            start = i + 1;
        }
        partial_.append(data + start, n - start);
    }
    template <typename F>
    void finish(F&& on_line) {
        if (!partial_.empty()) on_line(std::string_view(partial_));
        partial_.clear();
    }
    bool has_partial() const { return !partial_.empty(); }

private:
    std::string partial_;
};

}  // namespace

Reader::Reader(std::vector<Input> inputs, bool follow) : inputs_(std::move(inputs)), follow_(follow) {}

Reader::~Reader() { stop(); }

void Reader::start(std::function<void()> on_data) {
    on_data_ = std::move(on_data);
    running_inputs_ = static_cast<int>(inputs_.size());
    for (const auto& input : inputs_) {
        threads_.emplace_back([this, input] {
            if (input.path == "-") read_stdin(input);
            else read_file(input);
            --running_inputs_;
            if (on_data_) on_data_();
        });
    }
}

void Reader::stop() {
    stopping_ = true;
    for (auto& t : threads_) {
        // stdin may be blocked in a read that can't be interrupted portably; let it go with the process
        if (t.joinable()) t.detach();
    }
    threads_.clear();
}

std::vector<Entry> Reader::take() {
    std::lock_guard lock(mutex_);
    std::vector<Entry> out;
    out.swap(ready_);
    return out;
}

std::vector<std::string> Reader::take_errors() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(errors_);
    return out;
}

void Reader::deliver(std::vector<Entry>& batch) {
    if (batch.empty()) return;
    {
        std::lock_guard lock(mutex_);
        if (ready_.empty()) {
            ready_.swap(batch);
        } else {
            for (auto& e : batch) ready_.push_back(std::move(e));
        }
    }
    batch.clear();
    if (on_data_) on_data_();
}

void Reader::error(std::string message) {
    {
        std::lock_guard lock(mutex_);
        errors_.push_back(std::move(message));
    }
    if (on_data_) on_data_();
}

void Reader::read_file(const Input& input) {
    Parser parser(input.label);
    std::vector<Entry> batch;
    LineSplitter lines;
    auto on_line = [&](std::string_view line) {
        parser.feed(line, batch);
        if (batch.size() >= kBatch) deliver(batch);
    };

    std::ifstream file;
    std::uintmax_t offset = 0;
    bool warned = false;
    int idle_polls = 0;
    std::vector<char> buffer(1 << 16);

    while (!stopping_) {
        if (!file.is_open()) {
            file.open(fs::path(input.path), std::ios::binary);
            if (!file) {
                if (!warned) error("can't open " + input.path);
                warned = true;
                if (!follow_) return;
                std::this_thread::sleep_for(kPoll);
                continue;
            }
            offset = 0;
        }

        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize n = file.gcount();
        if (n > 0) {
            offset += static_cast<std::uintmax_t>(n);
            lines.push(buffer.data(), static_cast<size_t>(n), on_line);
            idle_polls = 0;
            continue;
        }

        // end of the file for now; the newest entry is held back one poll in case its stack trace is
        // still being written
        file.clear();
        if (idle_polls++ > 0) parser.flush(batch);
        deliver(batch);
        if (!follow_) {
            lines.finish(on_line);
            parser.flush(batch);
            deliver(batch);
            return;
        }
        std::this_thread::sleep_for(kPoll);

        std::error_code ec;
        auto size = fs::file_size(fs::path(input.path), ec);
        if (ec || size < offset) {
            // truncated, or rotated away: start again from the top of the (new) file
            file.close();
            lines = LineSplitter{};
        }
    }
}

void Reader::read_stdin(const Input& input) {
    Parser parser(input.label);
    std::vector<Entry> batch;
    LineSplitter lines;
    auto on_line = [&](std::string_view line) {
        parser.feed(line, batch);
        if (batch.size() >= kBatch) deliver(batch);
    };
    std::vector<char> buffer(1 << 16);

#ifdef _WIN32
    HANDLE handle = input.native != -1 ? reinterpret_cast<HANDLE>(input.native)
                                       : reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin)));
    bool is_pipe = GetFileType(handle) == FILE_TYPE_PIPE;
#else
    const int fd = input.native != -1 ? static_cast<int>(input.native) : STDIN_FILENO;
#endif

    while (!stopping_) {
        // wait up to 150 ms for input; when nothing comes, show the entry held back for a stack trace
        bool readable = true;
#ifdef _WIN32
        if (is_pipe) {
            DWORD available = 0;
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
                readable = true;  // broken pipe: let the read below report the end
            } else if (available == 0) {
                readable = false;
                Sleep(50);
            }
        }
#else
        pollfd p{fd, POLLIN, 0};
        readable = poll(&p, 1, 150) > 0;
#endif
        if (!readable) {
            if (parser.has_pending()) {
                parser.flush(batch);
                deliver(batch);
            }
            continue;
        }
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) || got == 0) break;
        size_t n = got;
#else
        ssize_t n = read(fd, buffer.data(), buffer.size());
        if (n <= 0) break;
#endif
        lines.push(buffer.data(), static_cast<size_t>(n), on_line);
        deliver(batch);
    }
    lines.finish(on_line);
    parser.flush(batch);
    deliver(batch);
}

}  // namespace culprit
