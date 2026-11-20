#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace sc::d3d12::rdna2 {

// Standard AMD RDNA2 (GFX10.3) 9-bit operand encoding constants
inline constexpr uint16_t kSrcInlineZero     = 128u;
inline constexpr uint16_t kSrcInlineHalf     = 240u; //  0.5f
inline constexpr uint16_t kSrcInlineNegHalf  = 241u; // -0.5f
inline constexpr uint16_t kSrcInlineOne      = 242u; //  1.0f
inline constexpr uint16_t kSrcInlineNegOne   = 243u; // -1.0f
inline constexpr uint16_t kSrcInlineTwo      = 244u; //  2.0f
inline constexpr uint16_t kSrcInlineNegTwo   = 245u; // -2.0f
inline constexpr uint16_t kSrcInlineFour     = 246u; //  4.0f
inline constexpr uint16_t kSrcInlineNegFour  = 247u; // -4.0f
inline constexpr uint16_t kSrcLiteral32      = 255u;
inline constexpr uint16_t kSrcVgprBase       = 256u;

inline constexpr uint16_t VgprSrc(uint8_t idx) {
    return static_cast<uint16_t>(kSrcVgprBase + idx);
}

// Real RDNA2 (GFX10.3) opcode tables
enum SoppOp : uint8_t {
    kSoppNop     = 0x00u,
    kSoppEndpgm  = 0x01u,
    kSoppWaitcnt = 0x0Cu
};

enum Vop1Op : uint8_t {
    kVop1Nop     = 0x00u,
    kVop1MovB32  = 0x01u,
    kVop1RcpF32  = 0x2Au,
    kVop1SqrtF32 = 0x33u
};

enum Vop2Op : uint8_t {
    kVop2AddF32    = 0x03u,
    kVop2SubF32    = 0x04u,
    kVop2SubRevF32 = 0x05u,
    kVop2MulF32    = 0x08u,
    kVop2MinF32    = 0x0Fu,
    kVop2MaxF32    = 0x10u,
    kVop2AddNcU32  = 0x25u,
    kVop2FmacF32   = 0x2Bu
};

enum Vop3Op : uint16_t {
    kVop3MadF32  = 0x141u,
    kVop3FmaF32  = 0x14Bu,
    kVop3Min3F32 = 0x150u,
    kVop3Max3F32 = 0x151u,
    kVop3Med3F32 = 0x152u,
    kVop3MulLoU32 = 0x169u
};

enum MubufOp : uint8_t {
    kMubufStoreDword = 0x1Cu
};

enum class InstFormat : uint8_t {
    Sopp,
    Vop1,
    Vop2,
    Vop3,
    Exp,
    Mubuf
};

struct DecodedInst {
    InstFormat format{InstFormat::Sopp};
    uint16_t   opcode{0};
    uint8_t    vdst{0};
    uint16_t   src0{0};
    uint16_t   src1{0};
    uint16_t   src2{0};
    uint32_t   literal32{0};
    bool       hasLiteral{false};
    uint8_t    absMask{0};
    uint8_t    negMask{0};
    bool       clamp{false};

    // SOPP / EXP fields
    uint16_t   simm16{0};
    uint8_t    expTarget{0};
    uint8_t    expEn{0};
    uint8_t    expVsrc[4]{};
    bool       expDone{false};
    bool       expVm{false};
    bool       expCompr{false};

    // MUBUF fields
    uint8_t    mubufVaddr{0};
    uint8_t    mubufVdata{0};
    uint8_t    mubufSrsrc{0};
    uint16_t   mubufOffset{0};
};

struct DecodedProgram {
    std::vector<DecodedInst> insts;
};

// ============================================================================
// Binary RDNA2 Encoders (for clean-room test shaders & assembler tooling)
// ============================================================================
inline void AppendSopp(std::vector<uint32_t>& words, SoppOp op, uint16_t simm16 = 0) {
    const uint32_t w = (0x17Fu << 23) | ((static_cast<uint32_t>(op) & 0x7Fu) << 16) | simm16;
    words.push_back(w);
}

inline void AppendVop1(std::vector<uint32_t>& words, Vop1Op op, uint8_t vdst, uint16_t src0) {
    const uint32_t w = (0x3Fu << 25)
                     | (static_cast<uint32_t>(vdst) << 17)
                     | (static_cast<uint32_t>(op) << 9)
                     | (static_cast<uint32_t>(src0) & 0x1FFu);
    words.push_back(w);
}

inline void AppendVop1LiteralF32(std::vector<uint32_t>& words, Vop1Op op, uint8_t vdst, float imm) {
    AppendVop1(words, op, vdst, kSrcLiteral32);
    words.push_back(std::bit_cast<uint32_t>(imm));
}

inline void AppendVop2(
    std::vector<uint32_t>& words, Vop2Op op, uint8_t vdst, uint16_t src0, uint8_t vsrc1) {
    const uint32_t w = ((static_cast<uint32_t>(op) & 0x3Fu) << 25)
                     | (static_cast<uint32_t>(vdst) << 17)
                     | (static_cast<uint32_t>(vsrc1) << 9)
                     | (static_cast<uint32_t>(src0) & 0x1FFu);
    words.push_back(w);
}

inline void AppendVop2LiteralF32(
    std::vector<uint32_t>& words, Vop2Op op, uint8_t vdst, float imm0, uint8_t vsrc1) {
    AppendVop2(words, op, vdst, kSrcLiteral32, vsrc1);
    words.push_back(std::bit_cast<uint32_t>(imm0));
}

inline void AppendVop3(
    std::vector<uint32_t>& words,
    Vop3Op op,
    uint8_t vdst,
    uint16_t src0,
    uint16_t src1,
    uint16_t src2,
    bool clamp = false,
    uint8_t absMask = 0,
    uint8_t negMask = 0) {
    const uint32_t w0 = (0x35u << 26)
                      | ((static_cast<uint32_t>(op) & 0x3FFu) << 16)
                      | (clamp ? (1u << 15) : 0u)
                      | ((static_cast<uint32_t>(absMask) & 0x7u) << 8)
                      | static_cast<uint32_t>(vdst);
    const uint32_t w1 = ((static_cast<uint32_t>(negMask) & 0x7u) << 29)
                      | ((static_cast<uint32_t>(src2) & 0x1FFu) << 18)
                      | ((static_cast<uint32_t>(src1) & 0x1FFu) << 9)
                      | (static_cast<uint32_t>(src0) & 0x1FFu);
    words.push_back(w0);
    words.push_back(w1);
}

inline void AppendExpMrt0(
    std::vector<uint32_t>& words,
    uint8_t vsrc0, uint8_t vsrc1, uint8_t vsrc2, uint8_t vsrc3,
    uint8_t enMask = 0xFu, bool done = true, bool vm = true) {
    const uint32_t w0 = (0x3Eu << 26)
                      | (vm ? (1u << 12) : 0u)
                      | (done ? (1u << 11) : 0u)
                      | (0u << 4) // target 0 = MRT0
                      | (static_cast<uint32_t>(enMask) & 0xFu);
    const uint32_t w1 = (static_cast<uint32_t>(vsrc3) << 24)
                      | (static_cast<uint32_t>(vsrc2) << 16)
                      | (static_cast<uint32_t>(vsrc1) << 8)
                      | static_cast<uint32_t>(vsrc0);
    words.push_back(w0);
    words.push_back(w1);
}

inline void AppendVop2LiteralU32(
    std::vector<uint32_t>& words, Vop2Op op, uint8_t vdst, uint32_t imm0, uint8_t vsrc1) {
    AppendVop2(words, op, vdst, kSrcLiteral32, vsrc1);
    words.push_back(imm0);
}

inline void AppendMubufStoreDword(
    std::vector<uint32_t>& words, uint8_t vdata, uint8_t vaddr, uint8_t srsrc = 0, uint16_t offset = 0) {
    const uint32_t w0 = (0x38u << 26)
                      | (static_cast<uint32_t>(kMubufStoreDword) << 18)
                      | (1u << 12) // offen=1
                      | (static_cast<uint32_t>(offset) & 0xFFFu);
    const uint32_t w1 = ((static_cast<uint32_t>(srsrc) & 0x1Fu) << 16)
                      | (static_cast<uint32_t>(vdata) << 8)
                      | static_cast<uint32_t>(vaddr);
    words.push_back(w0);
    words.push_back(w1);
}

// ============================================================================
// Binary RDNA2 Decoder
// ============================================================================
inline bool DecodeProgram(
    const void* data, size_t byteSize, DecodedProgram& outProg, std::string& outError) {
    outProg.insts.clear();
    if (!data || byteSize < 4u || (byteSize % 4u) != 0) {
        outError = "RDNA2 ISA stream must be non-empty and 4-byte aligned";
        return false;
    }

    const size_t wordCount = byteSize / sizeof(uint32_t);
    std::vector<uint32_t> words(wordCount);
    std::memcpy(words.data(), data, byteSize);

    size_t pc = 0;
    while (pc < wordCount) {
        const uint32_t w0 = words[pc++];
        DecodedInst inst{};

        if ((w0 >> 23) == 0x17Fu) {
            inst.format = InstFormat::Sopp;
            inst.opcode = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            inst.simm16 = static_cast<uint16_t>(w0 & 0xFFFFu);
            if (inst.opcode != kSoppNop && inst.opcode != kSoppEndpgm && inst.opcode != kSoppWaitcnt) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unsupported SOPP opcode 0x%02X", inst.opcode);
                outError = buf;
                return false;
            }
            outProg.insts.push_back(inst);
            if (inst.opcode == kSoppEndpgm) {
                break;
            }
            continue;
        }

        if ((w0 >> 25) == 0x3Fu) {
            inst.format = InstFormat::Vop1;
            inst.vdst   = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
            inst.opcode = static_cast<uint16_t>((w0 >> 9) & 0xFFu);
            inst.src0   = static_cast<uint16_t>(w0 & 0x1FFu);
            if (inst.src0 == kSrcLiteral32) {
                if (pc >= wordCount) {
                    outError = "truncated 32-bit literal in VOP1";
                    return false;
                }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            if (inst.opcode != kVop1Nop && inst.opcode != kVop1MovB32 &&
                inst.opcode != kVop1RcpF32 && inst.opcode != kVop1SqrtF32) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unsupported VOP1 opcode 0x%02X", inst.opcode);
                outError = buf;
                return false;
            }
            outProg.insts.push_back(inst);
            continue;
        }

        if ((w0 >> 31) == 0u && (w0 >> 25) != 0x3Eu) {
            inst.format = InstFormat::Vop2;
            inst.opcode = static_cast<uint16_t>((w0 >> 25) & 0x3Fu);
            inst.vdst   = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
            inst.src1   = VgprSrc(static_cast<uint8_t>((w0 >> 9) & 0xFFu));
            inst.src0   = static_cast<uint16_t>(w0 & 0x1FFu);
            if (inst.src0 == kSrcLiteral32) {
                if (pc >= wordCount) {
                    outError = "truncated 32-bit literal in VOP2";
                    return false;
                }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            if (inst.opcode != kVop2AddF32 && inst.opcode != kVop2SubF32 &&
                inst.opcode != kVop2SubRevF32 && inst.opcode != kVop2MulF32 &&
                inst.opcode != kVop2MinF32 && inst.opcode != kVop2MaxF32 &&
                inst.opcode != kVop2AddNcU32 && inst.opcode != kVop2FmacF32) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unsupported VOP2 opcode 0x%02X", inst.opcode);
                outError = buf;
                return false;
            }
            outProg.insts.push_back(inst);
            continue;
        }

        if ((w0 >> 26) == 0x35u) {
            if (pc >= wordCount) {
                outError = "truncated 64-bit VOP3 instruction";
                return false;
            }
            const uint32_t w1 = words[pc++];
            inst.format  = InstFormat::Vop3;
            inst.opcode  = static_cast<uint16_t>((w0 >> 16) & 0x3FFu);
            inst.clamp   = ((w0 >> 15) & 1u) != 0;
            inst.absMask = static_cast<uint8_t>((w0 >> 8) & 0x7u);
            inst.vdst    = static_cast<uint8_t>(w0 & 0xFFu);
            inst.negMask = static_cast<uint8_t>((w1 >> 29) & 0x7u);
            inst.src2    = static_cast<uint16_t>((w1 >> 18) & 0x1FFu);
            inst.src1    = static_cast<uint16_t>((w1 >> 9) & 0x1FFu);
            inst.src0    = static_cast<uint16_t>(w1 & 0x1FFu);
            if (inst.opcode != kVop3MadF32 && inst.opcode != kVop3FmaF32 &&
                inst.opcode != kVop3Min3F32 && inst.opcode != kVop3Max3F32 &&
                inst.opcode != kVop3Med3F32 && inst.opcode != kVop3MulLoU32) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unsupported VOP3 opcode 0x%03X", inst.opcode);
                outError = buf;
                return false;
            }
            outProg.insts.push_back(inst);
            continue;
        }

        if ((w0 >> 26) == 0x3Eu) {
            if (pc >= wordCount) {
                outError = "truncated 64-bit EXP instruction";
                return false;
            }
            const uint32_t w1 = words[pc++];
            inst.format     = InstFormat::Exp;
            inst.expVm      = ((w0 >> 12) & 1u) != 0;
            inst.expDone    = ((w0 >> 11) & 1u) != 0;
            inst.expCompr   = ((w0 >> 10) & 1u) != 0;
            inst.expTarget  = static_cast<uint8_t>((w0 >> 4) & 0x3Fu);
            inst.expEn      = static_cast<uint8_t>(w0 & 0xFu);
            inst.expVsrc[0] = static_cast<uint8_t>(w1 & 0xFFu);
            inst.expVsrc[1] = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            inst.expVsrc[2] = static_cast<uint8_t>((w1 >> 16) & 0xFFu);
            inst.expVsrc[3] = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            if (inst.expCompr) {
                outError = "compressed FP16 EXP not yet supported";
                return false;
            }
            outProg.insts.push_back(inst);
            continue;
        }

        if ((w0 >> 26) == 0x38u) {
            if (pc >= wordCount) {
                outError = "truncated 64-bit MUBUF instruction";
                return false;
            }
            const uint32_t w1 = words[pc++];
            inst.format      = InstFormat::Mubuf;
            inst.opcode      = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
            inst.mubufOffset = static_cast<uint16_t>(w0 & 0xFFFu);
            inst.mubufSrsrc  = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
            inst.mubufVdata  = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            inst.mubufVaddr  = static_cast<uint8_t>(w1 & 0xFFu);
            if (inst.opcode != kMubufStoreDword) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unsupported MUBUF opcode 0x%02X", inst.opcode);
                outError = buf;
                return false;
            }
            outProg.insts.push_back(inst);
            continue;
        }

        char buf[64];
        std::snprintf(buf, sizeof(buf), "unrecognized RDNA2 encoding 0x%08X", w0);
        outError = buf;
        return false;
    }

    if (outProg.insts.empty()) {
        outError = "decoded RDNA2 program is empty";
        return false;
    }
    return true;
}

// ============================================================================
// Disassembler & HLSL Lifter
// ============================================================================
inline std::string FormatOperandAsm(uint16_t src, bool hasLit, uint32_t lit) {
    if (src >= kSrcVgprBase && src < kSrcVgprBase + 256u) {
        return "v" + std::to_string(src - kSrcVgprBase);
    }
    if (src == kSrcLiteral32 && hasLit) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", lit);
        return buf;
    }
    if (src >= 128u && src <= 192u) {
        return std::to_string(static_cast<int>(src) - 128);
    }
    if (src >= 193u && src <= 208u) {
        return std::to_string(192 - static_cast<int>(src));
    }
    switch (src) {
        case kSrcInlineHalf:    return "0.5";
        case kSrcInlineNegHalf: return "-0.5";
        case kSrcInlineOne:     return "1.0";
        case kSrcInlineNegOne:  return "-1.0";
        case kSrcInlineTwo:     return "2.0";
        case kSrcInlineNegTwo:  return "-2.0";
        case kSrcInlineFour:    return "4.0";
        case kSrcInlineNegFour: return "-4.0";
        default:                return "src_" + std::to_string(src);
    }
}

inline std::string Disassemble(const DecodedProgram& prog) {
    std::string out;
    for (const auto& i : prog.insts) {
        switch (i.format) {
            case InstFormat::Sopp:
                if (i.opcode == kSoppEndpgm)      out += "s_endpgm\n";
                else if (i.opcode == kSoppNop)    out += "s_nop\n";
                else                              out += "s_waitcnt\n";
                break;
            case InstFormat::Vop1: {
                const char* mn = (i.opcode == kVop1MovB32)  ? "v_mov_b32"
                               : (i.opcode == kVop1RcpF32)  ? "v_rcp_f32"
                               : (i.opcode == kVop1SqrtF32) ? "v_sqrt_f32"
                                                            : "v_nop";
                out += std::string(mn) + " v" + std::to_string(i.vdst) + ", "
                     + FormatOperandAsm(i.src0, i.hasLiteral, i.literal32) + "\n";
                break;
            }
            case InstFormat::Vop2: {
                const char* mn = "v_op2";
                switch (i.opcode) {
                    case kVop2AddF32:    mn = "v_add_f32"; break;
                    case kVop2SubF32:    mn = "v_sub_f32"; break;
                    case kVop2SubRevF32: mn = "v_subrev_f32"; break;
                    case kVop2MulF32:    mn = "v_mul_f32"; break;
                    case kVop2MinF32:    mn = "v_min_f32"; break;
                    case kVop2MaxF32:    mn = "v_max_f32"; break;
                    case kVop2AddNcU32:  mn = "v_add_nc_u32"; break;
                    case kVop2FmacF32:   mn = "v_fmac_f32"; break;
                    default: break;
                }
                out += std::string(mn) + " v" + std::to_string(i.vdst) + ", "
                     + FormatOperandAsm(i.src0, i.hasLiteral, i.literal32) + ", "
                     + FormatOperandAsm(i.src1, false, 0) + "\n";
                break;
            }
            case InstFormat::Vop3: {
                const char* mn = "v_op3";
                switch (i.opcode) {
                    case kVop3MadF32:  mn = "v_mad_f32"; break;
                    case kVop3FmaF32:  mn = "v_fma_f32"; break;
                    case kVop3Min3F32: mn = "v_min3_f32"; break;
                    case kVop3Max3F32: mn = "v_max3_f32"; break;
                    case kVop3Med3F32:  mn = "v_med3_f32"; break;
                    case kVop3MulLoU32: mn = "v_mul_lo_u32"; break;
                    default: break;
                }
                out += std::string(mn) + " v" + std::to_string(i.vdst) + ", "
                     + FormatOperandAsm(i.src0, false, 0) + ", "
                     + FormatOperandAsm(i.src1, false, 0) + ", "
                     + FormatOperandAsm(i.src2, false, 0)
                     + (i.clamp ? " clamp\n" : "\n");
                break;
            }
            case InstFormat::Exp:
                out += "exp mrt" + std::to_string(i.expTarget) + ", v"
                     + std::to_string(i.expVsrc[0]) + ", v" + std::to_string(i.expVsrc[1])
                     + ", v" + std::to_string(i.expVsrc[2]) + ", v" + std::to_string(i.expVsrc[3])
                     + (i.expDone ? " done" : "") + (i.expVm ? " vm\n" : "\n");
                break;
            case InstFormat::Mubuf:
                out += "buffer_store_dword v" + std::to_string(i.mubufVdata)
                     + ", v" + std::to_string(i.mubufVaddr)
                     + ", s[" + std::to_string(i.mubufSrsrc) + "]\n";
                break;
        }
    }
    return out;
}

inline bool FormatOperandHlsl(
    uint16_t src, bool hasLit, uint32_t lit, bool absMod, bool negMod,
    std::string& outExpr, std::string& outError) {
    std::string base;
    if (src >= kSrcVgprBase && src < kSrcVgprBase + 256u) {
        base = "v" + std::to_string(src - kSrcVgprBase);
    } else if (src == kSrcLiteral32 && hasLit) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "asfloat(0x%08Xu)", lit);
        base = buf;
    } else if (src >= 128u && src <= 192u) {
        base = std::to_string(static_cast<int>(src) - 128) + ".0f";
    } else if (src >= 193u && src <= 208u) {
        base = "(" + std::to_string(192 - static_cast<int>(src)) + ".0f)";
    } else {
        switch (src) {
            case kSrcInlineHalf:    base = "0.5f"; break;
            case kSrcInlineNegHalf: base = "(-0.5f)"; break;
            case kSrcInlineOne:     base = "1.0f"; break;
            case kSrcInlineNegOne:  base = "(-1.0f)"; break;
            case kSrcInlineTwo:     base = "2.0f"; break;
            case kSrcInlineNegTwo:  base = "(-2.0f)"; break;
            case kSrcInlineFour:    base = "4.0f"; break;
            case kSrcInlineNegFour: base = "(-4.0f)"; break;
            default: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unsupported RDNA2 operand code %u", src);
                outError = buf;
                return false;
            }
        }
    }
    if (absMod) {
        base = "abs(" + base + ")";
    }
    if (negMod) {
        base = "(-(" + base + "))";
    }
    outExpr = std::move(base);
    return true;
}

inline bool LiftPixelShaderToHlsl(
    const DecodedProgram& prog, std::string& outHlsl, std::string& outError) {
    std::set<uint8_t> usedVgprs;
    auto markSrc = [&](uint16_t s) {
        if (s >= kSrcVgprBase && s < kSrcVgprBase + 256u) {
            usedVgprs.insert(static_cast<uint8_t>(s - kSrcVgprBase));
        }
    };

    for (const auto& i : prog.insts) {
        switch (i.format) {
            case InstFormat::Vop1:
                usedVgprs.insert(i.vdst);
                markSrc(i.src0);
                break;
            case InstFormat::Vop2:
                usedVgprs.insert(i.vdst);
                markSrc(i.src0);
                markSrc(i.src1);
                break;
            case InstFormat::Vop3:
                usedVgprs.insert(i.vdst);
                markSrc(i.src0);
                markSrc(i.src1);
                markSrc(i.src2);
                break;
            case InstFormat::Exp:
                for (int c = 0; c < 4; ++c) {
                    if ((i.expEn & (1u << c)) != 0) {
                        usedVgprs.insert(i.expVsrc[c]);
                    }
                }
                break;
            case InstFormat::Sopp:
                break;
        }
    }

    std::string body;
    bool hasMrt0Export = false;

    for (const auto& i : prog.insts) {
        if (i.format == InstFormat::Sopp) {
            if (i.opcode == kSoppEndpgm) {
                break;
            }
            continue;
        }

        if (i.format == InstFormat::Vop1) {
            if (i.opcode == kVop1Nop) {
                continue;
            }
            std::string s0;
            if (!FormatOperandHlsl(i.src0, i.hasLiteral, i.literal32, false, false, s0, outError)) {
                return false;
            }
            std::string dst = "v" + std::to_string(i.vdst);
            if (i.opcode == kVop1MovB32)       body += "    " + dst + " = " + s0 + ";\n";
            else if (i.opcode == kVop1RcpF32)  body += "    " + dst + " = rcp(" + s0 + ");\n";
            else if (i.opcode == kVop1SqrtF32) body += "    " + dst + " = sqrt(" + s0 + ");\n";
            continue;
        }

        if (i.format == InstFormat::Vop2) {
            std::string s0, s1;
            if (!FormatOperandHlsl(i.src0, i.hasLiteral, i.literal32, false, false, s0, outError) ||
                !FormatOperandHlsl(i.src1, false, 0, false, false, s1, outError)) {
                return false;
            }
            std::string dst = "v" + std::to_string(i.vdst);
            switch (i.opcode) {
                case kVop2AddF32:    body += "    " + dst + " = (" + s0 + " + " + s1 + ");\n"; break;
                case kVop2SubF32:    body += "    " + dst + " = (" + s0 + " - " + s1 + ");\n"; break;
                case kVop2SubRevF32: body += "    " + dst + " = (" + s1 + " - " + s0 + ");\n"; break;
                case kVop2MulF32:    body += "    " + dst + " = (" + s0 + " * " + s1 + ");\n"; break;
                case kVop2MinF32:    body += "    " + dst + " = min(" + s0 + ", " + s1 + ");\n"; break;
                case kVop2MaxF32:    body += "    " + dst + " = max(" + s0 + ", " + s1 + ");\n"; break;
                case kVop2FmacF32:   body += "    " + dst + " = (" + s0 + " * " + s1 + " + " + dst + ");\n"; break;
                default: break;
            }
            continue;
        }

        if (i.format == InstFormat::Vop3) {
            std::string s0, s1, s2;
            if (!FormatOperandHlsl(i.src0, false, 0, (i.absMask & 1u) != 0, (i.negMask & 1u) != 0, s0, outError) ||
                !FormatOperandHlsl(i.src1, false, 0, (i.absMask & 2u) != 0, (i.negMask & 2u) != 0, s1, outError) ||
                !FormatOperandHlsl(i.src2, false, 0, (i.absMask & 4u) != 0, (i.negMask & 4u) != 0, s2, outError)) {
                return false;
            }
            std::string expr;
            switch (i.opcode) {
                case kVop3MadF32:
                case kVop3FmaF32:  expr = "mad(" + s0 + ", " + s1 + ", " + s2 + ")"; break;
                case kVop3Min3F32: expr = "min(" + s0 + ", min(" + s1 + ", " + s2 + "))"; break;
                case kVop3Max3F32: expr = "max(" + s0 + ", max(" + s1 + ", " + s2 + "))"; break;
                case kVop3Med3F32:
                    expr = "max(min(" + s0 + ", " + s1 + "), min(max(" + s0 + ", " + s1 + "), " + s2 + "))";
                    break;
                default: break;
            }
            if (i.clamp) {
                expr = "saturate(" + expr + ")";
            }
            body += "    v" + std::to_string(i.vdst) + " = " + expr + ";\n";
            continue;
        }

        if (i.format == InstFormat::Exp && i.expTarget == 0u) {
            hasMrt0Export = true;
            std::string ch[4];
            for (int c = 0; c < 4; ++c) {
                ch[c] = ((i.expEn & (1u << c)) != 0)
                      ? ("v" + std::to_string(i.expVsrc[c]))
                      : ((c == 3) ? "1.0f" : "0.0f");
            }
            body += "    mrt0 = float4(" + ch[0] + ", " + ch[1] + ", " + ch[2] + ", " + ch[3] + ");\n";
        }
    }

    if (!hasMrt0Export) {
        outError = "RDNA2 pixel shader contains no EXP MRT0 export";
        return false;
    }

    outHlsl = "float4 PSMain(float4 svPos : SV_POSITION) : SV_TARGET {\n";
    for (uint8_t v : usedVgprs) {
        outHlsl += "    float v" + std::to_string(v) + " = 0.0f;\n";
    }
    outHlsl += "    float4 mrt0 = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
    outHlsl += body;
    outHlsl += "    return mrt0;\n}\n";
    return true;
}

inline bool FormatOperandUintHlsl(
    uint16_t src, bool hasLit, uint32_t lit, std::string& outExpr, std::string& outError) {
    if (src >= kSrcVgprBase && src < kSrcVgprBase + 256u) {
        outExpr = "u" + std::to_string(src - kSrcVgprBase);
        return true;
    }
    if (src == kSrcLiteral32 && hasLit) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%08Xu", lit);
        outExpr = buf;
        return true;
    }
    if (src >= 128u && src <= 192u) {
        outExpr = std::to_string(src - 128u) + "u";
        return true;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "unsupported RDNA2 uint operand %u", src);
    outError = buf;
    return false;
}

inline bool LiftComputeShaderToHlsl(
    const DecodedProgram& prog, std::string& outHlsl, std::string& outError) {
    std::set<uint8_t> usedVgprs{ 0u }; // v0 = SV_DispatchThreadID.x
    auto markSrc = [&](uint16_t s) {
        if (s >= kSrcVgprBase && s < kSrcVgprBase + 256u) {
            usedVgprs.insert(static_cast<uint8_t>(s - kSrcVgprBase));
        }
    };

    for (const auto& i : prog.insts) {
        switch (i.format) {
            case InstFormat::Vop1:  usedVgprs.insert(i.vdst); markSrc(i.src0); break;
            case InstFormat::Vop2:  usedVgprs.insert(i.vdst); markSrc(i.src0); markSrc(i.src1); break;
            case InstFormat::Vop3:  usedVgprs.insert(i.vdst); markSrc(i.src0); markSrc(i.src1); markSrc(i.src2); break;
            case InstFormat::Mubuf: usedVgprs.insert(i.mubufVaddr); usedVgprs.insert(i.mubufVdata); break;
            default: break;
        }
    }

    std::string body;
    bool hasBufferStore = false;

    for (const auto& i : prog.insts) {
        if (i.format == InstFormat::Sopp) {
            if (i.opcode == kSoppEndpgm) break;
            continue;
        }
        if (i.format == InstFormat::Vop1 && i.opcode == kVop1MovB32) {
            std::string s0;
            if (!FormatOperandUintHlsl(i.src0, i.hasLiteral, i.literal32, s0, outError)) return false;
            body += "    u" + std::to_string(i.vdst) + " = " + s0 + ";\n";
        } else if (i.format == InstFormat::Vop2 && i.opcode == kVop2AddNcU32) {
            std::string s0, s1;
            if (!FormatOperandUintHlsl(i.src0, i.hasLiteral, i.literal32, s0, outError) ||
                !FormatOperandUintHlsl(i.src1, false, 0, s1, outError)) return false;
            body += "    u" + std::to_string(i.vdst) + " = (" + s0 + " + " + s1 + ");\n";
        } else if (i.format == InstFormat::Vop3 && i.opcode == kVop3MulLoU32) {
            std::string s0, s1;
            if (!FormatOperandUintHlsl(i.src0, false, 0, s0, outError) ||
                !FormatOperandUintHlsl(i.src1, false, 0, s1, outError)) return false;
            body += "    u" + std::to_string(i.vdst) + " = (" + s0 + " * " + s1 + ");\n";
        } else if (i.format == InstFormat::Mubuf && i.opcode == kMubufStoreDword) {
            hasBufferStore = true;
            body += "    gOut[u" + std::to_string(i.mubufVaddr) + "] = u" + std::to_string(i.mubufVdata) + ";\n";
        }
    }

    if (!hasBufferStore) {
        outError = "RDNA2 compute shader contains no MUBUF buffer_store_dword";
        return false;
    }

    outHlsl =
        "RWStructuredBuffer<uint> gOut : register(u0);\n"
        "[numthreads(64, 1, 1)]\n"
        "void CSMain(uint3 dtid : SV_DispatchThreadID) {\n";
    for (uint8_t v : usedVgprs) {
        if (v == 0u) outHlsl += "    uint u0 = dtid.x;\n";
        else         outHlsl += "    uint u" + std::to_string(v) + " = 0u;\n";
    }
    outHlsl += body;
    outHlsl += "}\n";
    return true;
}

} // namespace sc::d3d12::rdna2