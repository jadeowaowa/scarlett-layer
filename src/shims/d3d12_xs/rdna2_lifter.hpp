#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace sc::d3d12::rdna2 {

struct LiftOptions {
    bool byteAddressedBuffers{false};
    bool allowFallbackOps{false};
};

namespace detail {

inline bool IsBranchOp(const DecodedInst& i) {
    if (i.format != InstFormat::Sopp) return false;
    switch (i.opcode) {
        case kSoppBranch: case kSoppCbranchScc0: case kSoppCbranchScc1:
        case kSoppCbranchVccz: case kSoppCbranchVccnz:
        case kSoppCbranchExecz: case kSoppCbranchExecnz:
            return true;
        default: return false;
    }
}

inline std::string U32Hex(uint32_t v) {
    char b[20];
    std::snprintf(b, sizeof(b), "0x%08Xu", v);
    return b;
}

inline bool FmtU(uint16_t src, bool hasLit, uint32_t lit, std::string& out, std::string& err) {
    if (src <= kSrcSgprMax) { out = "s" + std::to_string(src); return true; }
    if (src >= kSrcVgprBase && src < kSrcVgprBase + 256u) {
        out = "v" + std::to_string(src - kSrcVgprBase);
        return true;
    }
    if (src == kSrcLiteral32 && hasLit) { out = U32Hex(lit); return true; }
    if (src >= 128u && src <= 192u) { out = std::to_string(src - 128u) + "u"; return true; }
    if (src >= 193u && src <= 208u) {
        out = U32Hex(static_cast<uint32_t>(192 - static_cast<int32_t>(src)));
        return true;
    }
    switch (src) {
        case kSrcVccLo:         out = "_vcc"; return true;
        case kSrcVccHi:         out = "0u"; return true;
        case kSrcM0:            out = "_m0"; return true;
        case kSrcNull:          out = "0u"; return true;
        case kSrcExecLo:        out = "_exec"; return true;
        case kSrcExecHi:        out = "0u"; return true;
        case kSrcScc:           out = "_scc"; return true;
        case kSrcVccz:          out = "(_vcc == 0u ? 1u : 0u)"; return true;
        case kSrcExecz:         out = "(_exec == 0u ? 1u : 0u)"; return true;
        case kSrcInlineHalf:    out = "0x3F000000u"; return true;
        case kSrcInlineNegHalf: out = "0xBF000000u"; return true;
        case kSrcInlineOne:     out = "0x3F800000u"; return true;
        case kSrcInlineNegOne:  out = "0xBF800000u"; return true;
        case kSrcInlineTwo:     out = "0x40000000u"; return true;
        case kSrcInlineNegTwo:  out = "0xC0000000u"; return true;
        case kSrcInlineFour:    out = "0x40800000u"; return true;
        case kSrcInlineNegFour: out = "0xC0800000u"; return true;
        case kSrcInlineInv2Pi:  out = "0x3E22F983u"; return true;
        case kSrcSdwa: case kSrcDpp16:
            out = "v" + std::to_string(lit & 0xFFu);
            return true;
        default: {
            char b[64];
            std::snprintf(b, sizeof(b), "unsupported RDNA2 operand code %u", src);
            err = b;
            return false;
        }
    }
}

inline bool FmtF(uint16_t src, bool hasLit, uint32_t lit, bool absM, bool negM,
                 std::string& out, std::string& err) {
    std::string u;
    if (!FmtU(src, hasLit, lit, u, err)) return false;
    std::string e = "asfloat(" + u + ")";
    if (absM) e = "abs(" + e + ")";
    if (negM) e = "(-(" + e + "))";
    out = std::move(e);
    return true;
}

inline std::string SdstLhs(uint8_t sdst) {
    if (sdst <= kSrcSgprMax) return "s" + std::to_string(sdst);
    if (sdst == kSrcVccLo)   return "_vcc";
    if (sdst == kSrcExecLo)  return "_exec";
    if (sdst == kSrcM0)      return "_m0";
    return "_discard";
}

inline std::string EmitCmpExpr(uint16_t op, const std::string& aU, const std::string& bU,
                               const std::string& aF, const std::string& bF, bool& writesExec) {
    writesExec = false;
    uint16_t base = op;
    if ((op >= 0x10u && op <= 0x1Fu) || (op >= 0x50u && op <= 0x5Fu) ||
        (op >= 0x90u && op <= 0x9Fu) || (op >= 0xD0u && op <= 0xDFu)) {
        writesExec = true;
        base = static_cast<uint16_t>(op - 0x10u);
    }
    const uint8_t pred = static_cast<uint8_t>(base & 0xFu);
    if (base < 0x10u || (base >= 0x40u && base < 0x50u)) {
        switch (pred) {
            case 0x0: return "false";
            case 0x1: return "(" + aF + " < " + bF + ")";
            case 0x2: return "(" + aF + " == " + bF + ")";
            case 0x3: return "(" + aF + " <= " + bF + ")";
            case 0x4: return "(" + aF + " > " + bF + ")";
            case 0x5: return "(" + aF + " != " + bF + ")";
            case 0x6: return "(" + aF + " >= " + bF + ")";
            case 0x7: return "(!isnan(" + aF + ") && !isnan(" + bF + "))";
            case 0x8: return "(isnan(" + aF + ") || isnan(" + bF + "))";
            case 0x9: return "(!(" + aF + " >= " + bF + "))";
            case 0xA: return "(!(" + aF + " != " + bF + "))";
            case 0xB: return "(!(" + aF + " > " + bF + "))";
            case 0xC: return "(!(" + aF + " <= " + bF + "))";
            case 0xD: return "(!(" + aF + " == " + bF + "))";
            case 0xE: return "(!(" + aF + " < " + bF + "))";
            default:  return "true";
        }
    }
    const bool isSigned = (base >= 0x80u && base < 0x90u);
    const std::string a = isSigned ? ("asint(" + aU + ")") : aU;
    const std::string b = isSigned ? ("asint(" + bU + ")") : bU;
    switch (pred) {
        case 0x1: return "(" + a + " < " + b + ")";
        case 0x2: return "(" + a + " == " + b + ")";
        case 0x3: return "(" + a + " <= " + b + ")";
        case 0x4: return "(" + a + " > " + b + ")";
        case 0x5: return "(" + a + " != " + b + ")";
        case 0x6: return "(" + a + " >= " + b + ")";
        case 0x7: return "true";
        default:  return "false";
    }
}

struct UsageScan {
    std::set<uint8_t> vgprs;
    std::set<uint8_t> sgprs;
    std::set<uint8_t> interpParams;
    std::set<uint8_t> exportParams;
    bool hasSmem{false};
    bool hasMimgSample{false};
    bool hasMimgLoad{false};
    bool hasMimgStore{false};
    bool hasBufLoad{false};
    bool hasBufStore{false};
    bool hasLds{false};
    bool hasScratch{false};
    bool hasBranches{false};
    bool usesVsColor{false};
    bool usesGidX{false};
    uint32_t maxCbvDwords{64u};
};

inline void ScanProgram(const DecodedProgram& prog, bool isVs, UsageScan& u) {
    auto markSrc = [&](uint16_t s, uint32_t lit = 0u) {
        if (s <= kSrcSgprMax) u.sgprs.insert(static_cast<uint8_t>(s));
        else if (s >= kSrcVgprBase && s < kSrcVgprBase + 256u) {
            const uint8_t v = static_cast<uint8_t>(s - kSrcVgprBase);
            u.vgprs.insert(v);
            if (isVs && v >= 4u && v <= 7u) u.usesVsColor = true;
        } else if (s == kSrcSdwa || s == kSrcDpp16) {
            const uint8_t v = static_cast<uint8_t>(lit & 0xFFu);
            u.vgprs.insert(v);
            if (isVs && v >= 4u && v <= 7u) u.usesVsColor = true;
        }
    };
    auto markSdst = [&](uint8_t s, uint8_t cnt = 1u) {
        for (uint8_t k = 0; k < cnt; ++k) {
            if (static_cast<uint16_t>(s) + k <= kSrcSgprMax) {
                u.sgprs.insert(static_cast<uint8_t>(s + k));
            }
        }
    };
    auto markV = [&](uint8_t v, uint8_t cnt = 1u) {
        for (uint8_t k = 0; k < cnt; ++k) {
            if (static_cast<uint16_t>(v) + k < 256u) {
                u.vgprs.insert(static_cast<uint8_t>(v + k));
            }
        }
    };

    for (const auto& i : prog.insts) {
        if (IsBranchOp(i)) u.hasBranches = true;
        switch (i.format) {
            case InstFormat::Sop1:
                markSdst(i.sdst, (i.opcode == kSop1MovB64 || i.opcode == kSop1NotB64 || i.opcode == kSop1GetpcB64) ? 2u : 1u);
                markSrc(i.src0);
                break;
            case InstFormat::Sop2:
                markSdst(i.sdst, (i.opcode == kSop2AndB64 || i.opcode == kSop2OrB64 || i.opcode == kSop2XorB64) ? 2u : 1u);
                markSrc(i.src0); markSrc(i.src1);
                break;
            case InstFormat::Sopc:
                markSrc(i.src0); markSrc(i.src1);
                break;
            case InstFormat::Sopk:
                markSdst(i.sdst);
                break;
            case InstFormat::Vop1:
                if (i.opcode == kVop1Readfirstlane) markSdst(i.vdst);
                else markV(i.vdst);
                markSrc(i.src0, i.literal32);
                break;
            case InstFormat::Vop2:
                markV(i.vdst); markSrc(i.src0, i.literal32); markSrc(i.src1, i.literal32);
                break;
            case InstFormat::Vopc:
                markSrc(i.src0, i.literal32); markSrc(i.src1, i.literal32);
                break;
            case InstFormat::Vop3:
                if (i.opcode < 0x100u) {
                    markSdst(i.vdst);
                } else if (i.opcode == kVop3ReadlaneB32) {
                    markSdst(i.vdst);
                } else {
                    const bool is64Dst = (i.opcode == kVop3MadU64U32 || i.opcode == kVop3LshlrevB64 ||
                                          i.opcode == kVop3LshrrevB64 || i.opcode == kVop3AshrrevI64);
                    markV(i.vdst, is64Dst ? 2u : 1u);
                    if (is64Dst && i.src1 >= kSrcVgprBase && i.src1 + 1u < kSrcVgprBase + 256u) {
                        markV(static_cast<uint8_t>((i.src1 - kSrcVgprBase) + 1u));
                    }
                    if (i.opcode == kVop3AddCoU32 || i.opcode == kVop3SubCoU32 || i.opcode == kVop3MadU64U32) {
                        markSdst(i.sdst);
                    }
                }
                if (i.opcode == kVop3LshlAddU32 && i.src0 == 2u) u.usesGidX = true;
                markSrc(i.src0, i.literal32); markSrc(i.src1, i.literal32); markSrc(i.src2, i.literal32);
                break;
            case InstFormat::Vop3p:
                markV(i.vdst); markSrc(i.src0, i.literal32); markSrc(i.src1, i.literal32); markSrc(i.src2, i.literal32);
                break;
            case InstFormat::Smem: {
                u.hasSmem = true;
                const uint8_t cnt = std::max<uint8_t>(1u, SmemDwordCount(i.opcode));
                markSdst(i.smemSdata, cnt);
                if ((i.smemOffset % 4u) == 0u) {
                    const uint32_t endDw = (i.smemOffset / 4u) + cnt;
                    if (endDw <= 16384u && endDw > u.maxCbvDwords) {
                        u.maxCbvDwords = endDw;
                    }
                }
                break;
            }
            case InstFormat::Vintrp:
                markV(i.vdst); u.interpParams.insert(i.vintrpAttr);
                break;
            case InstFormat::Mimg:
                if (i.opcode == kMimgImageLoad) u.hasMimgLoad = true;
                else if (i.opcode == kMimgImageStore) u.hasMimgStore = true;
                else u.hasMimgSample = true;
                markV(i.mimgVaddr, 3u); markV(i.mimgVdata, 4u);
                break;
            case InstFormat::Mubuf:
            case InstFormat::Mtbuf: {
                const bool isStore = (i.format == InstFormat::Mubuf && i.opcode >= 0x1Cu && i.opcode <= 0x1Fu)
                                  || (i.format == InstFormat::Mubuf && i.opcode >= 0x30u && i.opcode <= 0x4Fu);
                if (isStore) u.hasBufStore = true;
                else u.hasBufLoad = true;
                markV(i.mubufVaddr); markV(i.mubufVdata, 4u);
                break;
            }
            case InstFormat::Ds:
                u.hasLds = true;
                markV(i.dsAddr); markV(i.dsData0, 4u); markV(i.dsData1, 4u); markV(i.vdst, 4u);
                break;
            case InstFormat::Flat:
                u.hasScratch = true;
                markV(i.dsAddr, 2u); markV(i.dsData0, 4u); markV(i.vdst, 4u);
                break;
            case InstFormat::Exp:
                if (i.expTarget >= kExpTargetParam0) {
                    u.exportParams.insert(static_cast<uint8_t>(i.expTarget - kExpTargetParam0));
                }
                for (int c = 0; c < 4; ++c) {
                    if (i.expCompr ? (c < 2 && ((i.expEn & (3u << (c * 2))) != 0u)) : ((i.expEn & (1u << c)) != 0u)) {
                        markV(i.expVsrc[c]);
                        if (isVs && i.expVsrc[c] >= 4u && i.expVsrc[c] <= 7u) u.usesVsColor = true;
                    }
                }
                break;
            default: break;
        }
    }
}

inline bool EmitCommonInst(
    const DecodedInst& i, bool isCompute, const LiftOptions& opts,
    std::string& out, bool& hasBufLoad, bool& hasBufStore, std::string& err) {
    static constexpr char kCh[4] = { 'x', 'y', 'z', 'w' };
    const std::string ind = "    ";

    if (i.format == InstFormat::Sopp) {
        if (i.opcode == kSoppBarrier && isCompute) {
            out += ind + "GroupMemoryBarrierWithGroupSync();\n";
        }
        return true;
    }

    if (i.format == InstFormat::Sop1) {
        std::string s0;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, s0, err)) return false;
        const std::string d = SdstLhs(i.sdst);
        switch (i.opcode) {
            case kSop1MovB32: case kSop1MovB64:
            case kSop1WqmB32: case kSop1WqmB64:
            case kSop1QuadmaskB32: case kSop1QuadmaskB64:
                out += ind + d + " = " + s0 + ";\n"; break;
            case kSop1CmovB32: case kSop1CmovB64:
                out += ind + "if (_scc != 0u) " + d + " = " + s0 + ";\n"; break;
            case kSop1NotB32: case kSop1NotB64:
                out += ind + d + " = (~(" + s0 + ")); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop1BrevB32: case kSop1BrevB64:
                out += ind + d + " = reversebits(" + s0 + ");\n"; break;
            case kSop1Bcnt0I32B32:
                out += ind + d + " = countbits(~(" + s0 + ")); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop1Bcnt1I32B32: case kSop1Bcnt1I32B64:
                out += ind + d + " = countbits(" + s0 + "); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop1Ff0I32B32:
                out += ind + d + " = firstbitlow(~(" + s0 + "));\n"; break;
            case kSop1Ff1I32B32: case kSop1Ff1I32B64:
                out += ind + d + " = firstbitlow(" + s0 + ");\n"; break;
            case kSop1FlbitI32B32:
                out += ind + d + " = firstbithigh(" + s0 + ");\n"; break;
            case kSop1SextI32I8:
                out += ind + d + " = asuint((asint(" + s0 + ") << 24) >> 24);\n"; break;
            case kSop1SextI32I16:
                out += ind + d + " = asuint((asint(" + s0 + ") << 16) >> 16);\n"; break;
            case kSop1Bitset0B32:
                out += ind + d + " = (" + d + " & (~(1u << (" + s0 + " & 31u))));\n"; break;
            case kSop1Bitset1B32:
                out += ind + d + " = (" + d + " | (1u << (" + s0 + " & 31u)));\n"; break;
            case kSop1AbsI32:
                out += ind + "{ int _v = asint(" + s0 + "); " + d + " = asuint(abs(_v)); _scc = (" + d + " != 0u) ? 1u : 0u; }\n"; break;
            case kSop1GetpcB64:
                out += ind + d + " = 0u;\n"; break;
            case kSop1AndSaveexecB32: case kSop1AndSaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (_old & _src); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1OrSaveexecB32: case kSop1OrSaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (_old | _src); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1XorSaveexecB32: case kSop1XorSaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (_old ^ _src); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1Andn2SaveexecB32: case kSop1Andn2SaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (_src & (~_old)); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1Orn2SaveexecB32: case kSop1Orn2SaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (_src | (~_old)); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1NandSaveexecB32: case kSop1NandSaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (~(_old & _src)); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1NorSaveexecB32: case kSop1NorSaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (~(_old | _src)); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1XnorSaveexecB32: case kSop1XnorSaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = (~(_old ^ _src)); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1Andn1SaveexecB32: case kSop1Andn1SaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = ((~_src) & _old); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            case kSop1Orn1SaveexecB32: case kSop1Orn1SaveexecB64:
                out += ind + "{ uint _old = _exec; uint _src = " + s0 + "; " + d + " = _old; _exec = ((~_src) | _old); _scc = (_exec != 0u) ? 1u : 0u; }\n"; break;
            default:
                if (!opts.allowFallbackOps) {
                    char b[64];
                    std::snprintf(b, sizeof(b), "unsupported SOP1 opcode 0x%02X", i.opcode);
                    err = b;
                    return false;
                }
                out += ind + d + " = " + s0 + ";\n"; break;
        }
        return true;
    }

    if (i.format == InstFormat::Sop2) {
        std::string a, b;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, a, err) ||
            !FmtU(i.src1, i.hasLiteral, i.literal32, b, err)) return false;
        const std::string d = SdstLhs(i.sdst);
        switch (i.opcode) {
            case kSop2AddU32: case kSop2AddI32:
                out += ind + "{ uint _r = (" + a + " + " + b + "); _scc = (_r < " + a + ") ? 1u : 0u; " + d + " = _r; }\n"; break;
            case kSop2SubU32: case kSop2SubI32:
                out += ind + "{ uint _r = (" + a + " - " + b + "); _scc = (" + b + " > " + a + ") ? 1u : 0u; " + d + " = _r; }\n"; break;
            case kSop2AddcU32:
                out += ind + "{ uint _r = (" + a + " + " + b + " + _scc); _scc = (_r < " + a + ") ? 1u : 0u; " + d + " = _r; }\n"; break;
            case kSop2MinI32: out += ind + d + " = asuint(min(asint(" + a + "), asint(" + b + ")));\n"; break;
            case kSop2MinU32: out += ind + d + " = min(" + a + ", " + b + ");\n"; break;
            case kSop2MaxI32: out += ind + d + " = asuint(max(asint(" + a + "), asint(" + b + ")));\n"; break;
            case kSop2MaxU32: out += ind + d + " = max(" + a + ", " + b + ");\n"; break;
            case kSop2CselectB32: case kSop2CselectB64:
                out += ind + d + " = (_scc != 0u) ? " + a + " : " + b + ";\n"; break;
            case kSop2AndB32: case kSop2AndB64:
                out += ind + d + " = (" + a + " & " + b + "); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2OrB32: case kSop2OrB64:
                out += ind + d + " = (" + a + " | " + b + "); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2XorB32: case kSop2XorB64:
                out += ind + d + " = (" + a + " ^ " + b + "); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2Andn2B32: case kSop2Andn2B64:
                out += ind + d + " = (" + a + " & (~(" + b + "))); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2Orn2B32: case kSop2Orn2B64:
                out += ind + d + " = (" + a + " | (~(" + b + "))); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2LshlB32:
                out += ind + d + " = (" + a + " << (" + b + " & 31u)); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2LshrB32:
                out += ind + d + " = (" + a + " >> (" + b + " & 31u)); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2AshrI32:
                out += ind + d + " = asuint(asint(" + a + ") >> (" + b + " & 31u)); _scc = (" + d + " != 0u) ? 1u : 0u;\n"; break;
            case kSop2MulI32:
                out += ind + d + " = (" + a + " * " + b + ");\n"; break;
            case kSop2MulHiU32:
                out += ind + "{ uint _a = " + a + ", _b = " + b + "; uint _a0 = _a & 0xFFFFu, _a1 = _a >> 16u, _b0 = _b & 0xFFFFu, _b1 = _b >> 16u; uint _m = ((_a0 * _b0) >> 16u) + ((_a0 * _b1) & 0xFFFFu) + ((_a1 * _b0) & 0xFFFFu); " + d + " = (_a1 * _b1) + ((_a0 * _b1) >> 16u) + ((_a1 * _b0) >> 16u) + (_m >> 16u); }\n"; break;
            case kSop2BfeU32:
                out += ind + "{ uint _off = (" + b + " & 31u); uint _w = ((" + b + " >> 16u) & 127u); "
                     + d + " = (_w == 0u) ? 0u : ((_w >= 32u) ? (" + a + " >> _off) : ((" + a + " >> _off) & ((1u << _w) - 1u))); }\n"; break;
            case kSop2BfeI32:
                out += ind + "{ uint _off = (" + b + " & 31u); uint _w = ((" + b + " >> 16u) & 127u); "
                     + d + " = (_w == 0u) ? 0u : asuint(asint(" + a + " << (32u - min(32u, _off + _w))) >> (32u - min(32u, _w))); }\n"; break;
            default:
                if (!opts.allowFallbackOps) { err = "unsupported SOP2 opcode"; return false; }
                out += ind + d + " = (" + a + " ^ " + b + ");\n"; break;
        }
        return true;
    }

    if (i.format == InstFormat::Sopc) {
        std::string a, b;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, a, err) ||
            !FmtU(i.src1, i.hasLiteral, i.literal32, b, err)) return false;
        std::string cond = "false";
        switch (i.opcode) {
            case kSopcCmpEqI32: case kSopcCmpEqU32: cond = "(" + a + " == " + b + ")"; break;
            case kSopcCmpLgI32: case kSopcCmpLgU32: cond = "(" + a + " != " + b + ")"; break;
            case kSopcCmpGtI32: cond = "(asint(" + a + ") > asint(" + b + "))"; break;
            case kSopcCmpGeI32: cond = "(asint(" + a + ") >= asint(" + b + "))"; break;
            case kSopcCmpLtI32: cond = "(asint(" + a + ") < asint(" + b + "))"; break;
            case kSopcCmpLeI32: cond = "(asint(" + a + ") <= asint(" + b + "))"; break;
            case kSopcCmpGtU32: cond = "(" + a + " > " + b + ")"; break;
            case kSopcCmpGeU32: cond = "(" + a + " >= " + b + ")"; break;
            case kSopcCmpLtU32: cond = "(" + a + " < " + b + ")"; break;
            case kSopcCmpLeU32: cond = "(" + a + " <= " + b + ")"; break;
            case kSopcBitcmp0B32: cond = "(((" + b + " >> (" + a + " & 31u)) & 1u) == 0u)"; break;
            case kSopcBitcmp1B32: cond = "(((" + b + " >> (" + a + " & 31u)) & 1u) != 0u)"; break;
            default: break;
        }
        out += ind + "_scc = " + cond + " ? 1u : 0u;\n";
        return true;
    }

    if (i.format == InstFormat::Sopk) {
        const std::string d = SdstLhs(i.sdst);
        const std::string immS = std::to_string(static_cast<int32_t>(static_cast<int16_t>(i.simm16)));
        const std::string immU = std::to_string(static_cast<uint32_t>(i.simm16)) + "u";
        switch (i.opcode) {
            case kSopkMovkI32:  out += ind + d + " = asuint(int(" + immS + "));\n"; break;
            case kSopkCmovkI32: out += ind + "if (_scc != 0u) " + d + " = asuint(int(" + immS + "));\n"; break;
            case kSopkAddkI32:  out += ind + d + " = asuint(asint(" + d + ") + int(" + immS + "));\n"; break;
            case kSopkMulkI32:  out += ind + d + " = asuint(asint(" + d + ") * int(" + immS + "));\n"; break;
            case kSopkCmpkEqI32: out += ind + "_scc = (asint(" + d + ") == int(" + immS + ")) ? 1u : 0u;\n"; break;
            case kSopkCmpkLgI32: out += ind + "_scc = (asint(" + d + ") != int(" + immS + ")) ? 1u : 0u;\n"; break;
            case kSopkCmpkGtI32: out += ind + "_scc = (asint(" + d + ") > int(" + immS + ")) ? 1u : 0u;\n"; break;
            case kSopkCmpkGeI32: out += ind + "_scc = (asint(" + d + ") >= int(" + immS + ")) ? 1u : 0u;\n"; break;
            case kSopkCmpkLtI32: out += ind + "_scc = (asint(" + d + ") < int(" + immS + ")) ? 1u : 0u;\n"; break;
            case kSopkCmpkLeI32: out += ind + "_scc = (asint(" + d + ") <= int(" + immS + ")) ? 1u : 0u;\n"; break;
            case kSopkCmpkEqU32: out += ind + "_scc = (" + d + " == " + immU + ") ? 1u : 0u;\n"; break;
            case kSopkCmpkLgU32: out += ind + "_scc = (" + d + " != " + immU + ") ? 1u : 0u;\n"; break;
            case kSopkCmpkGtU32: out += ind + "_scc = (" + d + " > " + immU + ") ? 1u : 0u;\n"; break;
            case kSopkCmpkGeU32: out += ind + "_scc = (" + d + " >= " + immU + ") ? 1u : 0u;\n"; break;
            case kSopkCmpkLtU32: out += ind + "_scc = (" + d + " < " + immU + ") ? 1u : 0u;\n"; break;
            case kSopkCmpkLeU32: out += ind + "_scc = (" + d + " <= " + immU + ") ? 1u : 0u;\n"; break;
            default: break; // setreg / waitcnt_vscnt
        }
        return true;
    }

    if (i.format == InstFormat::Smem) {
        const uint8_t cnt = SmemDwordCount(i.opcode);
        if (cnt == 0u) return true;
        const bool oob = (i.smemOffset % 4u) != 0u || (i.smemOffset / 4u) + cnt > 16384u;
        if (oob && !opts.allowFallbackOps) {
            err = "SMEM CBV offset out of range for Cb0 (max 65536 B)";
            return false;
        }
        for (uint8_t k = 0; k < cnt; ++k) {
            const uint32_t dw = oob ? (((i.smemOffset / 4u) + k) % 64u) : ((i.smemOffset / 4u) + k);
            const std::string lhs = SdstLhs(static_cast<uint8_t>(i.smemSdata + k));
            out += ind + lhs + " = asuint(gCb0[" + std::to_string(dw / 4u) + "]." + kCh[dw % 4u] + ");\n";
        }
        return true;
    }

    if (i.format == InstFormat::Vop1) {
        if (i.opcode == kVop1Nop) return true;
        std::string u0, f0;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, u0, err) ||
            !FmtF(i.src0, i.hasLiteral, i.literal32, false, false, f0, err)) return false;
        const std::string d = "v" + std::to_string(i.vdst);
        switch (i.opcode) {
            case kVop1MovB32:        out += ind + d + " = " + u0 + ";\n"; break;
            case kVop1Readfirstlane: out += ind + SdstLhs(i.vdst) + " = " + u0 + ";\n"; break;
            case kVop1CvtF32I32:     out += ind + d + " = asuint(float(asint(" + u0 + ")));\n"; break;
            case kVop1CvtF32U32:     out += ind + d + " = asuint(float(" + u0 + "));\n"; break;
            case kVop1CvtU32F32:     out += ind + d + " = uint(max(0.0f, " + f0 + "));\n"; break;
            case kVop1CvtI32F32:     out += ind + d + " = asuint(int(" + f0 + "));\n"; break;
            case kVop1CvtF16F32:     out += ind + d + " = f32tof16(" + f0 + ");\n"; break;
            case kVop1CvtF32F16:     out += ind + d + " = asuint(f16tof32(" + u0 + " & 0xFFFFu));\n"; break;
            case kVop1CvtF32Ubyte0:  out += ind + d + " = asuint(float(" + u0 + " & 0xFFu));\n"; break;
            case kVop1FractF32:      out += ind + d + " = asuint(frac(" + f0 + "));\n"; break;
            case kVop1TruncF32:      out += ind + d + " = asuint(trunc(" + f0 + "));\n"; break;
            case kVop1CeilF32:       out += ind + d + " = asuint(ceil(" + f0 + "));\n"; break;
            case kVop1RndneF32:      out += ind + d + " = asuint(round(" + f0 + "));\n"; break;
            case kVop1FloorF32:      out += ind + d + " = asuint(floor(" + f0 + "));\n"; break;
            case kVop1ExpF32:        out += ind + d + " = asuint(exp2(" + f0 + "));\n"; break;
            case kVop1LogF32:        out += ind + d + " = asuint(log2(" + f0 + "));\n"; break;
            case kVop1RcpF32: case kVop1RcpIflagF32:
                                     out += ind + d + " = asuint(rcp(" + f0 + "));\n"; break;
            case kVop1RsqF32:        out += ind + d + " = asuint(rsqrt(" + f0 + "));\n"; break;
            case kVop1SqrtF32:       out += ind + d + " = asuint(sqrt(" + f0 + "));\n"; break;
            case kVop1SinF32:        out += ind + d + " = asuint(sin(" + f0 + " * 6.28318530718f));\n"; break;
            case kVop1CosF32:        out += ind + d + " = asuint(cos(" + f0 + " * 6.28318530718f));\n"; break;
            case kVop1NotB32:        out += ind + d + " = (~(" + u0 + "));\n"; break;
            case kVop1BfrevB32:      out += ind + d + " = reversebits(" + u0 + ");\n"; break;
            case kVop1FfblB32:       out += ind + d + " = firstbitlow(" + u0 + ");\n"; break;
            default:
                if (!opts.allowFallbackOps) { err = "unsupported VOP1 opcode"; return false; }
                out += ind + d + " = " + u0 + ";\n"; break;
        }
        return true;
    }

    if (i.format == InstFormat::Vop2) {
        std::string u0, u1, f0, f1;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, u0, err) ||
            !FmtU(i.src1, false, 0, u1, err) ||
            !FmtF(i.src0, i.hasLiteral, i.literal32, false, false, f0, err) ||
            !FmtF(i.src1, false, 0, false, false, f1, err)) return false;
        const std::string d = "v" + std::to_string(i.vdst);
        const std::string litF = "asfloat(" + U32Hex(i.literal32) + ")";
        switch (i.opcode) {
            case kVop2CndmaskB32:   out += ind + d + " = ((_vcc & 1u) != 0u) ? " + u1 + " : " + u0 + ";\n"; break;
            case kVop2AddF32:       out += ind + d + " = asuint(" + f0 + " + " + f1 + ");\n"; break;
            case kVop2SubF32:       out += ind + d + " = asuint(" + f0 + " - " + f1 + ");\n"; break;
            case kVop2SubRevF32:    out += ind + d + " = asuint(" + f1 + " - " + f0 + ");\n"; break;
            case kVop2MulLegacyF32:
            case kVop2MulF32:       out += ind + d + " = asuint(" + f0 + " * " + f1 + ");\n"; break;
            case kVop2MulI32I24:    out += ind + d + " = asuint((asint(" + u0 + " << 8u) >> 8) * (asint(" + u1 + " << 8u) >> 8));\n"; break;
            case kVop2MulU32U24:    out += ind + d + " = ((" + u0 + " & 0xFFFFFFu) * (" + u1 + " & 0xFFFFFFu));\n"; break;
            case kVop2MinF32:       out += ind + d + " = asuint(min(" + f0 + ", " + f1 + "));\n"; break;
            case kVop2MaxF32:       out += ind + d + " = asuint(max(" + f0 + ", " + f1 + "));\n"; break;
            case kVop2MinI32:       out += ind + d + " = asuint(min(asint(" + u0 + "), asint(" + u1 + ")));\n"; break;
            case kVop2MaxI32:       out += ind + d + " = asuint(max(asint(" + u0 + "), asint(" + u1 + ")));\n"; break;
            case kVop2MinU32:       out += ind + d + " = min(" + u0 + ", " + u1 + ");\n"; break;
            case kVop2MaxU32:       out += ind + d + " = max(" + u0 + ", " + u1 + ");\n"; break;
            case kVop2LshrrevB32:   out += ind + d + " = (" + u1 + " >> (" + u0 + " & 31u));\n"; break;
            case kVop2AshrrevI32:   out += ind + d + " = asuint(asint(" + u1 + ") >> (" + u0 + " & 31u));\n"; break;
            case kVop2LshlrevB32:   out += ind + d + " = (" + u1 + " << (" + u0 + " & 31u));\n"; break;
            case kVop2AndB32:       out += ind + d + " = (" + u0 + " & " + u1 + ");\n"; break;
            case kVop2OrB32:        out += ind + d + " = (" + u0 + " | " + u1 + ");\n"; break;
            case kVop2XorB32:       out += ind + d + " = (" + u0 + " ^ " + u1 + ");\n"; break;
            case kVop2AddNcU32:     out += ind + d + " = (" + u0 + " + " + u1 + ");\n"; break;
            case kVop2SubNcU32:     out += ind + d + " = (" + u0 + " - " + u1 + ");\n"; break;
            case kVop2SubrevNcU32:  out += ind + d + " = (" + u1 + " - " + u0 + ");\n"; break;
            case kVop2AddCoCiU32:   out += ind + "{ uint _r = (" + u0 + " + " + u1 + " + (_vcc & 1u)); _vcc = (_r < " + u0 + ") ? 1u : 0u; " + d + " = _r; }\n"; break;
            case kVop2FmacF32:      out += ind + d + " = asuint(mad(" + f0 + ", " + f1 + ", asfloat(" + d + ")));\n"; break;
            case kVop2FmamkF32:     out += ind + d + " = asuint(mad(" + f0 + ", " + litF + ", " + f1 + "));\n"; break;
            case kVop2FmaakF32:     out += ind + d + " = asuint(mad(" + f0 + ", " + f1 + ", " + litF + "));\n"; break;
            case kVop2AddF16:       out += ind + d + " = f32tof16(f16tof32(" + u0 + ") + f16tof32(" + u1 + "));\n"; break;
            case kVop2SubF16:       out += ind + d + " = f32tof16(f16tof32(" + u0 + ") - f16tof32(" + u1 + "));\n"; break;
            case kVop2MulF16:       out += ind + d + " = f32tof16(f16tof32(" + u0 + ") * f16tof32(" + u1 + "));\n"; break;
            case kVop2FmacF16:      out += ind + d + " = f32tof16(mad(f16tof32(" + u0 + "), f16tof32(" + u1 + "), f16tof32(" + d + ")));\n"; break;
            default:
                if (!opts.allowFallbackOps) { err = "unsupported VOP2 opcode"; return false; }
                out += ind + d + " = (" + u0 + " + " + u1 + ");\n"; break;
        }
        return true;
    }

    if (i.format == InstFormat::Vopc) {
        std::string u0, u1, f0, f1;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, u0, err) ||
            !FmtU(i.src1, false, 0, u1, err) ||
            !FmtF(i.src0, i.hasLiteral, i.literal32, false, false, f0, err) ||
            !FmtF(i.src1, false, 0, false, false, f1, err)) return false;
        bool writesExec = false;
        const std::string c = EmitCmpExpr(i.opcode, u0, u1, f0, f1, writesExec);
        if (writesExec) out += ind + "_exec = (" + c + " ? 1u : 0u);\n";
        else            out += ind + "_vcc = (" + c + " ? 1u : 0u);\n";
        return true;
    }

    if (i.format == InstFormat::Vop3) {
        std::string u0, u1, u2, f0, f1, f2;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, u0, err) ||
            !FmtU(i.src1, i.hasLiteral, i.literal32, u1, err) ||
            !FmtU(i.src2, i.hasLiteral, i.literal32, u2, err) ||
            !FmtF(i.src0, i.hasLiteral, i.literal32, (i.absMask & 1u) != 0, (i.negMask & 1u) != 0, f0, err) ||
            !FmtF(i.src1, i.hasLiteral, i.literal32, (i.absMask & 2u) != 0, (i.negMask & 2u) != 0, f1, err) ||
            !FmtF(i.src2, i.hasLiteral, i.literal32, (i.absMask & 4u) != 0, (i.negMask & 4u) != 0, f2, err)) {
            return false;
        }
        // Promoted VOPC (0x00..0xFF)
        if (i.opcode < 0x100u) {
            bool writesExec = false;
            const std::string c = EmitCmpExpr(i.opcode, u0, u1, f0, f1, writesExec);
            if (writesExec) out += ind + "_exec = (" + c + " ? 1u : 0u);\n";
            else            out += ind + SdstLhs(i.vdst) + " = (" + c + " ? 1u : 0u);\n";
            return true;
        }
        const std::string d = "v" + std::to_string(i.vdst);
        auto emitF = [&](std::string e) {
            if (i.clamp) e = "saturate(" + e + ")";
            out += ind + d + " = asuint(" + e + ");\n";
        };
        // Promoted VOP2 (0x100..0x13F)
        if (i.opcode >= 0x100u && i.opcode <= 0x13Fu) {
            const uint8_t sub = static_cast<uint8_t>(i.opcode - 0x100u);
            switch (sub) {
                case kVop2CndmaskB32:   out += ind + d + " = ((" + u2 + " & 1u) != 0u) ? " + u1 + " : " + u0 + ";\n"; return true;
                case kVop2AddF32:       emitF(f0 + " + " + f1); return true;
                case kVop2SubF32:       emitF(f0 + " - " + f1); return true;
                case kVop2SubRevF32:    emitF(f1 + " - " + f0); return true;
                case kVop2MulLegacyF32:
                case kVop2MulF32:       emitF(f0 + " * " + f1); return true;
                case kVop2MinF32:       emitF("min(" + f0 + ", " + f1 + ")"); return true;
                case kVop2MaxF32:       emitF("max(" + f0 + ", " + f1 + ")"); return true;
                case kVop2LshlrevB32:   out += ind + d + " = (" + u1 + " << (" + u0 + " & 31u));\n"; return true;
                case kVop2LshrrevB32:   out += ind + d + " = (" + u1 + " >> (" + u0 + " & 31u));\n"; return true;
                case kVop2AndB32:       out += ind + d + " = (" + u0 + " & " + u1 + ");\n"; return true;
                case kVop2OrB32:        out += ind + d + " = (" + u0 + " | " + u1 + ");\n"; return true;
                case kVop2XorB32:       out += ind + d + " = (" + u0 + " ^ " + u1 + ");\n"; return true;
                case kVop2AddNcU32:     out += ind + d + " = (" + u0 + " + " + u1 + ");\n"; return true;
                case kVop2SubNcU32:     out += ind + d + " = (" + u0 + " - " + u1 + ");\n"; return true;
                case kVop2AddCoCiU32:   out += ind + "{ uint _r = (" + u0 + " + " + u1 + " + (" + u2 + " & 1u)); " + SdstLhs(i.sdst) + " = (_r < " + u0 + ") ? 1u : 0u; " + d + " = _r; }\n"; return true;
                case kVop2FmacF32:      emitF("mad(" + f0 + ", " + f1 + ", asfloat(" + d + "))"); return true;
                default: break;
            }
        }
        // Promoted VOP1 (0x180..0x1FF)
        if (i.opcode >= 0x180u && i.opcode <= 0x1FFu) {
            const uint8_t sub = static_cast<uint8_t>(i.opcode - 0x180u);
            switch (sub) {
                case kVop1MovB32:  out += ind + d + " = " + u0 + ";\n"; return true;
                case kVop1ExpF32:  emitF("exp2(" + f0 + ")"); return true;
                case kVop1LogF32:  emitF("log2(" + f0 + ")"); return true;
                case kVop1RcpF32:  emitF("rcp(" + f0 + ")"); return true;
                case kVop1RsqF32:  emitF("rsqrt(" + f0 + ")"); return true;
                case kVop1SqrtF32: emitF("sqrt(" + f0 + ")"); return true;
                default: break;
            }
        }
        switch (i.opcode) {
            case kVop3MadF32: case kVop3FmaF32: emitF("mad(" + f0 + ", " + f1 + ", " + f2 + ")"); break;
            case 0x150u: case kVop3Min3F32:     emitF("min(" + f0 + ", min(" + f1 + ", " + f2 + "))"); break;
            case 0x151u + 3u:                   emitF("max(" + f0 + ", max(" + f1 + ", " + f2 + "))"); break; // 0x154
            case 0x152u: case kVop3Med3F32:     emitF("max(min(" + f0 + ", " + f1 + "), min(max(" + f0 + ", " + f1 + "), " + f2 + "))"); break;
            case kVop3MulLoU32:  out += ind + d + " = (" + u0 + " * " + u1 + ");\n"; break;
            case kVop3MulHiU32:  out += ind + "{ uint _a = " + u0 + ", _b = " + u1 + "; uint _a0 = _a & 0xFFFFu, _a1 = _a >> 16u, _b0 = _b & 0xFFFFu, _b1 = _b >> 16u; uint _m = ((_a0 * _b0) >> 16u) + ((_a0 * _b1) & 0xFFFFu) + ((_a1 * _b0) & 0xFFFFu); " + d + " = (_a1 * _b1) + ((_a0 * _b1) >> 16u) + ((_a1 * _b0) >> 16u) + (_m >> 16u); }\n"; break;
            case kVop3MulHiI32:  out += ind + "{ uint _a = " + u0 + ", _b = " + u1 + "; uint _a0 = _a & 0xFFFFu, _a1 = _a >> 16u, _b0 = _b & 0xFFFFu, _b1 = _b >> 16u; uint _m = ((_a0 * _b0) >> 16u) + ((_a0 * _b1) & 0xFFFFu) + ((_a1 * _b0) & 0xFFFFu); uint _hi = (_a1 * _b1) + ((_a0 * _b1) >> 16u) + ((_a1 * _b0) >> 16u) + (_m >> 16u); if (asint(_a) < 0) _hi -= _b; if (asint(_b) < 0) _hi -= _a; " + d + " = _hi; }\n"; break;
            case kVop3MadU64U32: out += ind + "{ uint _a = " + u0 + ", _b = " + u1 + "; uint _lo = _a * _b; uint _a0 = _a & 0xFFFFu, _a1 = _a >> 16u, _b0 = _b & 0xFFFFu, _b1 = _b >> 16u; uint _m = ((_a0 * _b0) >> 16u) + ((_a0 * _b1) & 0xFFFFu) + ((_a1 * _b0) & 0xFFFFu); uint _hi = (_a1 * _b1) + ((_a0 * _b1) >> 16u) + ((_a1 * _b0) >> 16u) + (_m >> 16u); uint _s = _lo + " + u2 + "; " + d + " = _s; v" + std::to_string(i.vdst + 1u) + " = _hi + ((_s < _lo) ? 1u : 0u); }\n"; break;
            case kVop3MadU32U24: out += ind + d + " = (((" + u0 + " & 0xFFFFFFu) * (" + u1 + " & 0xFFFFFFu)) + " + u2 + ");\n"; break;
            case kVop3BfeU32:    out += ind + "{ uint _o = (" + u1 + " & 31u); uint _w = (" + u2 + " & 31u); " + d + " = (_w == 0u) ? 0u : ((_w >= 32u) ? (" + u0 + " >> _o) : ((" + u0 + " >> _o) & ((1u << _w) - 1u))); }\n"; break;
            case kVop3BfeI32:    out += ind + "{ uint _o = (" + u1 + " & 31u); uint _w = (" + u2 + " & 31u); " + d + " = (_w == 0u) ? 0u : asuint(asint(" + u0 + " << (32u - min(32u, _o + _w))) >> (32u - min(32u, _w))); }\n"; break;
            case kVop3BfiB32:    out += ind + d + " = ((" + u0 + " & " + u1 + ") | ((~" + u0 + ") & " + u2 + "));\n"; break;
            case kVop3AlignbitB32: out += ind + "{ uint _s = (" + u2 + " & 31u); " + d + " = (_s == 0u) ? " + u1 + " : ((" + u0 + " << (32u - _s)) | (" + u1 + " >> _s)); }\n"; break;
            case kVop3CvtPkrtzF16F32: out += ind + d + " = ((f32tof16(" + f0 + ") & 0xFFFFu) | ((f32tof16(" + f1 + ") & 0xFFFFu) << 16u));\n"; break;
            case kVop3PackB32F16:     out += ind + d + " = ((" + u0 + " & 0xFFFFu) | ((" + u1 + " & 0xFFFFu) << 16u));\n"; break;
            case kVop3Xor3B32:   out += ind + d + " = (" + u0 + " ^ " + u1 + " ^ " + u2 + ");\n"; break;
            case kVop3AddCoU32:  out += ind + "{ uint _r = (" + u0 + " + " + u1 + "); " + SdstLhs(i.sdst) + " = (_r < " + u0 + ") ? 1u : 0u; " + d + " = _r; }\n"; break;
            case kVop3SubCoU32:  out += ind + "{ uint _r = (" + u0 + " - " + u1 + "); " + SdstLhs(i.sdst) + " = (" + u1 + " > " + u0 + ") ? 1u : 0u; " + d + " = _r; }\n"; break;
            case kVop3XadU32:    out += ind + d + " = ((" + u0 + " ^ " + u1 + ") + " + u2 + ");\n"; break;
            case kVop3LshlAddU32: out += ind + d + " = ((" + u0 + " << (" + u1 + " & 31u)) + " + u2 + ");\n"; break;
            case kVop3AddLshlU32: out += ind + d + " = ((" + u0 + " + " + u1 + ") << (" + u2 + " & 31u));\n"; break;
            case kVop3Add3U32:   out += ind + d + " = (" + u0 + " + " + u1 + " + " + u2 + ");\n"; break;
            case kVop3LshlOrB32: out += ind + d + " = ((" + u0 + " << (" + u1 + " & 31u)) | " + u2 + ");\n"; break;
            case kVop3AndOrB32:  out += ind + d + " = ((" + u0 + " & " + u1 + ") | " + u2 + ");\n"; break;
            case kVop3Or3B32:    out += ind + d + " = (" + u0 + " | " + u1 + " | " + u2 + ");\n"; break;
            case kVop3CubeidF32:
                out += ind + "{ float _ax = abs(" + f0 + "), _ay = abs(" + f1 + "), _az = abs(" + f2 + "); float _id = (_az >= _ax && _az >= _ay) ? ((" + f2 + " < 0.0f) ? 5.0f : 4.0f) : ((_ay >= _ax) ? ((" + f1 + " < 0.0f) ? 3.0f : 2.0f) : ((" + f0 + " < 0.0f) ? 1.0f : 0.0f)); " + d + " = asuint(_id); }\n"; break;
            case kVop3CubescF32:
                out += ind + "{ float _ax = abs(" + f0 + "), _ay = abs(" + f1 + "), _az = abs(" + f2 + "); float _sc = (_az >= _ax && _az >= _ay) ? ((" + f2 + " < 0.0f) ? (-(" + f0 + ")) : (" + f0 + ")) : ((_ay >= _ax) ? (" + f0 + ") : ((" + f0 + " < 0.0f) ? (" + f2 + ") : (-(" + f2 + ")))); " + d + " = asuint(_sc); }\n"; break;
            case kVop3CubetcF32:
                out += ind + "{ float _ax = abs(" + f0 + "), _ay = abs(" + f1 + "), _az = abs(" + f2 + "); float _tc = (_az >= _ax && _az >= _ay) ? (-(" + f1 + ")) : ((_ay >= _ax) ? ((" + f1 + " < 0.0f) ? (-(" + f2 + ")) : (" + f2 + ")) : (-(" + f1 + "))); " + d + " = asuint(_tc); }\n"; break;
            case kVop3CubemaF32:
                emitF("2.0f * max(abs(" + f0 + "), max(abs(" + f1 + "), abs(" + f2 + ")))"); break;
            case kVop3LshlrevB64: {
                const std::string hiSrc = (i.src1 >= kSrcVgprBase && i.src1 + 1u < kSrcVgprBase + 256u)
                    ? ("v" + std::to_string((i.src1 - kSrcVgprBase) + 1u)) : "0u";
                out += ind + "{ uint _sh = (" + u0 + " & 63u); uint _lo = " + u1 + "; uint _hi = " + hiSrc
                     + "; if (_sh == 0u) { " + d + " = _lo; v" + std::to_string(i.vdst + 1u) + " = _hi; }"
                     + " else if (_sh < 32u) { " + d + " = (_lo << _sh); v" + std::to_string(i.vdst + 1u) + " = ((_hi << _sh) | (_lo >> (32u - _sh))); }"
                     + " else { " + d + " = 0u; v" + std::to_string(i.vdst + 1u) + " = (_lo << (_sh - 32u)); } }\n";
                break;
            }
            case kVop3LshrrevB64: {
                const std::string hiSrc = (i.src1 >= kSrcVgprBase && i.src1 + 1u < kSrcVgprBase + 256u)
                    ? ("v" + std::to_string((i.src1 - kSrcVgprBase) + 1u)) : "0u";
                out += ind + "{ uint _sh = (" + u0 + " & 63u); uint _lo = " + u1 + "; uint _hi = " + hiSrc
                     + "; if (_sh == 0u) { " + d + " = _lo; v" + std::to_string(i.vdst + 1u) + " = _hi; }"
                     + " else if (_sh < 32u) { " + d + " = ((_lo >> _sh) | (_hi << (32u - _sh))); v" + std::to_string(i.vdst + 1u) + " = (_hi >> _sh); }"
                     + " else { " + d + " = (_hi >> (_sh - 32u)); v" + std::to_string(i.vdst + 1u) + " = 0u; } }\n";
                break;
            }
            case kVop3AshrrevI64: {
                const std::string hiSrc = (i.src1 >= kSrcVgprBase && i.src1 + 1u < kSrcVgprBase + 256u)
                    ? ("v" + std::to_string((i.src1 - kSrcVgprBase) + 1u)) : "0u";
                out += ind + "{ uint _sh = (" + u0 + " & 63u); uint _lo = " + u1 + "; uint _hi = " + hiSrc
                     + "; if (_sh == 0u) { " + d + " = _lo; v" + std::to_string(i.vdst + 1u) + " = _hi; }"
                     + " else if (_sh < 32u) { " + d + " = ((_lo >> _sh) | (_hi << (32u - _sh))); v" + std::to_string(i.vdst + 1u) + " = asuint(asint(_hi) >> _sh); }"
                     + " else { " + d + " = asuint(asint(_hi) >> (_sh - 32u)); v" + std::to_string(i.vdst + 1u) + " = asuint(asint(_hi) >> 31); } }\n";
                break;
            }
            case kVop3AddNcU16:
                out += ind + d + " = (((" + u0 + " & 0xFFFFu) + (" + u1 + " & 0xFFFFu)) & 0xFFFFu);\n"; break;
            case kVop3SubNcU16:
                out += ind + d + " = (((" + u0 + " & 0xFFFFu) - (" + u1 + " & 0xFFFFu)) & 0xFFFFu);\n"; break;
            case kVop3MulLoU16:
                out += ind + d + " = (((" + u0 + " & 0xFFFFu) * (" + u1 + " & 0xFFFFu)) & 0xFFFFu);\n"; break;
            case kVop3LshlrevB16:
                out += ind + d + " = (((" + u1 + " & 0xFFFFu) << (" + u0 + " & 15u)) & 0xFFFFu);\n"; break;
            case kVop3LshrrevB16:
                out += ind + d + " = (((" + u1 + " & 0xFFFFu) >> (" + u0 + " & 15u)) & 0xFFFFu);\n"; break;
            case kVop3AshrrevI16:
                out += ind + d + " = (asuint(((asint(" + u1 + ") << 16) >> 16) >> (" + u0 + " & 15u)) & 0xFFFFu);\n"; break;
            case kVop3MbcntLoU32B32:
                if (isCompute) {
                    out += ind + "{ uint _lane = (gtid.x & 31u); uint _mask = (_lane == 0u) ? 0u : ((1u << _lane) - 1u); "
                         + d + " = (countbits(" + u0 + " & _mask) + " + u1 + "); }\n";
                } else {
                    out += ind + d + " = " + u1 + ";\n";
                }
                break;
            case kVop3MbcntHiU32B32:
                if (isCompute) {
                    out += ind + "{ uint _hiLane = (gtid.x >= 32u) ? (gtid.x - 32u) : 0u; uint _mask = (gtid.x >= 32u && _hiLane > 0u) ? ((1u << _hiLane) - 1u) : 0u; "
                         + d + " = (countbits(" + u0 + " & _mask) + " + u1 + "); }\n";
                } else {
                    out += ind + d + " = " + u1 + ";\n";
                }
                break;
            case kVop3ReadlaneB32: out += ind + SdstLhs(i.vdst) + " = " + u0 + ";\n"; break;
            case kVop3WritelaneB32: out += ind + d + " = " + u0 + ";\n"; break;
            default:
                if (!opts.allowFallbackOps) {
                    char b[64];
                    std::snprintf(b, sizeof(b), "unsupported VOP3 opcode 0x%03X", i.opcode);
                    err = b;
                    return false;
                }
                out += ind + d + " = (" + u0 + " ^ " + u1 + " ^ " + u2 + ");\n"; break;
        }
        return true;
    }

    if (i.format == InstFormat::Vop3p) {
        std::string u0, u1, u2;
        if (!FmtU(i.src0, i.hasLiteral, i.literal32, u0, err) ||
            !FmtU(i.src1, i.hasLiteral, i.literal32, u1, err) ||
            !FmtU(i.src2, i.hasLiteral, i.literal32, u2, err)) return false;
        const std::string d = "v" + std::to_string(i.vdst);
        auto half = [](const std::string& x, bool hi) {
            return hi ? ("((" + x + " >> 16u) & 0xFFFFu)") : ("(" + x + " & 0xFFFFu)");
        };
        const std::string a0 = half(u0, (i.opSel & 1u) != 0), a1 = half(u0, (i.opSelHi & 1u) != 0);
        const std::string b0 = half(u1, (i.opSel & 2u) != 0), b1 = half(u1, (i.opSelHi & 2u) != 0);
        const std::string c0 = half(u2, (i.opSel & 4u) != 0), c1 = half(u2, (i.opSelHi & 4u) != 0);
        auto emitPkF16 = [&](const std::string& e0, const std::string& e1) {
            const std::string r0 = i.clamp ? ("saturate(" + e0 + ")") : e0;
            const std::string r1 = i.clamp ? ("saturate(" + e1 + ")") : e1;
            out += ind + d + " = ((f32tof16(" + r0 + ") & 0xFFFFu) | ((f32tof16(" + r1 + ") & 0xFFFFu) << 16u));\n";
        };
        switch (i.opcode) {
            case kVop3pPkAddF16: emitPkF16("f16tof32(" + a0 + ") + f16tof32(" + b0 + ")", "f16tof32(" + a1 + ") + f16tof32(" + b1 + ")"); break;
            case kVop3pPkMulF16: emitPkF16("f16tof32(" + a0 + ") * f16tof32(" + b0 + ")", "f16tof32(" + a1 + ") * f16tof32(" + b1 + ")"); break;
            case kVop3pPkFmaF16: emitPkF16("mad(f16tof32(" + a0 + "), f16tof32(" + b0 + "), f16tof32(" + c0 + "))", "mad(f16tof32(" + a1 + "), f16tof32(" + b1 + "), f16tof32(" + c1 + "))"); break;
            case kVop3pPkMinF16: emitPkF16("min(f16tof32(" + a0 + "), f16tof32(" + b0 + "))", "min(f16tof32(" + a1 + "), f16tof32(" + b1 + "))"); break;
            case kVop3pPkMaxF16: emitPkF16("max(f16tof32(" + a0 + "), f16tof32(" + b0 + "))", "max(f16tof32(" + a1 + "), f16tof32(" + b1 + "))"); break;
            case kVop3pPkAddI16: out += ind + d + " = (((" + a0 + " + " + b0 + ") & 0xFFFFu) | (((" + a1 + " + " + b1 + ") & 0xFFFFu) << 16u));\n"; break;
            case kVop3pPkSubI16: out += ind + d + " = (((" + a0 + " - " + b0 + ") & 0xFFFFu) | (((" + a1 + " - " + b1 + ") & 0xFFFFu) << 16u));\n"; break;
            case kVop3pPkMulLoU16: out += ind + d + " = (((" + a0 + " * " + b0 + ") & 0xFFFFu) | (((" + a1 + " * " + b1 + ") & 0xFFFFu) << 16u));\n"; break;
            case kVop3pPkMadI16: out += ind + d + " = ((((" + a0 + " * " + b0 + ") + " + c0 + ") & 0xFFFFu) | ((((" + a1 + " * " + b1 + ") + " + c1 + ") & 0xFFFFu) << 16u));\n"; break;
            default:
                if (!opts.allowFallbackOps) { err = "unsupported VOP3P opcode"; return false; }
                out += ind + d + " = " + u0 + ";\n"; break;
        }
        return true;
    }

    if (i.format == InstFormat::Mubuf || i.format == InstFormat::Mtbuf) {
        const std::string addr = i.mubufOffen
            ? ("(v" + std::to_string(i.mubufVaddr) + " + " + std::to_string(i.mubufOffset) + "u)")
            : (std::to_string(i.mubufOffset) + "u");
        auto emitLoad = [&](uint8_t n) {
            hasBufLoad = true;
            for (uint8_t k = 0; k < n; ++k) {
                const std::string vd = "v" + std::to_string(i.mubufVdata + k);
                if (opts.byteAddressedBuffers) {
                    out += ind + vd + " = gIn.Load((" + addr + ") + " + std::to_string(k * 4u) + "u);\n";
                } else {
                    out += ind + vd + " = gIn[(" + addr + ") + " + std::to_string(k) + "u];\n";
                }
            }
        };
        auto emitStore = [&](uint8_t n) {
            hasBufStore = true;
            for (uint8_t k = 0; k < n; ++k) {
                const std::string vd = "v" + std::to_string(i.mubufVdata + k);
                if (opts.byteAddressedBuffers) {
                    out += ind + "gOut.Store((" + addr + ") + " + std::to_string(k * 4u) + "u, " + vd + ");\n";
                } else {
                    out += ind + "gOut[(" + addr + ") + " + std::to_string(k) + "u] = " + vd + ";\n";
                }
            }
        };
        if (i.format == InstFormat::Mtbuf) { emitLoad(4u); return true; }
        switch (i.opcode) {
            case kMubufLoadDword: case kMubufLoadDwordLegacy: emitLoad(1u); break;
            case kMubufLoadDwordX2: emitLoad(2u); break;
            case kMubufLoadDwordX3: emitLoad(3u); break;
            case kMubufLoadDwordX4: emitLoad(4u); break;
            case kMubufStoreDword:   emitStore(1u); break;
            case kMubufStoreDwordX2: emitStore(2u); break;
            case kMubufStoreDwordX3: emitStore(3u); break;
            case kMubufStoreDwordX4: emitStore(4u); break;
            case kMubufAtomicAdd:
                hasBufStore = true;
                if (opts.byteAddressedBuffers) out += ind + "gOut.InterlockedAdd(" + addr + ", v" + std::to_string(i.mubufVdata) + ");\n";
                else out += ind + "InterlockedAdd(gOut[" + addr + "], v" + std::to_string(i.mubufVdata) + ");\n";
                break;
            default: break;
        }
        return true;
    }

    if (i.format == InstFormat::Ds && isCompute) {
        const std::string idx = "((v" + std::to_string(i.dsAddr) + " + " + std::to_string(i.dsOffset) + "u) >> 2u) & 4095u";
        if (i.opcode == 0x0Du) out += ind + "gLds[" + idx + "] = v" + std::to_string(i.dsData0) + ";\n";
        else if (i.opcode == 0x36u) out += ind + "v" + std::to_string(i.vdst) + " = gLds[" + idx + "];\n";
        else if (i.opcode == 0x00u || i.opcode == 0x20u) {
            out += ind + "{ uint _prev; InterlockedAdd(gLds[" + idx + "], v" + std::to_string(i.dsData0) + ", _prev); "
                 + ((i.opcode == 0x20u) ? ("v" + std::to_string(i.vdst) + " = _prev; ") : "") + "}\n";
        } else if (i.opcode == 0x4Du || i.opcode == 0xDEu || i.opcode == 0xDFu) {
            const uint8_t n = (i.opcode == 0x4Du) ? 2u : (i.opcode == 0xDEu ? 3u : 4u);
            for (uint8_t k = 0; k < n; ++k) {
                out += ind + "gLds[((" + idx + ") + " + std::to_string(k) + "u) & 4095u] = v" + std::to_string(i.dsData0 + k) + ";\n";
            }
        } else if (i.opcode == 0x76u || i.opcode == 0xFEu || i.opcode == 0xFFu) {
            const uint8_t n = (i.opcode == 0x76u) ? 2u : (i.opcode == 0xFEu ? 3u : 4u);
            for (uint8_t k = 0; k < n; ++k) {
                out += ind + "v" + std::to_string(i.vdst + k) + " = gLds[((" + idx + ") + " + std::to_string(k) + "u) & 4095u];\n";
            }
        } else if (i.opcode == 0x0Eu || i.opcode == 0x0Fu) {
            const uint32_t stride = (i.opcode == 0x0Fu) ? 64u : 1u;
            const uint32_t o0 = (i.dsOffset & 0xFFu) * stride;
            const uint32_t o1 = ((i.dsOffset >> 8u) & 0xFFu) * stride;
            out += ind + "gLds[((v" + std::to_string(i.dsAddr) + " >> 2u) + " + std::to_string(o0) + "u) & 4095u] = v" + std::to_string(i.dsData0) + ";\n";
            out += ind + "gLds[((v" + std::to_string(i.dsAddr) + " >> 2u) + " + std::to_string(o1) + "u) & 4095u] = v" + std::to_string(i.dsData1) + ";\n";
        } else if (i.opcode == 0x37u || i.opcode == 0x38u) {
            const uint32_t stride = (i.opcode == 0x38u) ? 64u : 1u;
            const uint32_t o0 = (i.dsOffset & 0xFFu) * stride;
            const uint32_t o1 = ((i.dsOffset >> 8u) & 0xFFu) * stride;
            out += ind + "v" + std::to_string(i.vdst) + " = gLds[((v" + std::to_string(i.dsAddr) + " >> 2u) + " + std::to_string(o0) + "u) & 4095u];\n";
            out += ind + "v" + std::to_string(i.vdst + 1u) + " = gLds[((v" + std::to_string(i.dsAddr) + " >> 2u) + " + std::to_string(o1) + "u) & 4095u];\n";
        }
        return true;
    }

    if (i.format == InstFormat::Mimg) {
        if (i.opcode == kMimgImageLoad) {
            out += ind + "{ float4 _s = gTex0.Load(int3(int(v" + std::to_string(i.mimgVaddr)
                 + "), int(v" + std::to_string(i.mimgVaddr + 1u) + "), 0)); ";
            uint8_t dstIdx = 0;
            for (int c = 0; c < 4; ++c) {
                if ((i.mimgDmask & (1u << c)) != 0u) {
                    out += "v" + std::to_string(i.mimgVdata + dstIdx) + " = asuint(_s." + kCh[c] + "); ";
                    ++dstIdx;
                }
            }
            out += "}\n";
        } else if (i.opcode == kMimgImageStore && isCompute) {
            out += ind + "gRwTex0[uint2(v" + std::to_string(i.mimgVaddr) + ", v" + std::to_string(i.mimgVaddr + 1u)
                 + ")] = float4(asfloat(v" + std::to_string(i.mimgVdata) + "), asfloat(v" + std::to_string(i.mimgVdata + 1u)
                 + "), asfloat(v" + std::to_string(i.mimgVdata + 2u) + "), asfloat(v" + std::to_string(i.mimgVdata + 3u) + "));\n";
        } else {
            const std::string call = isCompute
                ? ("gTex0.SampleLevel(gSamp0, float2(asfloat(v" + std::to_string(i.mimgVaddr) + "), asfloat(v" + std::to_string(i.mimgVaddr + 1u) + ")), 0.0f)")
                : ("gTex0.Sample(gSamp0, float2(asfloat(v" + std::to_string(i.mimgVaddr) + "), asfloat(v" + std::to_string(i.mimgVaddr + 1u) + ")))");
            out += ind + "{\n        float4 _s = " + call + ";\n";
            uint8_t dstIdx = 0;
            for (int c = 0; c < 4; ++c) {
                if ((i.mimgDmask & (1u << c)) != 0u) {
                    out += "        v" + std::to_string(i.mimgVdata + dstIdx) + " = asuint(_s." + kCh[c] + ");\n";
                    ++dstIdx;
                }
            }
            out += ind + "}\n";
        }
        return true;
    }

    if (i.format == InstFormat::Flat) {
        const std::string idx = "(v" + std::to_string(i.dsAddr) + " >> 2u) & 63u";
        if (i.opcode == 0x1Fu) {
            out += ind + "_scratch[" + idx + "] = v" + std::to_string(i.dsData0) + ";\n";
        } else {
            out += ind + "v" + std::to_string(i.vdst) + " = _scratch[" + idx + "];\n";
        }
        return true;
    }

    return true;
}

inline bool EmitBodyWithControlFlow(
    const DecodedProgram& prog, bool isCompute, bool isPs, bool isVs, const LiftOptions& opts,
    std::string& body, bool& hasExportOrStore, bool& hasBufLoad, bool& hasBufStore, std::string& err) {
    static constexpr char kCh[4] = { 'x', 'y', 'z', 'w' };

    auto emitOne = [&](const DecodedInst& i, std::string& dst) -> bool {
        if (isPs && i.format == InstFormat::Vintrp) {
            if (i.opcode == kVintrpP1F32) return true; // p2 performs the full interpolated write
            dst += "    v" + std::to_string(i.vdst) + " = asuint(input.param"
                 + std::to_string(i.vintrpAttr) + "." + kCh[i.vintrpChan & 3u] + ");\n";
            return true;
        }
        if (i.format == InstFormat::Exp) {
            std::string ch[4];
            if (i.expCompr) {
                ch[0] = ((i.expEn & 0x1u) != 0u) ? ("f16tof32(v" + std::to_string(i.expVsrc[0]) + " & 0xFFFFu)") : "0.0f";
                ch[1] = ((i.expEn & 0x2u) != 0u) ? ("f16tof32((v" + std::to_string(i.expVsrc[0]) + " >> 16u) & 0xFFFFu)") : "0.0f";
                ch[2] = ((i.expEn & 0x4u) != 0u) ? ("f16tof32(v" + std::to_string(i.expVsrc[1]) + " & 0xFFFFu)") : "0.0f";
                ch[3] = ((i.expEn & 0x8u) != 0u) ? ("f16tof32((v" + std::to_string(i.expVsrc[1]) + " >> 16u) & 0xFFFFu)") : "1.0f";
            } else {
                for (int c = 0; c < 4; ++c) {
                    ch[c] = ((i.expEn & (1u << c)) != 0u)
                          ? ("asfloat(v" + std::to_string(i.expVsrc[c]) + ")")
                          : ((c == 3) ? "1.0f" : "0.0f");
                }
            }
            if (isPs && i.expTarget == kExpTargetMrt0) {
                hasExportOrStore = true;
                dst += "    mrt0 = float4(" + ch[0] + ", " + ch[1] + ", " + ch[2] + ", " + ch[3] + ");\n";
            } else if (isVs && i.expTarget == kExpTargetPos0) {
                hasExportOrStore = true;
                dst += "    o.pos0 = float4(" + ch[0] + ", " + ch[1] + ", " + ch[2] + ", " + ch[3] + ");\n";
            } else if (isVs && i.expTarget >= kExpTargetParam0) {
                const uint8_t pIdx = static_cast<uint8_t>(i.expTarget - kExpTargetParam0);
                dst += "    o.param" + std::to_string(pIdx) + " = float4("
                     + ch[0] + ", " + ch[1] + ", " + ch[2] + ", " + ch[3] + ");\n";
            }
            return true;
        }
        return EmitCommonInst(i, isCompute, opts, dst, hasBufLoad, hasBufStore, err);
    };

    bool hasBranches = false;
    bool hasBarrier = false;
    for (const auto& i : prog.insts) {
        if (IsBranchOp(i)) hasBranches = true;
        if (i.format == InstFormat::Sopp && i.opcode == kSoppBarrier) hasBarrier = true;
    }

    if (!hasBranches) {
        for (const auto& i : prog.insts) {
            if (i.format == InstFormat::Sopp && i.opcode == kSoppEndpgm) break;
            if (!emitOne(i, body)) return false;
        }
        if (isCompute) hasExportOrStore = hasBufStore;
        return true;
    }

    // Build basic blocks keyed by instruction index
    std::map<uint32_t, size_t> wordToInst;
    for (size_t idx = 0; idx < prog.insts.size(); ++idx) {
        wordToInst[prog.insts[idx].wordOffset] = idx;
    }
    std::set<size_t> leaders{ 0u };
    for (size_t idx = 0; idx < prog.insts.size(); ++idx) {
        const auto& i = prog.insts[idx];
        if (IsBranchOp(i)) {
            const int32_t tgtW = static_cast<int32_t>(i.wordOffset + i.wordCount) + static_cast<int16_t>(i.simm16);
            auto it = wordToInst.find(static_cast<uint32_t>(tgtW));
            if (it != wordToInst.end()) leaders.insert(it->second);
            if (idx + 1u < prog.insts.size()) leaders.insert(idx + 1u);
        } else if (i.format == InstFormat::Sopp && i.opcode == kSoppEndpgm && idx + 1u < prog.insts.size()) {
            leaders.insert(idx + 1u);
        } else if (isCompute && hasBarrier && i.format == InstFormat::Sopp && i.opcode == kSoppBarrier && idx + 1u < prog.insts.size()) {
            leaders.insert(idx + 1u);
        }
    }

    std::vector<size_t> blocks(leaders.begin(), leaders.end());
    std::map<size_t, uint32_t> instToBlock;
    for (size_t b = 0; b < blocks.size(); ++b) instToBlock[blocks[b]] = static_cast<uint32_t>(b);

    const bool uniformSyncLoop = isCompute && hasBarrier;
    body += uniformSyncLoop
        ? "    uint _pc = 0u;\n    [loop] for (uint _step = 0u; _step < 64u; ++_step) {\n        uint _cur = _pc;\n        _pc = 0xFFFFFFFFu;\n"
        : "    uint _pc = 0u;\n    [loop] for (uint _step = 0u; _step < 1024u; ++_step) {\n        uint _cur = _pc;\n        _pc = 0xFFFFFFFFu;\n";
    for (size_t b = 0; b < blocks.size(); ++b) {
        const size_t start = blocks[b];
        const size_t end = (b + 1u < blocks.size()) ? blocks[b + 1u] : prog.insts.size();
        const uint32_t fallthrough = (b + 1u < blocks.size()) ? static_cast<uint32_t>(b + 1u) : 0xFFFFFFFFu;
        body += "        [branch] if (_cur == " + std::to_string(b) + "u) {\n";
        bool terminated = false;
        for (size_t idx = start; idx < end; ++idx) {
            const auto& i = prog.insts[idx];
            if (i.format == InstFormat::Sopp && i.opcode == kSoppEndpgm) {
                body += "            _pc = 0xFFFFFFFFu;\n";
                terminated = true;
                break;
            }
            if (IsBranchOp(i)) {
                const int32_t tgtW = static_cast<int32_t>(i.wordOffset + i.wordCount) + static_cast<int16_t>(i.simm16);
                uint32_t tgtBlk = 0xFFFFFFFFu;
                auto itW = wordToInst.find(static_cast<uint32_t>(tgtW));
                if (itW != wordToInst.end()) {
                    auto itB = instToBlock.find(itW->second);
                    if (itB != instToBlock.end()) tgtBlk = itB->second;
                }
                std::string cond;
                switch (i.opcode) {
                    case kSoppBranch:        cond = "true"; break;
                    case kSoppCbranchScc0:   cond = "(_scc == 0u)"; break;
                    case kSoppCbranchScc1:   cond = "(_scc != 0u)"; break;
                    case kSoppCbranchVccz:   cond = "(_vcc == 0u)"; break;
                    case kSoppCbranchVccnz:  cond = "(_vcc != 0u)"; break;
                    case kSoppCbranchExecz:  cond = "(_exec == 0u)"; break;
                    case kSoppCbranchExecnz: cond = "(_exec != 0u)"; break;
                    default: cond = "false"; break;
                }
                body += "            _pc = (" + cond + ") ? " + std::to_string(tgtBlk) + "u : " + std::to_string(fallthrough) + "u;\n";
                terminated = true;
                break;
            }
            if (uniformSyncLoop && i.format == InstFormat::Sopp && i.opcode == kSoppBarrier) {
                continue; // Synchronized uniformly at the bottom of the lockstep loop
            }
            if (!emitOne(i, body)) return false;
        }
        if (!terminated) body += "            _pc = " + std::to_string(fallthrough) + "u;\n";
        body += "        }\n";
    }
    body += uniformSyncLoop
        ? "        GroupMemoryBarrierWithGroupSync();\n    }\n"
        : "        if (_pc == 0xFFFFFFFFu) break;\n    }\n";
    if (isCompute) hasExportOrStore = hasBufStore;
    return true;
}

inline void EmitStateDecls(const UsageScan& u, const std::set<uint8_t>& skipVgprs, std::string& out) {
    out += "    uint _scc = 0u; uint _vcc = 0u; uint _exec = 1u; uint _m0 = 0u; uint _discard = 0u;\n";
    if (u.hasScratch) out += "    uint _scratch[64]; [unroll] for (uint _i = 0u; _i < 64u; ++_i) _scratch[_i] = 0u;\n";
    for (uint8_t v : u.vgprs) {
        if (skipVgprs.count(v)) continue;
        out += "    uint v" + std::to_string(v) + " = 0u;\n";
    }
    for (uint8_t s : u.sgprs) {
        if (u.usesGidX && s == 2u) continue;
        out += "    uint s" + std::to_string(s) + " = 0u;\n";
    }
}

} // namespace detail

inline bool LiftPixelShaderToHlsl(
    const DecodedProgram& prog, std::string& outHlsl, std::string& outError,
    const LiftOptions& opts = {}) {
    detail::UsageScan u{};
    detail::ScanProgram(prog, false, u);

    std::string body;
    bool hasMrt0 = false, hasBufLoad = false, hasBufStore = false;
    if (!detail::EmitBodyWithControlFlow(prog, false, true, false, opts, body, hasMrt0, hasBufLoad, hasBufStore, outError)) {
        return false;
    }
    if (!hasMrt0 && !opts.allowFallbackOps) {
        outError = "RDNA2 pixel shader contains no EXP MRT0 export";
        return false;
    }

    outHlsl.clear();
    if (u.hasSmem) {
        const uint32_t cbVecs = std::max<uint32_t>(16u, (u.maxCbvDwords + 3u) / 4u);
        outHlsl += "cbuffer Cb0 : register(b0) {\n    float4 gCb0[" + std::to_string(cbVecs) + "];\n};\n";
    }
    if (u.hasMimgSample || u.hasMimgLoad) outHlsl += "Texture2D<float4> gTex0 : register(t0);\n";
    if (u.hasMimgSample) outHlsl += "SamplerState gSamp0 : register(s0);\n";
    if (hasBufLoad) outHlsl += (opts.byteAddressedBuffers ? "ByteAddressBuffer gIn : register(t1);\n" : "StructuredBuffer<uint> gIn : register(t1);\n");
    if (u.interpParams.empty()) {
        outHlsl += "float4 PSMain(float4 svPos : SV_POSITION) : SV_TARGET {\n";
    } else {
        outHlsl += "struct PSIn {\n    float4 svPos : SV_POSITION;\n";
        for (uint8_t p : u.interpParams) {
            outHlsl += "    float4 param" + std::to_string(p) + " : TEXCOORD" + std::to_string(p) + ";\n";
        }
        outHlsl += "};\nfloat4 PSMain(PSIn input) : SV_TARGET {\n";
    }
    detail::EmitStateDecls(u, {}, outHlsl);
    outHlsl += "    float4 mrt0 = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
    outHlsl += body;
    outHlsl += "    return mrt0;\n}\n";
    return true;
}

inline bool LiftVertexShaderToHlsl(
    const DecodedProgram& prog, std::string& outHlsl, std::string& outError,
    const LiftOptions& opts = {}) {
    detail::UsageScan u{};
    detail::ScanProgram(prog, true, u);

    std::string body;
    bool hasPos0 = false, hasBufLoad = false, hasBufStore = false;
    if (!detail::EmitBodyWithControlFlow(prog, false, false, true, opts, body, hasPos0, hasBufLoad, hasBufStore, outError)) {
        return false;
    }
    if (!hasPos0 && !opts.allowFallbackOps) {
        outError = "RDNA2 vertex shader contains no EXP pos0 export";
        return false;
    }

    outHlsl.clear();
    if (u.hasSmem) {
        const uint32_t cbVecs = std::max<uint32_t>(16u, (u.maxCbvDwords + 3u) / 4u);
        outHlsl += "cbuffer Cb0 : register(b0) {\n    float4 gCb0[" + std::to_string(cbVecs) + "];\n};\n";
    }
    if (hasBufLoad) outHlsl += (opts.byteAddressedBuffers ? "ByteAddressBuffer gIn : register(t0);\n" : "StructuredBuffer<uint> gIn : register(t0);\n");
    outHlsl += "struct VSIn {\n    float4 inPos : POSITION;\n";
    if (u.usesVsColor) outHlsl += "    float4 inCol : COLOR;\n";
    outHlsl += "};\nstruct VSOut {\n    float4 pos0 : SV_POSITION;\n";
    for (uint8_t p : u.exportParams) {
        outHlsl += "    float4 param" + std::to_string(p) + " : TEXCOORD" + std::to_string(p) + ";\n";
    }
    outHlsl += "};\nVSOut VSMain(VSIn input) {\n";
    outHlsl += "    uint v0 = asuint(input.inPos.x); uint v1 = asuint(input.inPos.y);\n"
               "    uint v2 = asuint(input.inPos.z); uint v3 = asuint(input.inPos.w);\n";
    std::set<uint8_t> skip{ 0u, 1u, 2u, 3u };
    if (u.usesVsColor) {
        outHlsl += "    uint v4 = asuint(input.inCol.x); uint v5 = asuint(input.inCol.y);\n"
                   "    uint v6 = asuint(input.inCol.z); uint v7 = asuint(input.inCol.w);\n";
        skip.insert({ 4u, 5u, 6u, 7u });
    }
    detail::EmitStateDecls(u, skip, outHlsl);
    outHlsl += "    VSOut o = (VSOut)0;\n";
    outHlsl += body;
    outHlsl += "    return o;\n}\n";
    return true;
}

inline bool LiftComputeShaderToHlsl(
    const DecodedProgram& prog, std::string& outHlsl, std::string& outError,
    const LiftOptions& opts = {}) {
    detail::UsageScan u{};
    u.vgprs.insert(0u);
    detail::ScanProgram(prog, false, u);

    std::string body;
    bool hasStore = false, hasBufLoad = false, hasBufStore = false;
    if (!detail::EmitBodyWithControlFlow(prog, true, false, false, opts, body, hasStore, hasBufLoad, hasBufStore, outError)) {
        return false;
    }
    if (!hasStore && !u.hasMimgStore && !opts.allowFallbackOps) {
        outError = "RDNA2 compute shader contains no MUBUF buffer_store_dword";
        return false;
    }

    outHlsl.clear();
    if (u.hasSmem) {
        const uint32_t cbVecs = std::max<uint32_t>(16u, (u.maxCbvDwords + 3u) / 4u);
        outHlsl += "cbuffer Cb0 : register(b0) {\n    float4 gCb0[" + std::to_string(cbVecs) + "];\n};\n";
    }
    if (hasBufLoad) outHlsl += (opts.byteAddressedBuffers ? "ByteAddressBuffer gIn : register(t0);\n" : "StructuredBuffer<uint> gIn : register(t0);\n");
    if (u.hasMimgSample || u.hasMimgLoad) outHlsl += "Texture2D<float4> gTex0 : register(t1);\n";
    if (u.hasMimgSample) outHlsl += "SamplerState gSamp0 : register(s0);\n";
    if (hasBufStore || !u.hasMimgStore) {
        outHlsl += (opts.byteAddressedBuffers ? "RWByteAddressBuffer gOut : register(u0);\n" : "RWStructuredBuffer<uint> gOut : register(u0);\n");
    }
    if (u.hasMimgStore) outHlsl += "RWTexture2D<float4> gRwTex0 : register(u1);\n";
    if (u.hasLds) outHlsl += "groupshared uint gLds[4096];\n";
    outHlsl += "[numthreads(64, 1, 1)]\nvoid CSMain(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {\n";
    if (u.usesGidX) {
        outHlsl += "    uint v0 = gtid.x; uint s2 = gid.x;\n";
    } else {
        outHlsl += "    uint v0 = dtid.x;\n";
    }
    detail::EmitStateDecls(u, { 0u }, outHlsl);
    outHlsl += body;
    outHlsl += "}\n";
    return true;
}

} // namespace sc::d3d12::rdna2
