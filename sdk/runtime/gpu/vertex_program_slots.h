#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gpu {

// NV2A instruction RAM. LoadVertexShaderProgram emits incrementing packets of
// up to 32 words at 0x0B00; the address advances after each four-word instruction.
class VertexProgramSlots {
public:
    static constexpr uint32_t kSlots = 136;

    bool Method(uint32_t method, uint32_t value) {
        if (method == 0x1E9C) {
            load_ = value;
            return true;
        }
        if (method < 0x0B00 || method >= 0x0B80 || (method & 3)) return false;
        uint32_t lane = ((method - 0x0B00) / 4) & 3;
        if (load_ < kSlots) {
            if (!(valid_[load_] & (1u << lane)) || code_[load_][lane] != value) {
                code_[load_][lane] = value;
                valid_[load_] |= uint8_t(1u << lane);
                ++revision_;
            }
            if (lane == 3) ++load_;
        }
        return true;
    }

    bool Load(uint32_t start, const std::vector<uint32_t>& words) {
        if (start >= kSlots || words.empty() || words.size() % 4 || words.size() / 4 > kSlots - start) return false;
        Method(0x1E9C, start);
        for (std::size_t n = 0; n < words.size(); ++n) Method(0x0B00 + uint32_t(n % 32) * 4, words[n]);
        return true;
    }

    // A declaration's original function is not the function at this address.
    // Stop at the microcode's FINAL bit; reject incomplete/out-of-range programs.
    bool Read(uint32_t start, std::vector<uint32_t>& words) const {
        words.clear();
        for (uint32_t slot = start; slot < kSlots; ++slot) {
            if (valid_[slot] != 15) break;
            words.insert(words.end(), code_[slot].begin(), code_[slot].end());
            if (code_[slot][3] & 1) return true;
        }
        words.clear();
        return false;
    }

    uint64_t Revision() const { return revision_; }

private:
    std::array<std::array<uint32_t, 4>, kSlots> code_{};
    std::array<uint8_t, kSlots> valid_{};
    uint32_t load_ = 0;
    uint64_t revision_ = 0;
};

}  // namespace gpu
