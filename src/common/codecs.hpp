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


// 3. RFC 1950 (zlib) / RFC 1951 (raw deflate) Inflater for GDK DirectStorage
inline bool InflateRawDeflate(
    const uint8_t* src, size_t srcSize,
    uint8_t* dst, size_t dstCap, size_t& outSize) {
    outSize = 0;
    if (!src || !dst) return false;

    struct BitReader {
        const uint8_t* p;
        const uint8_t* end;
        uint64_t buf = 0;
        int cnt = 0;
        bool ensure(int n) {
            while (cnt < n && p < end) {
                buf |= static_cast<uint64_t>(*p++) << cnt;
                cnt += 8;
            }
            return cnt >= n;
        }
        int readBits(int n) {
            if (n == 0) return 0;
            if (!ensure(n)) return -1;
            const int v = static_cast<int>(buf & ((1ULL << n) - 1ULL));
            buf >>= n;
            cnt -= n;
            return v;
        }
        void alignByte() {
            const int drop = cnt & 7;
            buf >>= drop;
            cnt -= drop;
        }
    } br{src, src + srcSize};

    struct Huff {
        uint16_t count[16]{};
        uint16_t sym[288]{};
    };

    auto buildHuff = [](const uint8_t* lengths, int numSyms, Huff& h) -> bool {
        std::memset(&h, 0, sizeof(h));
        for (int i = 0; i < numSyms; ++i) {
            if (lengths[i] > 15) return false;
            h.count[lengths[i]]++;
        }
        h.count[0] = 0;
        uint16_t offs[16]{};
        for (int len = 1; len < 15; ++len) {
            offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
        }
        for (int i = 0; i < numSyms; ++i) {
            if (lengths[i] != 0) {
                h.sym[offs[lengths[i]]++] = static_cast<uint16_t>(i);
            }
        }
        return true;
    };

    auto decodeHuff = [&br](const Huff& h) -> int {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            const int bit = br.readBits(1);
            if (bit < 0) return -1;
            code |= bit;
            const int c = h.count[len];
            if (code - c < first) {
                return h.sym[index + (code - first)];
            }
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    };

    static constexpr uint16_t kLenBase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static constexpr uint8_t kLenExtra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static constexpr uint16_t kDistBase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static constexpr uint8_t kDistExtra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static constexpr uint8_t kCodeOrder[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    size_t outPos = 0;
    int bfinal = 0;
    do {
        bfinal = br.readBits(1);
        const int btype = br.readBits(2);
        if (bfinal < 0 || btype < 0 || btype == 3) return false;

        if (btype == 0) {
            br.alignByte();
            const int len = br.readBits(16);
            const int nlen = br.readBits(16);
            if (len < 0 || nlen < 0 || ((len ^ 0xFFFF) != nlen)) return false;
            if (outPos + static_cast<size_t>(len) > dstCap) return false;
            br.p -= (br.cnt / 8);
            br.buf = 0;
            br.cnt = 0;
            if (static_cast<size_t>(br.end - br.p) < static_cast<size_t>(len)) return false;
            std::memcpy(dst + outPos, br.p, static_cast<size_t>(len));
            br.p += len;
            outPos += static_cast<size_t>(len);
            continue;
        }

        Huff litLen{}, dist{};
        if (btype == 1) {
            uint8_t ll[288]{};
            for (int i = 0; i <= 143; ++i) ll[i] = 8;
            for (int i = 144; i <= 255; ++i) ll[i] = 9;
            for (int i = 256; i <= 279; ++i) ll[i] = 7;
            for (int i = 280; i <= 287; ++i) ll[i] = 8;
            uint8_t dl[32]{};
            for (int i = 0; i < 32; ++i) dl[i] = 5;
            if (!buildHuff(ll, 288, litLen) || !buildHuff(dl, 32, dist)) return false;
        } else {
            const int hlit = br.readBits(5) + 257;
            const int hdist = br.readBits(5) + 1;
            const int hclen = br.readBits(4) + 4;
            if (hlit < 257 || hlit > 286 || hdist < 1 || hdist > 30 || hclen < 4) return false;

            uint8_t clLens[19]{};
            for (int i = 0; i < hclen; ++i) {
                const int v = br.readBits(3);
                if (v < 0) return false;
                clLens[kCodeOrder[i]] = static_cast<uint8_t>(v);
            }
            Huff clHuff{};
            if (!buildHuff(clLens, 19, clHuff)) return false;

            uint8_t lengths[320]{};
            int idx = 0;
            const int total = hlit + hdist;
            while (idx < total) {
                const int s = decodeHuff(clHuff);
                if (s < 0) return false;
                if (s < 16) {
                    lengths[idx++] = static_cast<uint8_t>(s);
                } else {
                    uint8_t repVal = 0;
                    int repCnt = 0;
                    if (s == 16) {
                        if (idx == 0) return false;
                        const int e = br.readBits(2);
                        if (e < 0) return false;
                        repVal = lengths[idx - 1];
                        repCnt = 3 + e;
                    } else if (s == 17) {
                        const int e = br.readBits(3);
                        if (e < 0) return false;
                        repCnt = 3 + e;
                    } else {
                        const int e = br.readBits(7);
                        if (e < 0) return false;
                        repCnt = 11 + e;
                    }
                    if (idx + repCnt > total) return false;
                    for (int r = 0; r < repCnt; ++r) lengths[idx++] = repVal;
                }
            }
            if (!buildHuff(lengths, hlit, litLen) || !buildHuff(lengths + hlit, hdist, dist)) return false;
        }

        while (true) {
            const int sym = decodeHuff(litLen);
            if (sym < 0) return false;
            if (sym < 256) {
                if (outPos >= dstCap) return false;
                dst[outPos++] = static_cast<uint8_t>(sym);
            } else if (sym == 256) {
                break;
            } else {
                const int lenIdx = sym - 257;
                if (lenIdx < 0 || lenIdx >= 29) return false;
                const int extraL = br.readBits(kLenExtra[lenIdx]);
                if (extraL < 0) return false;
                const size_t matchLen = static_cast<size_t>(kLenBase[lenIdx] + extraL);

                const int distSym = decodeHuff(dist);
                if (distSym < 0 || distSym >= 30) return false;
                const int extraD = br.readBits(kDistExtra[distSym]);
                if (extraD < 0) return false;
                const size_t matchDist = static_cast<size_t>(kDistBase[distSym] + extraD);

                if (matchDist == 0 || matchDist > outPos || outPos + matchLen > dstCap) return false;
                for (size_t k = 0; k < matchLen; ++k) {
                    dst[outPos] = dst[outPos - matchDist];
                    ++outPos;
                }
            }
        }
    } while (!bfinal);

    outSize = outPos;
    return true;
}

inline bool DecompressZlib(
    const void* compData, size_t compSize,
    void* outData, size_t outCap, size_t& outSize) {
    outSize = 0;
    if (!compData || !outData || compSize < 2 || outCap == 0) return false;
    const auto* src = static_cast<const uint8_t*>(compData);
    auto* dst = static_cast<uint8_t*>(outData);

    // Check for RFC 1950 zlib 2-byte header (CM=8, CINFO<=7, FCHECK mod 31 == 0, FDICT=0)
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if (compSize >= 6 &&
        (cmf & 0x0Fu) == 8u &&
        (cmf >> 4u) <= 7u &&
        (((static_cast<uint16_t>(cmf) << 8) | flg) % 31u) == 0u &&
        (flg & 0x20u) == 0u) {
        if (InflateRawDeflate(src + 2, compSize - 2, dst, outCap, outSize)) {
            return true;
        }
    }
    // Fallback: raw RFC 1951 deflate stream
    return InflateRawDeflate(src, compSize, dst, outCap, outSize);
}
} // namespace sc::codec