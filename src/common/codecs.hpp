#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sc::codec {

inline constexpr uint16_t kWaveFormatImaAdpcm       = 0x0011u;
inline constexpr uint16_t kWaveFormatXma2           = 0x0166u;
inline constexpr uint16_t kImaAdpcmBlockAlign       = 256u;
inline constexpr uint32_t kImaAdpcmSamplesPerBlock  = 1u + (kImaAdpcmBlockAlign - 4u) * 2u; // 505 samples

// ============================================================================
// 1. SCLZ Fast 64 KB Sliding-Window LZ77 Streaming Block Codec
// ============================================================================
struct ScLzHeader {
    char     magic[4];              // "SCLZ"
    uint32_t uncompressedSize;      // Expected decompressed size in bytes
    uint32_t compressedPayloadSize; // Bytes following this 16-byte header
    uint32_t uncompressedFnv1a;     // FNV-1a 32-bit checksum of uncompressed data
};
static_assert(sizeof(ScLzHeader) == 16, "ScLzHeader must be 16 bytes");

inline uint32_t ComputeFnv1a32(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

inline std::vector<uint8_t> CompressLzBlock(const void* srcData, size_t srcSize) {
    const auto* src = static_cast<const uint8_t*>(srcData);
    std::vector<uint8_t> out;
    out.resize(sizeof(ScLzHeader));

    constexpr size_t kHashBits = 12;
    constexpr size_t kHashSize = 1u << kHashBits;
    constexpr size_t kMaxWindow = 65536u;
    constexpr size_t kMinMatch = 3u;
    constexpr size_t kMaxMatch = 130u; // 3 + 127

    std::vector<int32_t> head(kHashSize, -1);
    std::vector<uint8_t> pendingLits;
    pendingLits.reserve(128);

    auto flushLiterals = [&]() {
        size_t offset = 0;
        while (offset < pendingLits.size()) {
            const size_t chunk = std::min<size_t>(128u, pendingLits.size() - offset);
            out.push_back(static_cast<uint8_t>(chunk - 1u)); // bit 7 = 0 -> literal run
            out.insert(out.end(),
                       pendingLits.begin() + static_cast<std::ptrdiff_t>(offset),
                       pendingLits.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            offset += chunk;
        }
        pendingLits.clear();
    };

    auto hash3 = [](const uint8_t* p) -> size_t {
        return ((static_cast<size_t>(p[0]) << 10) ^
                (static_cast<size_t>(p[1]) << 5) ^
                static_cast<size_t>(p[2])) & (kHashSize - 1u);
    };

    size_t pos = 0;
    while (pos < srcSize) {
        size_t bestLen = 0;
        size_t bestDist = 0;

        if (pos + kMinMatch <= srcSize) {
            const size_t h = hash3(src + pos);
            const int32_t cand = head[h];
            head[h] = static_cast<int32_t>(pos);

            if (cand >= 0) {
                const size_t dist = pos - static_cast<size_t>(cand);
                if (dist >= 1u && dist <= kMaxWindow) {
                    const size_t maxLen = std::min<size_t>(kMaxMatch, srcSize - pos);
                    size_t len = 0;
                    while (len < maxLen && src[static_cast<size_t>(cand) + len] == src[pos + len]) {
                        ++len;
                    }
                    if (len >= kMinMatch) {
                        bestLen = len;
                        bestDist = dist;
                    }
                }
            }
        }

        if (bestLen >= kMinMatch) {
            flushLiterals();
            const uint8_t token = static_cast<uint8_t>(0x80u | ((bestLen - kMinMatch) & 0x7Fu));
            const uint16_t distMinus1 = static_cast<uint16_t>(bestDist - 1u);
            out.push_back(token);
            out.push_back(static_cast<uint8_t>(distMinus1 & 0xFFu));
            out.push_back(static_cast<uint8_t>((distMinus1 >> 8) & 0xFFu));

            // Keep hash table warm across skipped bytes inside the match
            const size_t endPos = pos + bestLen;
            for (size_t k = pos + 1; k < endPos && k + kMinMatch <= srcSize; ++k) {
                head[hash3(src + k)] = static_cast<int32_t>(k);
            }
            pos = endPos;
        } else {
            pendingLits.push_back(src[pos]);
            if (pendingLits.size() == 128u) {
                flushLiterals();
            }
            ++pos;
        }
    }
    flushLiterals();

    ScLzHeader hdr{};
    std::memcpy(hdr.magic, "SCLZ", 4);
    hdr.uncompressedSize = static_cast<uint32_t>(srcSize);
    hdr.compressedPayloadSize = static_cast<uint32_t>(out.size() - sizeof(ScLzHeader));
    hdr.uncompressedFnv1a = (src && srcSize > 0) ? ComputeFnv1a32(src, srcSize) : 0u;
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    return out;
}

inline bool DecompressLzBlock(
    const void* compData,
    size_t compSize,
    void* dstData,
    size_t dstCapacity,
    size_t& outDecompressedSize) {
    outDecompressedSize = 0;
    if (!compData || compSize < sizeof(ScLzHeader)) {
        return false;
    }

    const auto* in = static_cast<const uint8_t*>(compData);
    ScLzHeader hdr{};
    std::memcpy(&hdr, in, sizeof(hdr));

    if (std::memcmp(hdr.magic, "SCLZ", 4) != 0) {
        return false;
    }
    if (sizeof(ScLzHeader) + static_cast<size_t>(hdr.compressedPayloadSize) > compSize) {
        return false;
    }
    if (hdr.uncompressedSize > dstCapacity) {
        return false;
    }
    if (hdr.uncompressedSize == 0) {
        return true;
    }
    if (!dstData) {
        return false;
    }

    auto* out = static_cast<uint8_t*>(dstData);
    size_t inPos = sizeof(ScLzHeader);
    const size_t inEnd = sizeof(ScLzHeader) + static_cast<size_t>(hdr.compressedPayloadSize);
    size_t outPos = 0;
    const size_t outEnd = hdr.uncompressedSize;

    while (inPos < inEnd) {
        const uint8_t cmd = in[inPos++];
        if ((cmd & 0x80u) == 0) {
            const size_t litLen = static_cast<size_t>(cmd & 0x7Fu) + 1u;
            if (inPos + litLen > inEnd || outPos + litLen > outEnd) {
                return false;
            }
            std::memcpy(out + outPos, in + inPos, litLen);
            inPos += litLen;
            outPos += litLen;
        } else {
            if (inPos + 2u > inEnd) {
                return false;
            }
            const size_t matchLen = static_cast<size_t>(cmd & 0x7Fu) + 3u;
            const uint16_t distMinus1 = static_cast<uint16_t>(
                static_cast<uint16_t>(in[inPos]) | (static_cast<uint16_t>(in[inPos + 1]) << 8));
            inPos += 2u;
            const size_t dist = static_cast<size_t>(distMinus1) + 1u;
            if (dist > outPos || outPos + matchLen > outEnd) {
                return false;
            }
            for (size_t k = 0; k < matchLen; ++k) {
                out[outPos + k] = out[outPos - dist + k];
            }
            outPos += matchLen;
        }
    }

    if (outPos != outEnd) {
        return false;
    }
    if (ComputeFnv1a32(out, outPos) != hdr.uncompressedFnv1a) {
        return false;
    }

    outDecompressedSize = outPos;
    return true;
}

// ============================================================================
// 2. Standard IMA-ADPCM Mono Codec (WAVE_FORMAT_IMA_ADPCM = 0x0011, 4:1)
// ============================================================================
inline constexpr std::array<int8_t, 16> kImaIndexTable = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

inline constexpr std::array<int16_t, 89> kImaStepTable = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

inline std::vector<uint8_t> EncodeImaAdpcmMono(
    const int16_t* pcm,
    size_t sampleCount,
    uint16_t blockAlign = kImaAdpcmBlockAlign) {
    std::vector<uint8_t> out;
    if (!pcm || sampleCount == 0 || blockAlign < 8u) {
        return out;
    }

    const size_t dataBytesPerBlock = static_cast<size_t>(blockAlign) - 4u;
    const size_t samplesPerBlock = 1u + dataBytesPerBlock * 2u;
    const size_t numBlocks = (sampleCount + samplesPerBlock - 1u) / samplesPerBlock;
    out.reserve(numBlocks * blockAlign);

    int stepIndex = 0;
    size_t pos = 0;

    while (pos < sampleCount) {
        int predictor = static_cast<int>(pcm[pos++]);
        // 4-byte IMA block header: [int16 predictor][uint8 stepIndex][uint8 reserved]
        out.push_back(static_cast<uint8_t>(predictor & 0xFF));
        out.push_back(static_cast<uint8_t>((predictor >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(stepIndex));
        out.push_back(0u);

        auto encodeNibble = [&](int16_t sample) -> uint8_t {
            const int step = kImaStepTable[static_cast<size_t>(stepIndex)];
            int diff = static_cast<int>(sample) - predictor;
            uint8_t nibble = 0u;
            if (diff < 0) {
                nibble = 8u;
                diff = -diff;
            }
            int delta = step >> 3;
            if (diff >= step) {
                nibble |= 4u;
                diff -= step;
                delta += step;
            }
            if (diff >= (step >> 1)) {
                nibble |= 2u;
                diff -= (step >> 1);
                delta += (step >> 1);
            }
            if (diff >= (step >> 2)) {
                nibble |= 1u;
                delta += (step >> 2);
            }
            predictor = std::clamp(
                (nibble & 8u) ? (predictor - delta) : (predictor + delta),
                -32768, 32767);
            stepIndex = std::clamp(stepIndex + static_cast<int>(kImaIndexTable[nibble]), 0, 88);
            return nibble;
        };

        for (size_t b = 0; b < dataBytesPerBlock; ++b) {
            const int16_t s0 = (pos < sampleCount) ? pcm[pos++] : static_cast<int16_t>(predictor);
            const uint8_t lo = encodeNibble(s0);
            const int16_t s1 = (pos < sampleCount) ? pcm[pos++] : static_cast<int16_t>(predictor);
            const uint8_t hi = encodeNibble(s1);
            out.push_back(static_cast<uint8_t>((lo & 0x0Fu) | ((hi & 0x0Fu) << 4)));
        }
    }
    return out;
}

inline bool DecodeImaAdpcmMono(
    const uint8_t* adpcmData,
    size_t adpcmBytes,
    uint16_t blockAlign,
    std::vector<int16_t>& outPcm) {
    outPcm.clear();
    if (!adpcmData || adpcmBytes < 4u || blockAlign < 8u) {
        return false;
    }

    const size_t fullBlocks = adpcmBytes / blockAlign;
    const size_t tailBytes = adpcmBytes % blockAlign;
    const size_t samplesPerBlock = 1u + (static_cast<size_t>(blockAlign) - 4u) * 2u;
    outPcm.reserve((fullBlocks + (tailBytes >= 4u ? 1u : 0u)) * samplesPerBlock);

    size_t offset = 0;
    while (offset + 4u <= adpcmBytes) {
        const size_t blockBytes = std::min<size_t>(blockAlign, adpcmBytes - offset);
        const uint8_t* blk = adpcmData + offset;
        offset += blockBytes;

        int predictor = static_cast<int>(static_cast<int16_t>(
            static_cast<uint16_t>(blk[0]) | (static_cast<uint16_t>(blk[1]) << 8)));
        int stepIndex = std::clamp(static_cast<int>(blk[2]), 0, 88);

        outPcm.push_back(static_cast<int16_t>(predictor));

        auto decodeNibble = [&](uint8_t nibble) -> int16_t {
            const int step = kImaStepTable[static_cast<size_t>(stepIndex)];
            int delta = step >> 3;
            if (nibble & 4u) delta += step;
            if (nibble & 2u) delta += (step >> 1);
            if (nibble & 1u) delta += (step >> 2);
            predictor = std::clamp(
                (nibble & 8u) ? (predictor - delta) : (predictor + delta),
                -32768, 32767);
            stepIndex = std::clamp(stepIndex + static_cast<int>(kImaIndexTable[nibble & 0x0Fu]), 0, 88);
            return static_cast<int16_t>(predictor);
        };

        for (size_t b = 4u; b < blockBytes; ++b) {
            const uint8_t packed = blk[b];
            outPcm.push_back(decodeNibble(static_cast<uint8_t>(packed & 0x0Fu)));
            outPcm.push_back(decodeNibble(static_cast<uint8_t>((packed >> 4) & 0x0Fu)));
        }
    }
    return !outPcm.empty();
}

} // namespace sc::codec