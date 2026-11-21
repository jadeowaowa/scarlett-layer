#pragma once

#include "shader_inspector.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sc::d3d12 {

namespace detail {

inline uint32_t RotL32(uint32_t v, uint32_t s) {
    return (v << s) | (v >> (32u - s));
}

inline const std::array<uint32_t, 64>& GetMd5RoundConstants() {
    static const std::array<uint32_t, 64> kTable = [] {
        std::array<uint32_t, 64> t{};
        for (uint32_t i = 0; i < 64; ++i) {
            const double mag = std::fabs(std::sin(static_cast<double>(i + 1)));
            t[i] = static_cast<uint32_t>(std::floor(mag * 4294967296.0));
        }
        return t;
    }();
    return kTable;
}

inline void Md5CompressBlock(uint32_t state[4], const uint8_t block[64]) {
    static constexpr uint32_t kShifts[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    const auto& k = GetMd5RoundConstants();

    uint32_t m[16];
    std::memcpy(m, block, 64);

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f = 0;
        uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = c ^ (b | (~d));
            g = (7u * i) & 15u;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + RotL32(a + f + k[i] + m[g], kShifts[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace detail

// Computes the 16-byte DXBC container modified-MD5 digest over bytes [20..fileSize).
inline void ComputeDxbcDigest(const uint8_t* payloadAfterDigest, size_t payloadLen, uint8_t outDigest[16]) {
    uint32_t state[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };

    const size_t fullBlocks = payloadLen / 64u;
    for (size_t i = 0; i < fullBlocks; ++i) {
        detail::Md5CompressBlock(state, payloadAfterDigest + i * 64u);
    }

    const size_t rem = payloadLen % 64u;
    const uint8_t* tail = payloadAfterDigest + fullBlocks * 64u;
    const uint32_t numBits = static_cast<uint32_t>(payloadLen) * 8u;
    const uint32_t tailTag = (static_cast<uint32_t>(payloadLen) << 1u) | 1u;

    uint8_t pad[64]{};
    if (rem >= 56u) {
        std::memcpy(pad, tail, rem);
        pad[rem] = 0x80u;
        detail::Md5CompressBlock(state, pad);

        std::memset(pad, 0, sizeof(pad));
        std::memcpy(pad, &numBits, sizeof(numBits));
        std::memcpy(pad + 60, &tailTag, sizeof(tailTag));
        detail::Md5CompressBlock(state, pad);
    } else {
        std::memcpy(pad, &numBits, sizeof(numBits));
        if (rem > 0) {
            std::memcpy(pad + 4, tail, rem);
        }
        pad[4 + rem] = 0x80u;
        std::memcpy(pad + 60, &tailTag, sizeof(tailTag));
        detail::Md5CompressBlock(state, pad);
    }

    std::memcpy(outDigest, state, 16);
}

inline bool IsPcWhitelistedDxbcChunk(const char fourcc[4]) {
    static constexpr const char* kWhitelist[] = {
        "DXIL", "SHDR", "SHEX", "ISGN", "ISG1", "OSGN", "OSG1",
        "PSG1", "PCSG", "RTS0", "RDEF", "RDAT", "STAT", "PSV0",
        "SFI0", "HASH", "PRIV", "SPDB", "ILDB", "IFCE", "Aon9"
    };
    for (const char* ok : kWhitelist) {
        if (std::memcmp(fourcc, ok, 4) == 0) {
            return true;
        }
    }
    return false;
}

inline bool HasNonPcDxbcChunks(const void* data, size_t size) {
    if (!data || size < sizeof(DxbcHeader)) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return false;
    }
    DxbcHeader hdr{};
    std::memcpy(&hdr, bytes, sizeof(hdr));
    if (hdr.partCount == 0 || hdr.partCount > 256u) {
        return false;
    }
    const size_t tableEnd = sizeof(DxbcHeader) + static_cast<size_t>(hdr.partCount) * sizeof(uint32_t);
    if (tableEnd > size) {
        return false;
    }
    for (uint32_t i = 0; i < hdr.partCount; ++i) {
        uint32_t off = 0;
        std::memcpy(&off, bytes + sizeof(DxbcHeader) + static_cast<size_t>(i) * sizeof(uint32_t), sizeof(off));
        if (static_cast<size_t>(off) + sizeof(DxbcPartHeader) > size) {
            return false;
        }
        DxbcPartHeader part{};
        std::memcpy(&part, bytes + off, sizeof(part));
        if (!IsPcWhitelistedDxbcChunk(part.fourcc)) {
            return true;
        }
    }
    return false;
}

struct DxbcStripResult {
    bool                 validDxbc{false};
    bool                 wasStripped{false};
    bool                 hasExecutableShader{false};
    uint32_t             keptCount{0};
    uint32_t             strippedCount{0};
    std::string          strippedChunkNames;
    std::vector<uint8_t> bytecode;
};

inline bool StripXboxDxbcChunks(const void* data, size_t size, DxbcStripResult& out) {
    out = {};
    if (!data || size < sizeof(DxbcHeader)) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return false;
    }

    DxbcHeader hdr{};
    std::memcpy(&hdr, bytes, sizeof(hdr));
    const size_t tableEnd = sizeof(DxbcHeader) + static_cast<size_t>(hdr.partCount) * sizeof(uint32_t);
    if (hdr.partCount == 0 || hdr.partCount > 256u || tableEnd > size) {
        return false;
    }

    struct PartSlice {
        char     fourcc[4];
        uint32_t offset;
        uint32_t size;
    };
    std::vector<PartSlice> kept;
    kept.reserve(hdr.partCount);

    for (uint32_t i = 0; i < hdr.partCount; ++i) {
        uint32_t off = 0;
        std::memcpy(&off, bytes + sizeof(DxbcHeader) + static_cast<size_t>(i) * sizeof(uint32_t), sizeof(off));
        if (static_cast<size_t>(off) + sizeof(DxbcPartHeader) > size) {
            return false;
        }
        DxbcPartHeader ph{};
        std::memcpy(&ph, bytes + off, sizeof(ph));
        const size_t payloadEnd = static_cast<size_t>(off) + sizeof(DxbcPartHeader) + static_cast<size_t>(ph.size);
        if (payloadEnd > size) {
            return false;
        }

        if (IsPcWhitelistedDxbcChunk(ph.fourcc)) {
            PartSlice sl{};
            std::memcpy(sl.fourcc, ph.fourcc, 4);
            sl.offset = off;
            sl.size   = ph.size;
            kept.push_back(sl);
            if (std::memcmp(ph.fourcc, "DXIL", 4) == 0 ||
                std::memcmp(ph.fourcc, "SHDR", 4) == 0 ||
                std::memcmp(ph.fourcc, "SHEX", 4) == 0) {
                out.hasExecutableShader = true;
            }
        } else {
            ++out.strippedCount;
            if (!out.strippedChunkNames.empty()) {
                out.strippedChunkNames.push_back(',');
            }
            out.strippedChunkNames.append(ph.fourcc, 4);
        }
    }

    out.validDxbc = true;
    out.keptCount = static_cast<uint32_t>(kept.size());
    if (out.strippedCount == 0) {
        out.wasStripped = false;
        return true;
    }

    // Rebuild a clean DXBC container with updated part offsets and recomputed DXBC digest
    const uint32_t newPartCount = static_cast<uint32_t>(kept.size());
    const size_t newHeaderEnd = sizeof(DxbcHeader) + static_cast<size_t>(newPartCount) * sizeof(uint32_t);
    size_t totalBytes = newHeaderEnd;
    for (const auto& p : kept) {
        totalBytes += sizeof(DxbcPartHeader) + static_cast<size_t>(p.size);
        totalBytes = (totalBytes + 3u) & ~size_t{3u};
    }

    out.bytecode.assign(totalBytes, 0);
    DxbcHeader newHdr = hdr;
    newHdr.fileSize  = static_cast<uint32_t>(totalBytes);
    newHdr.partCount = newPartCount;
    std::memset(newHdr.digest, 0, sizeof(newHdr.digest));
    std::memcpy(out.bytecode.data(), &newHdr, sizeof(newHdr));

    size_t cursor = newHeaderEnd;
    for (uint32_t i = 0; i < newPartCount; ++i) {
        const uint32_t off32 = static_cast<uint32_t>(cursor);
        std::memcpy(out.bytecode.data() + sizeof(DxbcHeader) + static_cast<size_t>(i) * sizeof(uint32_t),
                    &off32, sizeof(off32));
        const size_t copyBytes = sizeof(DxbcPartHeader) + static_cast<size_t>(kept[i].size);
        std::memcpy(out.bytecode.data() + cursor, bytes + kept[i].offset, copyBytes);
        cursor += copyBytes;
        cursor = (cursor + 3u) & ~size_t{3u};
    }

    uint8_t digest[16]{};
    ComputeDxbcDigest(out.bytecode.data() + 20, out.bytecode.size() - 20, digest);
    std::memcpy(out.bytecode.data() + 4, digest, sizeof(digest));
    out.wasStripped = true;
    return true;
}

} // namespace sc::d3d12