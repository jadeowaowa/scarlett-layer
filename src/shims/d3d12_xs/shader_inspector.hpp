#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <d3d12.h>

namespace sc::d3d12 {

enum class ShaderContainerKind : uint8_t {
    None    = 0,
    Dxbc    = 1,
    Dxil    = 2,
    Scxs    = 3,
    Unknown = 4
};

struct ShaderInspectionInfo {
    ShaderContainerKind kind{ShaderContainerKind::None};
    std::string         profile;
    std::string         chunks;
    uint16_t            dxilMajor{0};
    uint16_t            dxilMinor{0};
    bool                hasLlvmBitcode{false};
    uint32_t            bitcodeSize{0};
    bool                hasPsv0{false};
    size_t              byteLength{0};
    std::string         summary;
};

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

// Official LLVM DirectX / DXC DxilProgramHeader layout inside a DXIL chunk
struct DxilProgramHeader {
    uint32_t programVersion; // [31:16]=ShaderKind, [7:4]=Major, [3:0]=Minor
    uint32_t sizeInUint32;
    uint32_t dxilMagic;      // 'DXIL' (0x4C495844)
    uint32_t dxilVersion;    // [15:8]=Major, [7:0]=Minor
    uint32_t bitcodeOffset;  // Offset in bytes from dxilMagic (+8 from start of header)
    uint32_t bitcodeSize;    // Size of LLVM bitcode stream in bytes
};
static_assert(sizeof(DxilProgramHeader) == 24, "DxilProgramHeader must be 24 bytes");

inline constexpr uint32_t kDxilMagicFourCc = 0x4C495844u; // 'DXIL' little-endian

inline std::string DecodeShaderProgramToken(uint32_t token) {
    const uint32_t type = (token >> 16) & 0xFFFFu;
    const uint32_t major = (token >> 4) & 0xFu;
    const uint32_t minor = token & 0xFu;
    const char* prefix = "unk";
    switch (type) {
        case 0:  prefix = "ps";  break;
        case 1:  prefix = "vs";  break;
        case 2:  prefix = "gs";  break;
        case 3:  prefix = "hs";  break;
        case 4:  prefix = "ds";  break;
        case 5:  prefix = "cs";  break;
        case 6:  prefix = "lib"; break;
        case 13: prefix = "ms";  break;
        case 14: prefix = "as";  break;
        default: break;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s_%u_%u", prefix, major, minor);
    return buf;
}

inline ShaderInspectionInfo InspectShaderBytecodeDetailed(const D3D12_SHADER_BYTECODE& bc) {
    ShaderInspectionInfo info{};
    info.byteLength = bc.BytecodeLength;

    if (!bc.pShaderBytecode || bc.BytecodeLength == 0) {
        info.kind = ShaderContainerKind::None;
        info.summary = "none";
        return info;
    }
    if (bc.BytecodeLength < 4) {
        info.kind = ShaderContainerKind::Unknown;
        info.summary = "RAW_SMALL(" + std::to_string(bc.BytecodeLength) + " B)";
        return info;
    }

    const auto* bytes = static_cast<const uint8_t*>(bc.pShaderBytecode);
    if (std::memcmp(bytes, "SCXS", 4) == 0) {
        info.kind = ShaderContainerKind::Scxs;
        info.profile = "SCXS";
        info.summary = "SCXS_CONTAINER (" + std::to_string(bc.BytecodeLength) + " B)";
        return info;
    }

    if (bc.BytecodeLength < sizeof(DxbcHeader)) {
        info.kind = ShaderContainerKind::Unknown;
        info.summary = "RAW_SMALL(" + std::to_string(bc.BytecodeLength) + " B)";
        return info;
    }

    DxbcHeader hdr{};
    std::memcpy(&hdr, bytes, sizeof(hdr));
    if (std::memcmp(hdr.magic, "DXBC", 4) != 0) {
        info.kind = ShaderContainerKind::Unknown;
        info.summary = "SCARLETT_OR_CUSTOM(" + std::to_string(bc.BytecodeLength) + " B)";
        return info;
    }

    const uint32_t maxParts = hdr.partCount < 32u ? hdr.partCount : 32u;
    const size_t offsetTableEnd = sizeof(DxbcHeader) + static_cast<size_t>(maxParts) * sizeof(uint32_t);
    if (offsetTableEnd > bc.BytecodeLength) {
        info.kind = ShaderContainerKind::Unknown;
        info.summary = "DXBC_TRUNCATED(" + std::to_string(bc.BytecodeLength) + " B)";
        return info;
    }

    info.kind = ShaderContainerKind::Dxbc;
    info.profile = "DXBC";

    for (uint32_t i = 0; i < maxParts; ++i) {
        uint32_t partOff = 0;
        std::memcpy(&partOff, bytes + sizeof(DxbcHeader) + i * sizeof(uint32_t), sizeof(uint32_t));
        if (static_cast<size_t>(partOff) + sizeof(DxbcPartHeader) > bc.BytecodeLength) {
            continue;
        }

        DxbcPartHeader part{};
        std::memcpy(&part, bytes + partOff, sizeof(part));

        char name[5]{ part.fourcc[0], part.fourcc[1], part.fourcc[2], part.fourcc[3], '\0' };
        if (!info.chunks.empty()) {
            info.chunks += ',';
        }
        info.chunks += name;

        const size_t bodyOff = static_cast<size_t>(partOff) + sizeof(DxbcPartHeader);
        const size_t bodyEnd = bodyOff + static_cast<size_t>(part.size);

        if (std::memcmp(part.fourcc, "PSV0", 4) == 0) {
            info.hasPsv0 = true;
        } else if ((std::memcmp(part.fourcc, "SHDR", 4) == 0 || std::memcmp(part.fourcc, "SHEX", 4) == 0) &&
                   bodyOff + sizeof(uint32_t) <= bc.BytecodeLength) {
            uint32_t verToken = 0;
            std::memcpy(&verToken, bytes + bodyOff, sizeof(uint32_t));
            if (info.kind != ShaderContainerKind::Dxil) {
                info.profile = DecodeShaderProgramToken(verToken);
            }
        } else if (std::memcmp(part.fourcc, "DXIL", 4) == 0 &&
                   bodyOff + sizeof(DxilProgramHeader) <= bc.BytecodeLength) {
            info.kind = ShaderContainerKind::Dxil;
            DxilProgramHeader dxilHdr{};
            std::memcpy(&dxilHdr, bytes + bodyOff, sizeof(dxilHdr));
            info.profile = DecodeShaderProgramToken(dxilHdr.programVersion);

            if (dxilHdr.dxilMagic == kDxilMagicFourCc) {
                info.dxilMajor = static_cast<uint16_t>((dxilHdr.dxilVersion >> 8) & 0xFFu);
                info.dxilMinor = static_cast<uint16_t>(dxilHdr.dxilVersion & 0xFFu);
                info.bitcodeSize = dxilHdr.bitcodeSize;

                // bitcodeOffset is relative to dxilMagic (+8 bytes into DxilProgramHeader)
                const size_t bcOff = bodyOff + 8u + static_cast<size_t>(dxilHdr.bitcodeOffset);
                if (bcOff + 4u <= bc.BytecodeLength && bcOff + 4u <= bodyEnd &&
                    bytes[bcOff + 0] == 0x42u && bytes[bcOff + 1] == 0x43u &&
                    bytes[bcOff + 2] == 0xC0u && bytes[bcOff + 3] == 0xDEu) {
                    info.hasLlvmBitcode = true;
                }
            }
        }
    }

    if (info.kind == ShaderContainerKind::Dxil) {
        char dxilTag[64];
        std::snprintf(dxilTag, sizeof(dxilTag), "%s/DXIL %u.%u%s",
                      info.profile.c_str(),
                      static_cast<unsigned>(info.dxilMajor),
                      static_cast<unsigned>(info.dxilMinor),
                      info.hasLlvmBitcode ? " (LLVM BC)" : "");
        info.summary = std::string(dxilTag) + " [" + info.chunks + "] ("
                     + std::to_string(bc.BytecodeLength) + " B)";
    } else {
        info.summary = info.profile + " [" + info.chunks + "] ("
                     + std::to_string(bc.BytecodeLength) + " B)";
    }
    return info;
}

inline std::string InspectShaderBytecode(const D3D12_SHADER_BYTECODE& bc) {
    return InspectShaderBytecodeDetailed(bc).summary;
}

} // namespace sc::d3d12