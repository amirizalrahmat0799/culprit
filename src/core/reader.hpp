#pragma once
// Reads log files and standard input on background threads, parses them, and hands over entries in
// batches. Files are followed like `tail -F`: new lines appear as they're written, and a file that is
// truncated or replaced by log rotation is read again from the start.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/entry.hpp"

namespace culprit {

class Reader {
public:
    struct Input {
        std::string path;  // "-" for standard input
        std::string label; // shown as the entry's source when reading several inputs
        // For "-": a duplicate of the original stdin (a file descriptor, or a HANDLE on Windows), taken
        // before stdin is pointed back at the keyboard for the viewer. -1 reads stdin itself.
        intptr_t native = -1;
    };

    /// `follow`: keep watching files for new lines after reaching their end.
    Reader(std::vector<Input> inputs, bool follow);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// Starts the threads. `on_data` is called (from a reader thread) whenever new entries are waiting.
    void start(std::function<void()> on_data);
    void stop();

    /// Entries parsed since the last call, in the order they were completed.
    std::vector<Entry> take();
    /// Problems so far, such as a file that can't be opened.
    std::vector<std::string> take_errors();
    /// True when every input has ended and none is being followed.
    bool finished() const { return running_inputs_.load() == 0; }

private:
    void read_file(const Input& input);
    void read_stdin(const Input& input);
    void deliver(std::vector<Entry>& batch);
    void error(std::string message);

    std::vector<Input> inputs_;
    bool follow_;
    std::function<void()> on_data_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> running_inputs_{0};
    std::mutex mutex_;
    std::vector<Entry> ready_;
    std::vector<std::string> errors_;
};

}  // namespace culprit
