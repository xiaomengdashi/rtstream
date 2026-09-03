#pragma once

// XOR FEC：每 K 个数据包生成 1 个恢复包（K 个包按位异或）。
// 接收端组内 ≤1 个包丢失时可恢复；≥2 丢失时走 NACK。
// 恢复包载荷 = 参与包载荷的异或，短包按 0 填充到组内最大长度。

#include <cstdint>
#include <cstring>
#include <vector>

namespace rtstream {

// 生成恢复包。data：组内 K 个数据包载荷。
inline std::vector<uint8_t> fec_generate(const std::vector<std::vector<uint8_t>>& data) {
    size_t max_len = 0;
    for (auto& d : data) max_len = std::max(max_len, d.size());
    std::vector<uint8_t> parity(max_len, 0);
    for (auto& d : data)
        for (size_t i = 0; i < d.size(); ++i)
            parity[i] ^= d[i];
    return parity;
}

// 尝试恢复：known 为组内已知包（缺失槽位为空）。若恰好缺 1 个则填充并返回 true。
inline bool fec_recover(std::vector<std::vector<uint8_t>>& known, size_t expect_len) {
    size_t missing = 0;
    int miss_idx = -1;
    for (size_t i = 0; i < known.size(); ++i) {
        if (known[i].empty()) { missing++; miss_idx = static_cast<int>(i); }
    }
    if (missing != 1) return false;
    std::vector<uint8_t> rec(expect_len, 0);
    for (auto& d : known)
        if (!d.empty())
            for (size_t i = 0; i < d.size() && i < rec.size(); ++i)
                rec[i] ^= d[i];
    known[miss_idx] = std::move(rec);
    return true;
}

} // namespace rtstream
