#pragma once
// FRAMECAP v1: bounded recorder; the backend reads images only at frame boundaries.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace godfather_diag {
inline std::atomic<bool> request{false};

// Called under the input lock. Holding the chord produces one request only.
struct ButtonLatch {
    bool held = false;
    bool Poll(uint64_t& buttons, uint64_t mask) {
        bool down = (buttons & mask) == mask;
        bool edge = down && !held;
        held = down;
        if (down) buttons &= ~mask;
        return edge;
    }
};

class FrameTrace {
public:
    static constexpr uint32_t kMaxEvents = 2048;
    static constexpr size_t kMaxData = 32u << 20, kMaxText = 2u << 20;
    FrameTrace() = default;
    FrameTrace(const FrameTrace&) = delete;
    FrameTrace& operator=(const FrameTrace&) = delete;
    ~FrameTrace() { End(); }
    bool Active() const { return text_ != nullptr; }
    bool Recording() const { return Active() && events_ <= kMaxEvents && !failed_; }
    bool Failed() const { return failed_; }
    const std::filesystem::path& Directory() const { return dir_; }

    bool Begin(const std::filesystem::path& root, uint64_t frame) {
        End();
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) return false;
        auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        bool made = false;
        for (unsigned n = 0; n < 64; ++n) {
            dir_ = root / ("frame-" + std::to_string(stamp) + "-" + std::to_string(frame) + "-" + std::to_string(n));
            if (std::filesystem::create_directory(dir_, ec)) { made = true; break; }
            if (ec) return false;
        }
        if (!made) return false;
        text_ = std::fopen((dir_ / "frame.txt").string().c_str(), "wb");
        data_ = std::fopen((dir_ / "state.bin").string().c_str(), "wb");
        if (!text_ || !data_) { End(); return false; }
        std::setvbuf(text_, nullptr, _IOFBF, 65536);
        std::setvbuf(data_, nullptr, _IOFBF, 65536);
        events_ = 0; bytes_ = text_bytes_ = 0; limited_ = failed_ = false;
        files_.clear();
        Line("FRAMECAP v1 frame=%llu; binary records: magic GFD1, event, tag, bytes (4 LE uint32), payload\n",
             (unsigned long long)frame);
        Line("tags: 1=rs uint32, 2=simple uint32, 3=vsc float4[192], 4=DrawConstants, 5=tss uint32, 6=PixelKey\n");
        return true;
    }
    void Event(const char* kind) {
        if (!Active()) return;
        ++events_;
        if (events_ > kMaxEvents) { limited_ = true; return; }
        Line("\nEVENT %u %s\n", events_, kind);
    }
    void Line(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        if (!Recording()) return;
        char line[4096];
        va_list args;
        va_start(args, format);
        int result = std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (result < 0) { failed_ = true; return; }
        size_t n = size_t(result);
        if (n >= sizeof(line)) { n = sizeof(line) - 1; limited_ = true; }
        if (text_bytes_ + n > kMaxText) { limited_ = true; return; }
        if (std::fwrite(line, 1, n, text_) != n) failed_ = true;
        text_bytes_ += n;
    }
    void Blob(uint32_t tag, const void* ptr, size_t size) {
        if (!Recording()) return;
        if (size > kMaxData || bytes_ + 16 + size > kMaxData) { limited_ = true; return; }
        uint32_t header[4] = {0x31444647, events_, tag, uint32_t(size)};
        if (std::fwrite(header, 1, 16, data_) != 16 || std::fwrite(ptr, 1, size, data_) != size) failed_ = true;
        bytes_ += 16 + size;
    }
    bool NeedsFile(const std::string& name) const {
        return Recording() && files_.size() < 128 && !files_.count(name);
    }
    bool File(const std::string& name, const void* ptr, size_t size) {
        if (!NeedsFile(name) || name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos ||
            name.find("..") != std::string::npos) return false;
        if (size > kMaxData || bytes_ + size > kMaxData) { limited_ = true; return false; }
        FILE* f = std::fopen((dir_ / name).string().c_str(), "wb");
        if (!f) { failed_ = true; return false; }
        bool ok = std::fwrite(ptr, 1, size, f) == size;
        if (std::fclose(f)) ok = false;
        bytes_ += size;
        files_.insert(name);
        if (!ok) failed_ = true;
        return ok;
    }
    void End() {
        if (data_) {
            if (std::fclose(data_)) failed_ = true;
            data_ = nullptr;
        }
        if (text_) {
            if (std::fprintf(text_, "\nEND events=%u limited=%u failed=%u bytes=%zu\n", events_, unsigned(limited_), unsigned(failed_), bytes_) < 0)
                failed_ = true;
            if (std::fclose(text_)) failed_ = true;
            text_ = nullptr;
        }
    }
private:
    FILE* text_ = nullptr;
    FILE* data_ = nullptr;
    std::filesystem::path dir_;
    std::unordered_set<std::string> files_;
    uint32_t events_ = 0;
    size_t bytes_ = 0, text_bytes_ = 0;
    bool limited_ = false, failed_ = false;
};
} // namespace godfather_diag
