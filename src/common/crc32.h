#pragma once

// CRC32 (IEEE 802.3)：帧校验，查表实现。

#include <cstdint>
#include <cstddef>

namespace rtstream {

inline const uint32_t* crc32_table() {
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        inited = true;
    }
    return table;
}

inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0xFFFFFFFFu) {
    const uint32_t* table = crc32_table();
    uint32_t c = seed;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace rtstream
