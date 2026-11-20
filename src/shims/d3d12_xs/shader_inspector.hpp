#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <d3d12.h>

namespace sc::d3d12 {

struct DxbcHeader {
    char     magic[4];
    uint8_t  digest[16];
    uint16_t majorVersion;
    uint16_t minorVersion;
    uint32_t fileSize;
    uint32_t partCount;
};
static_assert(sizeof(DxbcHeader) == 32, "DXBC header must be 32 bytes");

struct DxbcPartHeader {
    char     fourcc[4];
    uint32_t size;
};

inline std::string DecodeShaderProgramToken(uint32_t token) {
    const uint32_t type = (token >> 16) & 0xFFFFu;
    const uint32_t major = (token >> 4) & 0xFu;
    const uint32_t minor = token & 0xFu;
    const char* prefix = "unk";
    switch (type) {
        case 0: prefix = "ps"; break;
        case 1: prefix = "vs"; break;
        case 2: prefix = "gs"; break;
        case 3: prefix = "hs"; break;
        case 4: prefix = "ds"; break;
        case 5: prefix = "cs"; break;
        default: break;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s_%u_%u", prefix, major, minor);
    return buf;
}

inline std::string InspectShaderBytecode(const D3D12_SHADER_BYTECODE& bc) {
    if (!bc.pShaderBytecode || bc.BytecodeLength == 0) {
        return "none";
    }
    if (bc.BytecodeLength < sizeof(DxbcHeader)) {
        return "RAW_SMALL(" + std::to_string(bc.BytecodeLength) + " B)";
    }

    const auto* bytes = static_cast<const uint8_t*>(bc.pShaderBytecode);
    DxbcHeader hdr{};
    std::memcpy(&hdr, bytes, sizeof(hdr));

    if (std::memcmp(hdr.magic, "DXBC", 4) != 0) {
        return "SCARLETT_OR_CUSTOM(" + std::to_string(bc.BytecodeLength) + " B)";
    }

    const uint32_t maxParts = hdr.partCount < 32u ? hdr.partCount : 32u;
    const size_t offsetTableEnd = sizeof(DxbcHeader) + static_cast<size_t>(maxParts) * sizeof(uint32_t);
    if (offsetTableEnd > bc.BytecodeLength) {
        return "DXBC_TRUNCATED(" + std::to_string(bc.BytecodeLength) + " B)";
    }

    std::string profile = "DXBC";
    std::string chunks;
    bool hasDxil = false;

    for (uint32_t i = 0; i < maxParts; ++i) {
        uint32_t partOff = 0;
        std::memcpy(&partOff, bytes + sizeof(DxbcHeader) + i * sizeof(uint32_t), sizeof(uint32_t));
        if (static_cast<size_t>(partOff) + sizeof(DxbcPartHeader) > bc.BytecodeLength) {
            continue;
        }

        DxbcPartHeader part{};
        std::memcpy(&part, bytes + partOff, sizeof(part));

        char name[5]{ part.fourcc[0], part.fourcc[1], part.fourcc[2], part.fourcc[3], '\0' };
        if (!chunks.empty()) {
            chunks += ',';
        }
        chunks += name;

        const size_t bodyOff = static_cast<size_t>(partOff) + sizeof(DxbcPartHeader);
        if ((std::memcmp(part.fourcc, "SHDR", 4) == 0 || std::memcmp(part.fourcc, "SHEX", 4) == 0) &&
            bodyOff + sizeof(uint32_t) <= bc.BytecodeLength) {
            uint32_t verToken = 0;
            std::memcpy(&verToken, bytes + bodyOff, sizeof(uint32_t));
            profile = DecodeShaderProgramToken(verToken);
        } else if (std::memcmp(part.fourcc, "DXIL", 4) == 0 &&
                   bodyOff + sizeof(uint32_t) <= bc.BytecodeLength) {
            hasDxil = true;
            uint32_t verToken = 0;
            std::memcpy(&verToken, bytes + bodyOff, sizeof(uint32_t));
            profile = DecodeShaderProgramToken(verToken) + "/DXIL";
        }
    }

    if (hasDxil && profile == "DXBC") {
        profile = "DXIL";
    }

    return profile + " [" + chunks + "] (" + std::to_string(bc.BytecodeLength) + " B)";
}

} // namespace sc::d3d12