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
inline constexpr uint16_t kSrcSgprMax        = 105u; // s0..s105
inline constexpr uint16_t kSrcVccLo          = 106u;
inline constexpr uint16_t kSrcVccHi          = 107u;
inline constexpr uint16_t kSrcM0             = 124u;
inline constexpr uint16_t kSrcNull           = 125u;
inline constexpr uint16_t kSrcExecLo         = 126u;
inline constexpr uint16_t kSrcExecHi         = 127u;
inline constexpr uint16_t kSrcInlineZero     = 128u;
inline constexpr uint16_t kSrcInlineHalf     = 240u; //  0.5f
inline constexpr uint16_t kSrcInlineNegHalf  = 241u; // -0.5f
inline constexpr uint16_t kSrcInlineOne      = 242u; //  1.0f
inline constexpr uint16_t kSrcInlineNegOne   = 243u; // -1.0f
inline constexpr uint16_t kSrcInlineTwo      = 244u; //  2.0f
inline constexpr uint16_t kSrcInlineNegTwo   = 245u; // -2.0f
inline constexpr uint16_t kSrcInlineFour     = 246u; //  4.0f
inline constexpr uint16_t kSrcInlineNegFour  = 247u; // -4.0f
inline constexpr uint16_t kSrcInlineInv2Pi   = 248u; //  1/(2*pi)
inline constexpr uint16_t kSrcSdwa           = 249u;
inline constexpr uint16_t kSrcDpp16          = 250u;
inline constexpr uint16_t kSrcVccz           = 251u;
inline constexpr uint16_t kSrcExecz          = 252u;
inline constexpr uint16_t kSrcScc            = 253u;
inline constexpr uint16_t kSrcLiteral32      = 255u;
inline constexpr uint16_t kSrcVgprBase       = 256u;

inline constexpr uint16_t SgprSrc(uint8_t idx) { return static_cast<uint16_t>(idx); }
inline constexpr uint16_t VgprSrc(uint8_t idx) { return static_cast<uint16_t>(kSrcVgprBase + idx); }

inline constexpr uint8_t kExpTargetMrt0   = 0u;
inline constexpr uint8_t kExpTargetPos0   = 12u;
inline constexpr uint8_t kExpTargetParam0 = 32u;

// Real AMD RDNA2 (GFX10.3) opcode enums
enum SoppOp : uint8_t {
    kSoppNop           = 0x00u,
    kSoppEndpgm        = 0x01u,
    kSoppBranch        = 0x02u,
    kSoppCbranchScc0   = 0x04u,
    kSoppCbranchScc1   = 0x05u,
    kSoppCbranchVccz   = 0x06u,
    kSoppCbranchVccnz  = 0x07u,
    kSoppCbranchExecz  = 0x08u,
    kSoppCbranchExecnz = 0x09u,
    kSoppBarrier       = 0x0Au,
    kSoppWaitcnt       = 0x0Cu,
    kSoppSendmsg       = 0x10u,
    kSoppInstPrefetch  = 0x20u,
    kSoppClause        = 0x21u,
    kSoppDenormMode    = 0x25u
};

enum Sop1Op : uint8_t {
    kSop1MovB32            = 0x03u,
    kSop1MovB64            = 0x04u,
    kSop1CmovB32           = 0x05u,
    kSop1CmovB64           = 0x06u,
    kSop1NotB32            = 0x07u,
    kSop1NotB64            = 0x08u,
    kSop1WqmB32            = 0x09u,
    kSop1WqmB64            = 0x0Au,
    kSop1BrevB32           = 0x0Bu,
    kSop1BrevB64           = 0x0Cu,
    kSop1Bcnt0I32B32       = 0x0Du,
    kSop1Bcnt1I32B32       = 0x0Eu,
    kSop1Bcnt1I32B64       = 0x0Fu,
    kSop1Ff0I32B32         = 0x10u,
    kSop1Ff1I32B32         = 0x12u,
    kSop1Ff1I32B64         = 0x13u,
    kSop1FlbitI32B32       = 0x16u,
    kSop1SextI32I8         = 0x18u,
    kSop1SextI32I16        = 0x19u,
    kSop1Bitset1B32        = 0x1Au,
    kSop1Bitset0B32        = 0x1Bu,
    kSop1GetpcB64          = 0x1Fu,
    kSop1AndSaveexecB64    = 0x24u,
    kSop1OrSaveexecB64     = 0x25u,
    kSop1XorSaveexecB64    = 0x26u,
    kSop1Andn2SaveexecB64  = 0x27u,
    kSop1Orn2SaveexecB64   = 0x28u,
    kSop1NandSaveexecB64   = 0x29u,
    kSop1NorSaveexecB64    = 0x2Au,
    kSop1XnorSaveexecB64   = 0x2Bu,
    kSop1QuadmaskB32       = 0x2Cu,
    kSop1QuadmaskB64       = 0x2Du,
    kSop1AbsI32            = 0x34u,
    kSop1AndSaveexecB32    = 0x3Cu,
    kSop1OrSaveexecB32     = 0x3Du,
    kSop1XorSaveexecB32    = 0x3Eu,
    kSop1Andn2SaveexecB32  = 0x3Fu,
    kSop1Orn2SaveexecB32   = 0x40u,
    kSop1NandSaveexecB32   = 0x41u,
    kSop1NorSaveexecB32    = 0x42u,
    kSop1XnorSaveexecB32   = 0x43u,
    kSop1Andn1SaveexecB64  = 0x49u,
    kSop1Orn1SaveexecB64   = 0x4Au,
    kSop1Andn1SaveexecB32  = 0x4Du,
    kSop1Orn1SaveexecB32   = 0x4Eu
};

enum Sop2Op : uint8_t {
    kSop2AddU32     = 0x00u,
    kSop2SubU32     = 0x01u,
    kSop2AddI32     = 0x02u,
    kSop2SubI32     = 0x03u,
    kSop2AddcU32    = 0x04u,
    kSop2MinI32     = 0x06u,
    kSop2MinU32     = 0x07u,
    kSop2MaxI32     = 0x08u,
    kSop2MaxU32     = 0x09u,
    kSop2CselectB32 = 0x0Au,
    kSop2CselectB64 = 0x0Bu,
    kSop2AndB32     = 0x0Eu,
    kSop2AndB64     = 0x0Fu,
    kSop2OrB32      = 0x10u,
    kSop2OrB64      = 0x11u,
    kSop2XorB32     = 0x12u,
    kSop2XorB64     = 0x13u,
    kSop2Andn2B32   = 0x14u,
    kSop2Andn2B64   = 0x15u,
    kSop2Orn2B32    = 0x16u,
    kSop2Orn2B64    = 0x17u,
    kSop2LshlB32    = 0x1Eu,
    kSop2LshrB32    = 0x20u,
    kSop2AshrI32    = 0x22u,
    kSop2MulI32     = 0x26u,
    kSop2BfeU32     = 0x27u,
    kSop2BfeI32     = 0x28u,
    kSop2MulHiU32   = 0x35u
};

enum SopcOp : uint8_t {
    kSopcCmpEqI32   = 0x00u,
    kSopcCmpLgI32   = 0x01u,
    kSopcCmpGtI32   = 0x02u,
    kSopcCmpGeI32   = 0x03u,
    kSopcCmpLtI32   = 0x04u,
    kSopcCmpLeI32   = 0x05u,
    kSopcCmpEqU32   = 0x06u,
    kSopcCmpLgU32   = 0x07u,
    kSopcCmpGtU32   = 0x08u,
    kSopcCmpGeU32   = 0x09u,
    kSopcCmpLtU32   = 0x0Au,
    kSopcCmpLeU32   = 0x0Bu,
    kSopcBitcmp0B32 = 0x0Cu,
    kSopcBitcmp1B32 = 0x0Du
};

enum SopkOp : uint8_t {
    kSopkMovkI32       = 0x00u,
    kSopkCmovkI32      = 0x02u,
    kSopkCmpkEqI32     = 0x03u,
    kSopkCmpkLgI32     = 0x04u,
    kSopkCmpkGtI32     = 0x05u,
    kSopkCmpkGeI32     = 0x06u,
    kSopkCmpkLtI32     = 0x07u,
    kSopkCmpkLeI32     = 0x08u,
    kSopkCmpkEqU32     = 0x09u,
    kSopkCmpkLgU32     = 0x0Au,
    kSopkCmpkGtU32     = 0x0Bu,
    kSopkCmpkGeU32     = 0x0Cu,
    kSopkCmpkLtU32     = 0x0Du,
    kSopkCmpkLeU32     = 0x0Eu,
    kSopkAddkI32       = 0x0Fu,
    kSopkMulkI32       = 0x10u,
    kSopkSetregB32     = 0x13u,
    kSopkSetregImm32   = 0x15u,
    kSopkWaitcntVscnt  = 0x17u
};

enum Vop1Op : uint8_t {
    kVop1Nop           = 0x00u,
    kVop1MovB32        = 0x01u,
    kVop1Readfirstlane = 0x02u,
    kVop1CvtF32I32     = 0x05u,
    kVop1CvtF32U32     = 0x06u,
    kVop1CvtU32F32     = 0x07u,
    kVop1CvtI32F32     = 0x08u,
    kVop1CvtF16F32     = 0x0Au,
    kVop1CvtF32F16     = 0x0Bu,
    kVop1CvtF32Ubyte0  = 0x11u,
    kVop1FractF32      = 0x20u,
    kVop1TruncF32      = 0x21u,
    kVop1CeilF32       = 0x22u,
    kVop1RndneF32      = 0x23u,
    kVop1FloorF32      = 0x24u,
    kVop1ExpF32        = 0x25u,
    kVop1LogF32        = 0x27u,
    kVop1RcpF32        = 0x2Au,
    kVop1RcpIflagF32   = 0x2Bu,
    kVop1RsqF32        = 0x2Eu,
    kVop1SqrtF32       = 0x33u,
    kVop1SinF32        = 0x35u,
    kVop1CosF32        = 0x36u,
    kVop1NotB32        = 0x37u,
    kVop1BfrevB32      = 0x38u,
    kVop1FfblB32       = 0x3Au
};

enum Vop2Op : uint8_t {
    kVop2CndmaskB32   = 0x01u,
    kVop2AddF32       = 0x03u,
    kVop2SubF32       = 0x04u,
    kVop2SubRevF32    = 0x05u,
    kVop2MulLegacyF32 = 0x07u,
    kVop2MulF32       = 0x08u,
    kVop2MulI32I24    = 0x09u,
    kVop2MulU32U24    = 0x0Bu,
    kVop2MinF32       = 0x0Fu,
    kVop2MaxF32       = 0x10u,
    kVop2MinI32       = 0x11u,
    kVop2MaxI32       = 0x12u,
    kVop2MinU32       = 0x13u,
    kVop2MaxU32       = 0x14u,
    kVop2LshrrevB32   = 0x16u,
    kVop2AshrrevI32   = 0x18u,
    kVop2LshlrevB32   = 0x1Au,
    kVop2AndB32       = 0x1Bu,
    kVop2OrB32        = 0x1Cu,
    kVop2XorB32       = 0x1Du,
    kVop2AddNcU32     = 0x25u,
    kVop2SubNcU32     = 0x26u,
    kVop2SubrevNcU32  = 0x27u,
    kVop2AddCoCiU32   = 0x28u,
    kVop2FmacF32      = 0x2Bu,
    kVop2FmamkF32     = 0x2Cu,
    kVop2FmaakF32     = 0x2Du,
    kVop2AddF16       = 0x32u,
    kVop2SubF16       = 0x33u,
    kVop2MulF16       = 0x35u,
    kVop2FmacF16      = 0x36u,
    kVop2FmamkF16     = 0x37u,
    kVop2FmaakF16     = 0x38u
};

enum Vop3Op : uint16_t {
    kVop3MadF32          = 0x141u,
    kVop3MadU32U24       = 0x143u,
    kVop3CubeidF32       = 0x144u,
    kVop3CubescF32       = 0x145u,
    kVop3CubetcF32       = 0x146u,
    kVop3CubemaF32       = 0x147u,
    kVop3BfeU32          = 0x148u,
    kVop3BfeI32          = 0x149u,
    kVop3BfiB32          = 0x14Au,
    kVop3FmaF32          = 0x14Bu,
    kVop3AlignbitB32     = 0x14Eu,
    kVop3Min3F32         = 0x151u,
    kVop3Max3F32         = 0x154u,
    kVop3Med3F32         = 0x157u,
    kVop3CvtPkrtzF16F32  = 0x168u,
    kVop3MulLoU32        = 0x169u,
    kVop3MulHiU32        = 0x16Au,
    kVop3MulHiI32        = 0x16Cu,
    kVop3MadU64U32       = 0x176u,
    kVop3Xor3B32         = 0x178u,
    kVop3LshlrevB64      = 0x2FFu,
    kVop3LshrrevB64      = 0x300u,
    kVop3AshrrevI64      = 0x301u,
    kVop3AddNcU16        = 0x303u,
    kVop3SubNcU16        = 0x304u,
    kVop3MulLoU16        = 0x305u,
    kVop3AddCoU32        = 0x30Fu,
    kVop3SubCoU32        = 0x310u,
    kVop3PackB32F16      = 0x311u,
    kVop3LshlrevB16      = 0x314u,
    kVop3LshrrevB16      = 0x315u,
    kVop3AshrrevI16      = 0x316u,
    kVop3PermB32         = 0x344u,
    kVop3XadU32          = 0x345u,
    kVop3LshlAddU32      = 0x346u,
    kVop3AddLshlU32      = 0x347u,
    kVop3ReadlaneB32     = 0x360u,
    kVop3WritelaneB32    = 0x361u,
    kVop3MbcntLoU32B32   = 0x365u,
    kVop3MbcntHiU32B32   = 0x366u,
    kVop3Add3U32         = 0x36Du,
    kVop3LshlOrB32       = 0x36Fu,
    kVop3AndOrB32        = 0x371u,
    kVop3Or3B32          = 0x372u
};

enum Vop3pOp : uint8_t {
    kVop3pPkMadI16   = 0x00u,
    kVop3pPkMulLoU16 = 0x01u,
    kVop3pPkAddI16   = 0x02u,
    kVop3pPkSubI16   = 0x03u,
    kVop3pPkFmaF16   = 0x0Eu,
    kVop3pPkAddF16   = 0x0Fu,
    kVop3pPkMulF16   = 0x10u,
    kVop3pPkMinF16   = 0x11u,
    kVop3pPkMaxF16   = 0x12u
};

enum MubufOp : uint8_t {
    kMubufLoadDword        = 0x0Cu,
    kMubufLoadDwordX2      = 0x0Du,
    kMubufLoadDwordX4      = 0x0Eu,
    kMubufLoadDwordX3      = 0x0Fu,
    kMubufLoadDwordLegacy  = 0x14u,
    kMubufStoreDword       = 0x1Cu,
    kMubufStoreDwordX2     = 0x1Du,
    kMubufStoreDwordX4     = 0x1Eu,
    kMubufStoreDwordX3     = 0x1Fu,
    kMubufAtomicAdd        = 0x32u
};

enum SmemOp : uint8_t {
    kSmemLoadDword         = 0x00u,
    kSmemLoadDwordX2       = 0x01u,
    kSmemLoadDwordX4       = 0x02u,
    kSmemLoadDwordX8       = 0x03u,
    kSmemLoadDwordX16      = 0x04u,
    kSmemBufferLoadDword   = 0x08u,
    kSmemBufferLoadDwordX2 = 0x09u,
    kSmemBufferLoadDwordX4 = 0x0Au,
    kSmemBufferLoadDwordX8 = 0x0Bu
};

enum VintrpOp : uint8_t {
    kVintrpP1F32  = 0x00u,
    kVintrpP2F32  = 0x01u,
    kVintrpMovF32 = 0x02u
};

enum MimgOp : uint8_t {
    kMimgImageLoad    = 0x00u,
    kMimgImageStore   = 0x08u,
    kMimgImageSample  = 0x20u,
    kMimgImageSampleL = 0x24u,
    kMimgImageSampleLz = 0x27u,
    kMimgImageSampleLO = 0x34u
};

enum class InstFormat : uint8_t {
    Sopp, Sop1, Sop2, Sopc, Sopk,
    Vop1, Vop2, Vopc, Vop3, Vop3p,
    Exp, Mubuf, Mtbuf, Smem, Vintrp, Mimg, Ds, Flat
};

struct DecodedInst {
    InstFormat format{InstFormat::Sopp};
    uint16_t   opcode{0};
    uint32_t   wordOffset{0};
    uint8_t    wordCount{1};
    uint8_t    vdst{0};
    uint8_t    sdst{0};
    uint16_t   src0{0};
    uint16_t   src1{0};
    uint16_t   src2{0};
    uint32_t   literal32{0};
    bool       hasLiteral{false};
    uint8_t    absMask{0};
    uint8_t    negMask{0};
    uint8_t    opSel{0};
    uint8_t    opSelHi{0};
    bool       clamp{false};
    uint16_t   simm16{0};

    // EXP
    uint8_t    expTarget{0};
    uint8_t    expEn{0};
    uint8_t    expVsrc[4]{};
    bool       expDone{false};
    bool       expVm{false};
    bool       expCompr{false};

    // MUBUF / MTBUF
    uint8_t    mubufVaddr{0};
    uint8_t    mubufVdata{0};
    uint8_t    mubufSrsrc{0};
    uint16_t   mubufOffset{0};
    bool       mubufOffen{false};
    bool       mubufIdxen{false};

    // SMEM
    uint8_t    smemSdata{0};
    uint8_t    smemSbase{0};
    uint8_t    smemSoffset{0};
    uint32_t   smemOffset{0};

    // VINTRP
    uint8_t    vintrpAttr{0};
    uint8_t    vintrpChan{0};
    uint8_t    vintrpVsrc{0};

    // MIMG
    uint8_t    mimgDmask{0};
    uint8_t    mimgDim{0};
    uint8_t    mimgVaddr{0};
    uint8_t    mimgVdata{0};
    uint8_t    mimgSrsrc{0};
    uint8_t    mimgSsamp{0};

    // DS / FLAT
    uint8_t    dsAddr{0};
    uint8_t    dsData0{0};
    uint8_t    dsData1{0};
    uint16_t   dsOffset{0};
};

struct DecodedProgram {
    std::vector<DecodedInst> insts;
};

// ============================================================================
// Binary RDNA2 Encoders (Existing + Sprint 71 Extensions)
// ============================================================================
inline void AppendSopp(std::vector<uint32_t>& words, SoppOp op, uint16_t simm16 = 0) {
    words.push_back((0x17Fu << 23) | ((static_cast<uint32_t>(op) & 0x7Fu) << 16) | simm16);
}

inline void AppendSop1(std::vector<uint32_t>& words, Sop1Op op, uint8_t sdst, uint16_t src0) {
    words.push_back((0x17Du << 23) | ((static_cast<uint32_t>(sdst) & 0x7Fu) << 16)
                  | (static_cast<uint32_t>(op) << 8) | (src0 & 0xFFu));
}

inline void AppendSop1Literal(std::vector<uint32_t>& words, Sop1Op op, uint8_t sdst, uint32_t lit) {
    AppendSop1(words, op, sdst, kSrcLiteral32);
    words.push_back(lit);
}

inline void AppendSop2(std::vector<uint32_t>& words, Sop2Op op, uint8_t sdst, uint16_t src0, uint16_t src1) {
    words.push_back((0x2u << 30) | ((static_cast<uint32_t>(op) & 0x7Fu) << 23)
                  | ((static_cast<uint32_t>(sdst) & 0x7Fu) << 16)
                  | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu));
}

inline void AppendSopc(std::vector<uint32_t>& words, SopcOp op, uint16_t src0, uint16_t src1) {
    words.push_back((0x17Eu << 23) | ((static_cast<uint32_t>(op) & 0x7Fu) << 16)
                  | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu));
}

inline void AppendSopk(std::vector<uint32_t>& words, SopkOp op, uint8_t sdst, uint16_t simm16) {
    words.push_back((0xBu << 28) | ((static_cast<uint32_t>(op) & 0x1Fu) << 23)
                  | ((static_cast<uint32_t>(sdst) & 0x7Fu) << 16) | simm16);
}

inline void AppendVop1(std::vector<uint32_t>& words, Vop1Op op, uint8_t vdst, uint16_t src0) {
    words.push_back((0x3Fu << 25) | (static_cast<uint32_t>(vdst) << 17)
                  | (static_cast<uint32_t>(op) << 9) | (src0 & 0x1FFu));
}

inline void AppendVop1LiteralF32(std::vector<uint32_t>& words, Vop1Op op, uint8_t vdst, float imm) {
    AppendVop1(words, op, vdst, kSrcLiteral32);
    words.push_back(std::bit_cast<uint32_t>(imm));
}

inline void AppendVop1LiteralU32(std::vector<uint32_t>& words, Vop1Op op, uint8_t vdst, uint32_t imm) {
    AppendVop1(words, op, vdst, kSrcLiteral32);
    words.push_back(imm);
}

inline void AppendVop2(std::vector<uint32_t>& words, Vop2Op op, uint8_t vdst, uint16_t src0, uint8_t vsrc1) {
    words.push_back(((static_cast<uint32_t>(op) & 0x3Fu) << 25) | (static_cast<uint32_t>(vdst) << 17)
                  | (static_cast<uint32_t>(vsrc1) << 9) | (src0 & 0x1FFu));
}

inline void AppendVop2LiteralF32(std::vector<uint32_t>& words, Vop2Op op, uint8_t vdst, float imm0, uint8_t vsrc1) {
    AppendVop2(words, op, vdst, kSrcLiteral32, vsrc1);
    words.push_back(std::bit_cast<uint32_t>(imm0));
}

inline void AppendVop2LiteralU32(std::vector<uint32_t>& words, Vop2Op op, uint8_t vdst, uint32_t imm0, uint8_t vsrc1) {
    AppendVop2(words, op, vdst, kSrcLiteral32, vsrc1);
    words.push_back(imm0);
}

inline void AppendVopc(std::vector<uint32_t>& words, uint8_t op, uint16_t src0, uint8_t vsrc1) {
    words.push_back((0x3Eu << 25) | (static_cast<uint32_t>(op) << 17)
                  | (static_cast<uint32_t>(vsrc1) << 9) | (src0 & 0x1FFu));
}

inline void AppendVop3(
    std::vector<uint32_t>& words, Vop3Op op, uint8_t vdst,
    uint16_t src0, uint16_t src1, uint16_t src2,
    bool clamp = false, uint8_t absMask = 0, uint8_t negMask = 0) {
    const uint32_t w0 = (0x35u << 26) | ((static_cast<uint32_t>(op) & 0x3FFu) << 16)
                      | (clamp ? (1u << 15) : 0u) | ((static_cast<uint32_t>(absMask) & 0x7u) << 8)
                      | static_cast<uint32_t>(vdst);
    const uint32_t w1 = ((static_cast<uint32_t>(negMask) & 0x7u) << 29)
                      | ((static_cast<uint32_t>(src2) & 0x1FFu) << 18)
                      | ((static_cast<uint32_t>(src1) & 0x1FFu) << 9) | (src0 & 0x1FFu);
    words.push_back(w0);
    words.push_back(w1);
}

inline void AppendVop3p(
    std::vector<uint32_t>& words, Vop3pOp op, uint8_t vdst,
    uint16_t src0, uint16_t src1, uint16_t src2 = kSrcInlineZero,
    uint8_t opSel = 0, uint8_t opSelHi = 0x7u, bool clamp = false) {
    const uint32_t w0 = (0x33u << 26) | ((static_cast<uint32_t>(op) & 0x7Fu) << 16)
                      | (clamp ? (1u << 15) : 0u)
                      | ((static_cast<uint32_t>((opSelHi >> 2) & 0x1u)) << 14)
                      | ((static_cast<uint32_t>(opSel) & 0x7u) << 11)
                      | static_cast<uint32_t>(vdst);
    const uint32_t w1 = ((static_cast<uint32_t>(opSelHi & 0x3u)) << 27)
                      | ((static_cast<uint32_t>(src2) & 0x1FFu) << 18)
                      | ((static_cast<uint32_t>(src1) & 0x1FFu) << 9) | (src0 & 0x1FFu);
    words.push_back(w0);
    words.push_back(w1);
}

inline void AppendExp(
    std::vector<uint32_t>& words, uint8_t target,
    uint8_t vsrc0, uint8_t vsrc1, uint8_t vsrc2, uint8_t vsrc3,
    uint8_t enMask = 0xFu, bool done = true, bool vm = false, bool compr = false) {
    const uint32_t w0 = (0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u)
                      | (compr ? (1u << 10) : 0u) | ((static_cast<uint32_t>(target) & 0x3Fu) << 4)
                      | (static_cast<uint32_t>(enMask) & 0xFu);
    const uint32_t w1 = (static_cast<uint32_t>(vsrc3) << 24) | (static_cast<uint32_t>(vsrc2) << 16)
                      | (static_cast<uint32_t>(vsrc1) << 8) | static_cast<uint32_t>(vsrc0);
    words.push_back(w0);
    words.push_back(w1);
}

inline void AppendExpMrt0(
    std::vector<uint32_t>& words, uint8_t vsrc0, uint8_t vsrc1, uint8_t vsrc2, uint8_t vsrc3,
    uint8_t enMask = 0xFu, bool done = true, bool vm = true) {
    AppendExp(words, kExpTargetMrt0, vsrc0, vsrc1, vsrc2, vsrc3, enMask, done, vm, false);
}

inline void AppendMubufLoadDword(
    std::vector<uint32_t>& words, uint8_t vdata, uint8_t vaddr, uint8_t srsrc = 0, uint16_t offset = 0) {
    words.push_back((0x38u << 26) | (static_cast<uint32_t>(kMubufLoadDword) << 18)
                  | (1u << 12) | (static_cast<uint32_t>(offset) & 0xFFFu));
    words.push_back(((static_cast<uint32_t>(srsrc) & 0x1Fu) << 16)
                  | (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr));
}

inline void AppendMubufStoreDword(
    std::vector<uint32_t>& words, uint8_t vdata, uint8_t vaddr, uint8_t srsrc = 0, uint16_t offset = 0) {
    words.push_back((0x38u << 26) | (static_cast<uint32_t>(kMubufStoreDword) << 18)
                  | (1u << 12) | (static_cast<uint32_t>(offset) & 0xFFFu));
    words.push_back(((static_cast<uint32_t>(srsrc) & 0x1Fu) << 16)
                  | (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr));
}

inline void AppendSmemLoad(
    std::vector<uint32_t>& words, SmemOp op, uint8_t sdata, uint8_t sbasePair, uint32_t byteOffset) {
    words.push_back((0x3Du << 26) | (static_cast<uint32_t>(op) << 18)
                  | ((static_cast<uint32_t>(sdata) & 0x7Fu) << 6) | (static_cast<uint32_t>(sbasePair) & 0x3Fu));
    words.push_back((0x7Fu << 25) | (byteOffset & 0x1FFFFFu));
}

inline void AppendVintrpMov(
    std::vector<uint32_t>& words, uint8_t vdst, uint8_t attr, uint8_t chan, uint8_t vsrc = 0) {
    words.push_back((0x32u << 26) | (static_cast<uint32_t>(vdst) << 18)
                  | (static_cast<uint32_t>(kVintrpMovF32) << 16)
                  | ((static_cast<uint32_t>(attr) & 0x3Fu) << 10)
                  | ((static_cast<uint32_t>(chan) & 0x3u) << 8) | static_cast<uint32_t>(vsrc));
}

inline void AppendMubufOp(
    std::vector<uint32_t>& words, MubufOp op, uint8_t vdata, uint8_t vaddr,
    uint8_t srsrc = 0, uint16_t offset = 0, bool offen = true) {
    words.push_back((0x38u << 26) | (static_cast<uint32_t>(op) << 18)
                  | (offen ? (1u << 12) : 0u) | (static_cast<uint32_t>(offset) & 0xFFFu));
    words.push_back(((static_cast<uint32_t>(srsrc) & 0x1Fu) << 16)
                  | (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr));
}

inline void AppendDs(
    std::vector<uint32_t>& words, uint8_t op, uint8_t vdst, uint8_t addr,
    uint8_t data0 = 0, uint8_t data1 = 0, uint16_t offset = 0) {
    words.push_back((0x36u << 26) | (static_cast<uint32_t>(op) << 18) | offset);
    words.push_back((static_cast<uint32_t>(vdst) << 24) | (static_cast<uint32_t>(data1) << 16)
                  | (static_cast<uint32_t>(data0) << 8) | static_cast<uint32_t>(addr));
}

inline void AppendVop2FmaakF32(
    std::vector<uint32_t>& words, uint8_t vdst, uint16_t src0, uint8_t vsrc1, float kImm) {
    AppendVop2(words, kVop2FmaakF32, vdst, src0, vsrc1);
    words.push_back(std::bit_cast<uint32_t>(kImm));
}

inline void AppendVop2FmamkF32(
    std::vector<uint32_t>& words, uint8_t vdst, uint16_t src0, uint8_t vsrc1, float kImm) {
    AppendVop2(words, kVop2FmamkF32, vdst, src0, vsrc1);
    words.push_back(std::bit_cast<uint32_t>(kImm));
}

inline void AppendMimgImageSample(
    std::vector<uint32_t>& words, uint8_t vdata, uint8_t vaddr, uint8_t srsrc, uint8_t ssamp,
    uint8_t dmask = 0xFu, uint8_t dim = 1u) {
    words.push_back((0x3Cu << 26) | (static_cast<uint32_t>(kMimgImageSample) << 18)
                  | ((static_cast<uint32_t>(dmask) & 0xFu) << 8) | ((static_cast<uint32_t>(dim) & 0x7u) << 3));
    words.push_back(((static_cast<uint32_t>(ssamp) & 0x1Fu) << 21)
                  | ((static_cast<uint32_t>(srsrc) & 0x1Fu) << 16)
                  | (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr));
}

// ============================================================================
// Binary RDNA2 (GFX10.3) Decoder — All 14 Hardware Encodings
// ============================================================================
inline bool SrcNeedsExtraDword(uint16_t s) {
    return s == kSrcLiteral32 || s == kSrcSdwa || s == kSrcDpp16 || s == 233u || s == 234u;
}

inline uint8_t SmemDwordCount(uint16_t opcode) {
    switch (opcode) {
        case kSmemLoadDword:   case kSmemBufferLoadDword:   return 1u;
        case kSmemLoadDwordX2: case kSmemBufferLoadDwordX2: return 2u;
        case kSmemLoadDwordX4: case kSmemBufferLoadDwordX4: return 4u;
        case kSmemLoadDwordX8: case kSmemBufferLoadDwordX8: return 8u;
        case kSmemLoadDwordX16: return 16u;
        default: return 0u;
    }
}

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
    size_t maxBranchTarget = 0;
    while (pc < wordCount) {
        const size_t startPc = pc;
        const uint32_t w0 = words[pc++];
        DecodedInst inst{};
        inst.wordOffset = static_cast<uint32_t>(startPc);

        auto finish = [&]() {
            inst.wordCount = static_cast<uint8_t>(pc - startPc);
            outProg.insts.push_back(inst);
        };

        // 1. VOP1 / VOPC / VOP2 (bit 31 == 0)
        if ((w0 >> 31) == 0u) {
            const uint32_t tag = (w0 >> 25) & 0x3Fu;
            if (tag == 0x3Fu) {
                inst.format = InstFormat::Vop1;
                inst.vdst   = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
                inst.opcode = static_cast<uint16_t>((w0 >> 9) & 0xFFu);
                inst.src0   = static_cast<uint16_t>(w0 & 0x1FFu);
                if (SrcNeedsExtraDword(inst.src0)) {
                    if (pc >= wordCount) { outError = "truncated extra dword in VOP1"; return false; }
                    inst.literal32 = words[pc++];
                    inst.hasLiteral = (inst.src0 == kSrcLiteral32);
                }
                finish();
                continue;
            }
            if (tag == 0x3Eu) {
                inst.format = InstFormat::Vopc;
                inst.opcode = static_cast<uint16_t>((w0 >> 17) & 0xFFu);
                inst.src1   = VgprSrc(static_cast<uint8_t>((w0 >> 9) & 0xFFu));
                inst.src0   = static_cast<uint16_t>(w0 & 0x1FFu);
                if (SrcNeedsExtraDword(inst.src0)) {
                    if (pc >= wordCount) { outError = "truncated extra dword in VOPC"; return false; }
                    inst.literal32 = words[pc++];
                    inst.hasLiteral = (inst.src0 == kSrcLiteral32);
                }
                finish();
                continue;
            }
            inst.format = InstFormat::Vop2;
            inst.opcode = static_cast<uint16_t>(tag);
            inst.vdst   = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
            inst.src1   = VgprSrc(static_cast<uint8_t>((w0 >> 9) & 0xFFu));
            inst.src0   = static_cast<uint16_t>(w0 & 0x1FFu);
            const bool mandatoryLit = (inst.opcode == kVop2FmamkF32 || inst.opcode == kVop2FmaakF32 ||
                                       inst.opcode == kVop2FmamkF16 || inst.opcode == kVop2FmaakF16);
            if (mandatoryLit || SrcNeedsExtraDword(inst.src0)) {
                if (pc >= wordCount) { outError = "truncated extra dword in VOP2"; return false; }
                inst.literal32 = words[pc++];
                inst.hasLiteral = mandatoryLit || (inst.src0 == kSrcLiteral32);
            }
            finish();
            continue;
        }

        // 2. Scalar formats (must check SOP1/SOPC/SOPP/SOPK before SOP2!)
        if ((w0 >> 23) == 0x17Fu) {
            inst.format = InstFormat::Sopp;
            inst.opcode = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            inst.simm16 = static_cast<uint16_t>(w0 & 0xFFFFu);
            if (inst.opcode == 0x1Fu) break; // s_code_end ELF padding
            if (inst.opcode == kSoppBranch || inst.opcode == kSoppCbranchScc0 ||
                inst.opcode == kSoppCbranchScc1 || inst.opcode == kSoppCbranchVccz ||
                inst.opcode == kSoppCbranchVccnz || inst.opcode == kSoppCbranchExecz ||
                inst.opcode == kSoppCbranchExecnz) {
                const int32_t tgt = static_cast<int32_t>(pc) + static_cast<int16_t>(inst.simm16);
                if (tgt > static_cast<int32_t>(maxBranchTarget)) maxBranchTarget = static_cast<size_t>(tgt);
            }
            finish();
            if (inst.opcode == kSoppEndpgm && pc > maxBranchTarget) break;
            continue;
        }
        if ((w0 >> 23) == 0x17Du) {
            inst.format = InstFormat::Sop1;
            inst.sdst   = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            inst.opcode = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            inst.src0   = static_cast<uint16_t>(w0 & 0xFFu);
            if (inst.src0 == kSrcLiteral32) {
                if (pc >= wordCount) { outError = "truncated literal in SOP1"; return false; }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            finish();
            continue;
        }
        if ((w0 >> 23) == 0x17Eu) {
            inst.format = InstFormat::Sopc;
            inst.opcode = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            inst.src1   = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            inst.src0   = static_cast<uint16_t>(w0 & 0xFFu);
            if (inst.src0 == kSrcLiteral32 || inst.src1 == kSrcLiteral32) {
                if (pc >= wordCount) { outError = "truncated literal in SOPC"; return false; }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            finish();
            continue;
        }
        if ((w0 >> 28) == 0xBu) {
            inst.format = InstFormat::Sopk;
            inst.opcode = static_cast<uint16_t>((w0 >> 23) & 0x1Fu);
            inst.sdst   = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            inst.simm16 = static_cast<uint16_t>(w0 & 0xFFFFu);
            if (inst.opcode == kSopkSetregImm32) {
                if (pc >= wordCount) { outError = "truncated literal in SOPK"; return false; }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            finish();
            continue;
        }
        if ((w0 >> 30) == 0x2u) {
            inst.format = InstFormat::Sop2;
            inst.opcode = static_cast<uint16_t>((w0 >> 23) & 0x7Fu);
            inst.sdst   = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            inst.src1   = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            inst.src0   = static_cast<uint16_t>(w0 & 0xFFu);
            if (inst.src0 == kSrcLiteral32 || inst.src1 == kSrcLiteral32) {
                if (pc >= wordCount) { outError = "truncated literal in SOP2"; return false; }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            finish();
            continue;
        }

        // 3. 32/64/96-bit GFX10.3 formats (bits 31:26 == 11xxxx)
        const uint32_t top6 = w0 >> 26;
        if (top6 == 0x32u) {
            inst.format     = InstFormat::Vintrp;
            inst.vdst       = static_cast<uint8_t>((w0 >> 18) & 0xFFu);
            inst.opcode     = static_cast<uint16_t>((w0 >> 16) & 0x3u);
            inst.vintrpAttr = static_cast<uint8_t>((w0 >> 10) & 0x3Fu);
            inst.vintrpChan = static_cast<uint8_t>((w0 >> 8) & 0x3u);
            inst.vintrpVsrc = static_cast<uint8_t>(w0 & 0xFFu);
            finish();
            continue;
        }

        if (pc >= wordCount) {
            outError = "truncated 64-bit RDNA2 instruction";
            return false;
        }
        const uint32_t w1 = words[pc++];

        if (top6 == 0x35u) {
            inst.format  = InstFormat::Vop3;
            inst.opcode  = static_cast<uint16_t>((w0 >> 16) & 0x3FFu);
            inst.clamp   = ((w0 >> 15) & 1u) != 0;
            inst.absMask = static_cast<uint8_t>((w0 >> 8) & 0x7u);
            inst.sdst    = static_cast<uint8_t>((w0 >> 8) & 0x7Fu);
            inst.vdst    = static_cast<uint8_t>(w0 & 0xFFu);
            inst.negMask = static_cast<uint8_t>((w1 >> 29) & 0x7u);
            inst.src2    = static_cast<uint16_t>((w1 >> 18) & 0x1FFu);
            inst.src1    = static_cast<uint16_t>((w1 >> 9) & 0x1FFu);
            inst.src0    = static_cast<uint16_t>(w1 & 0x1FFu);
            if (inst.src0 == kSrcLiteral32 || inst.src1 == kSrcLiteral32 || inst.src2 == kSrcLiteral32) {
                if (pc >= wordCount) { outError = "truncated literal in VOP3"; return false; }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            finish();
            continue;
        }
        if (top6 == 0x33u) {
            inst.format  = InstFormat::Vop3p;
            inst.opcode  = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            inst.clamp   = ((w0 >> 15) & 1u) != 0;
            inst.opSel   = static_cast<uint8_t>((w0 >> 11) & 0x7u);
            inst.vdst    = static_cast<uint8_t>(w0 & 0xFFu);
            inst.negMask = static_cast<uint8_t>((w1 >> 29) & 0x7u);
            inst.opSelHi = static_cast<uint8_t>(((w1 >> 27) & 0x3u) | (((w0 >> 14) & 0x1u) << 2));
            inst.src2    = static_cast<uint16_t>((w1 >> 18) & 0x1FFu);
            inst.src1    = static_cast<uint16_t>((w1 >> 9) & 0x1FFu);
            inst.src0    = static_cast<uint16_t>(w1 & 0x1FFu);
            if (inst.src0 == kSrcLiteral32 || inst.src1 == kSrcLiteral32 || inst.src2 == kSrcLiteral32) {
                if (pc >= wordCount) { outError = "truncated literal in VOP3P"; return false; }
                inst.literal32 = words[pc++];
                inst.hasLiteral = true;
            }
            finish();
            continue;
        }
        if (top6 == 0x38u) {
            inst.format      = InstFormat::Mubuf;
            inst.opcode      = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
            inst.mubufIdxen  = ((w0 >> 13) & 1u) != 0;
            inst.mubufOffen  = ((w0 >> 12) & 1u) != 0;
            inst.mubufOffset = static_cast<uint16_t>(w0 & 0xFFFu);
            inst.mubufSrsrc  = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
            inst.mubufVdata  = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            inst.mubufVaddr  = static_cast<uint8_t>(w1 & 0xFFu);
            finish();
            continue;
        }
        if (top6 == 0x3Au) {
            inst.format      = InstFormat::Mtbuf;
            inst.opcode      = static_cast<uint16_t>((w0 >> 15) & 0xFu);
            inst.mubufIdxen  = ((w0 >> 13) & 1u) != 0;
            inst.mubufOffen  = ((w0 >> 12) & 1u) != 0;
            inst.mubufOffset = static_cast<uint16_t>(w0 & 0xFFFu);
            inst.mubufSrsrc  = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
            inst.mubufVdata  = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            inst.mubufVaddr  = static_cast<uint8_t>(w1 & 0xFFu);
            finish();
            continue;
        }
        if (top6 == 0x3Du) {
            inst.format      = InstFormat::Smem;
            inst.opcode      = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
            inst.smemSdata   = static_cast<uint8_t>((w0 >> 6) & 0x7Fu);
            inst.smemSbase   = static_cast<uint8_t>(w0 & 0x3Fu);
            inst.smemSoffset = static_cast<uint8_t>((w1 >> 25) & 0x7Fu);
            inst.smemOffset  = w1 & 0x1FFFFFu;
            finish();
            continue;
        }
        if (top6 == 0x3Cu) {
            const uint8_t nsize = static_cast<uint8_t>((w0 >> 1) & 0x3u);
            if (pc + nsize > wordCount) { outError = "truncated NSA MIMG instruction"; return false; }
            pc += nsize;
            inst.format    = InstFormat::Mimg;
            inst.opcode    = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
            inst.mimgDmask = static_cast<uint8_t>((w0 >> 8) & 0xFu);
            inst.mimgDim   = static_cast<uint8_t>((w0 >> 3) & 0x7u);
            inst.mimgVaddr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.mimgVdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            inst.mimgSrsrc = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
            inst.mimgSsamp = static_cast<uint8_t>((w1 >> 21) & 0x1Fu);
            finish();
            continue;
        }
        if (top6 == 0x36u) {
            inst.format   = InstFormat::Ds;
            inst.opcode   = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
            inst.dsOffset = static_cast<uint16_t>(w0 & 0xFFFFu);
            inst.dsAddr   = static_cast<uint8_t>(w1 & 0xFFu);
            inst.dsData0  = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            inst.dsData1  = static_cast<uint8_t>((w1 >> 16) & 0xFFu);
            inst.vdst     = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            finish();
            continue;
        }
        if (top6 == 0x37u) {
            inst.format   = InstFormat::Flat;
            inst.opcode   = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
            inst.dsOffset = static_cast<uint16_t>(w0 & 0xFFFu);
            inst.dsAddr   = static_cast<uint8_t>(w1 & 0xFFu);
            inst.dsData0  = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            inst.vdst     = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            finish();
            continue;
        }
        if (top6 == 0x3Eu) {
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
            finish();
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

inline std::string FormatExpTargetAsm(uint8_t target) {
    if (target < 8u) return "mrt" + std::to_string(target);
    if (target >= 12u && target <= 15u) return "pos" + std::to_string(target - 12u);
    if (target >= 32u) return "param" + std::to_string(target - 32u);
    return "tgt" + std::to_string(target);
}

inline std::string FormatOperandAsm(uint16_t src, bool hasLit, uint32_t lit) {
    if (src <= kSrcSgprMax) return "s" + std::to_string(src);
    if (src >= kSrcVgprBase && src < kSrcVgprBase + 256u) return "v" + std::to_string(src - kSrcVgprBase);
    if (src == kSrcLiteral32 && hasLit) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%08X", lit);
        return buf;
    }
    if (src >= 128u && src <= 192u) return std::to_string(static_cast<int>(src) - 128);
    if (src >= 193u && src <= 208u) return std::to_string(192 - static_cast<int>(src));
    if (src == kSrcVccLo) return "vcc_lo";
    if (src == kSrcExecLo) return "exec_lo";
    if (src == kSrcNull) return "null";
    switch (src) {
        case kSrcInlineHalf:    return "0.5";
        case kSrcInlineNegHalf: return "-0.5";
        case kSrcInlineOne:     return "1.0";
        case kSrcInlineNegOne:  return "-1.0";
        case kSrcInlineTwo:     return "2.0";
        case kSrcInlineNegTwo:  return "-2.0";
        case kSrcInlineFour:    return "4.0";
        case kSrcInlineNegFour: return "-4.0";
        case kSrcInlineInv2Pi:  return "0.15915494";
        default:                return "src_" + std::to_string(src);
    }
}

inline std::string Disassemble(const DecodedProgram& prog) {
    std::string out;
    char buf[160];
    for (const auto& i : prog.insts) {
        switch (i.format) {
            case InstFormat::Sopp:
                if (i.opcode == kSoppEndpgm) out += "s_endpgm\n";
                else if (i.opcode == kSoppNop) out += "s_nop\n";
                else if (i.opcode == kSoppWaitcnt) out += "s_waitcnt\n";
                else if (i.opcode == kSoppBarrier) out += "s_barrier\n";
                else if (i.opcode == kSoppBranch) { std::snprintf(buf, sizeof(buf), "s_branch %d\n", static_cast<int16_t>(i.simm16)); out += buf; }
                else if (i.opcode == kSoppCbranchScc0) { std::snprintf(buf, sizeof(buf), "s_cbranch_scc0 %d\n", static_cast<int16_t>(i.simm16)); out += buf; }
                else if (i.opcode == kSoppCbranchScc1) { std::snprintf(buf, sizeof(buf), "s_cbranch_scc1 %d\n", static_cast<int16_t>(i.simm16)); out += buf; }
                else if (i.opcode == kSoppCbranchExecz) { std::snprintf(buf, sizeof(buf), "s_cbranch_execz %d\n", static_cast<int16_t>(i.simm16)); out += buf; }
                else { std::snprintf(buf, sizeof(buf), "sopp_0x%02X %d\n", i.opcode, static_cast<int16_t>(i.simm16)); out += buf; }
                break;
            case InstFormat::Vop1: {
                const char* mn = "v_op1";
                switch (i.opcode) {
                    case kVop1Nop: mn = "v_nop"; break;
                    case kVop1MovB32: mn = "v_mov_b32"; break;
                    case kVop1RcpF32: mn = "v_rcp_f32"; break;
                    case kVop1RsqF32: mn = "v_rsq_f32"; break;
                    case kVop1SqrtF32: mn = "v_sqrt_f32"; break;
                    case kVop1ExpF32: mn = "v_exp_f32"; break;
                    case kVop1LogF32: mn = "v_log_f32"; break;
                    case kVop1SinF32: mn = "v_sin_f32"; break;
                    case kVop1CosF32: mn = "v_cos_f32"; break;
                    case kVop1CvtF16F32: mn = "v_cvt_f16_f32"; break;
                    case kVop1CvtF32F16: mn = "v_cvt_f32_f16"; break;
                    default: break;
                }
                out += std::string(mn) + " v" + std::to_string(i.vdst) + ", " + FormatOperandAsm(i.src0, i.hasLiteral, i.literal32) + "\n";
                break;
            }
            case InstFormat::Vop2: {
                const char* mn = "v_op2";
                switch (i.opcode) {
                    case kVop2CndmaskB32: mn = "v_cndmask_b32"; break;
                    case kVop2AddF32: mn = "v_add_f32"; break;
                    case kVop2SubF32: mn = "v_sub_f32"; break;
                    case kVop2SubRevF32: mn = "v_subrev_f32"; break;
                    case kVop2MulF32: mn = "v_mul_f32"; break;
                    case kVop2MinF32: mn = "v_min_f32"; break;
                    case kVop2MaxF32: mn = "v_max_f32"; break;
                    case kVop2LshlrevB32: mn = "v_lshlrev_b32"; break;
                    case kVop2LshrrevB32: mn = "v_lshrrev_b32"; break;
                    case kVop2AndB32: mn = "v_and_b32"; break;
                    case kVop2OrB32: mn = "v_or_b32"; break;
                    case kVop2XorB32: mn = "v_xor_b32"; break;
                    case kVop2AddNcU32: mn = "v_add_nc_u32"; break;
                    case kVop2FmacF32: mn = "v_fmac_f32"; break;
                    case kVop2FmaakF32: mn = "v_fmaak_f32"; break;
                    case kVop2FmamkF32: mn = "v_fmamk_f32"; break;
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
                    case kVop3MadF32: mn = "v_mad_f32"; break;
                    case kVop3FmaF32: mn = "v_fma_f32"; break;
                    case 0x150u: case kVop3Min3F32: mn = "v_min3_f32"; break;
                    case kVop3Max3F32: mn = "v_max3_f32"; break;
                    case 0x152u: case kVop3Med3F32: mn = "v_med3_f32"; break;
                    case kVop3MulLoU32: mn = "v_mul_lo_u32"; break;
                    case kVop3LshlAddU32: mn = "v_lshl_add_u32"; break;
                    case kVop3CvtPkrtzF16F32: mn = "v_cvt_pkrtz_f16_f32"; break;
                    default: break;
                }
                out += std::string(mn) + " v" + std::to_string(i.vdst) + ", "
                     + FormatOperandAsm(i.src0, i.hasLiteral, i.literal32) + ", "
                     + FormatOperandAsm(i.src1, i.hasLiteral, i.literal32) + ", "
                     + FormatOperandAsm(i.src2, i.hasLiteral, i.literal32)
                     + (i.clamp ? " clamp\n" : "\n");
                break;
            }
            case InstFormat::Exp:
                out += "exp " + FormatExpTargetAsm(i.expTarget) + ", v"
                     + std::to_string(i.expVsrc[0]) + ", v" + std::to_string(i.expVsrc[1])
                     + ", v" + std::to_string(i.expVsrc[2]) + ", v" + std::to_string(i.expVsrc[3])
                     + (i.expCompr ? " compr" : "") + (i.expDone ? " done" : "") + (i.expVm ? " vm\n" : "\n");
                break;
            case InstFormat::Mubuf:
                std::snprintf(buf, sizeof(buf), "%s v%u, v%u, s[%u]\n",
                    (i.opcode >= 0x1Cu && i.opcode <= 0x1Fu) ? "buffer_store_dword" : "buffer_load_dword",
                    i.mubufVdata, i.mubufVaddr, i.mubufSrsrc);
                out += buf;
                break;
            case InstFormat::Smem: {
                const uint8_t cnt = SmemDwordCount(i.opcode);
                const char* mn = (cnt == 16u) ? "s_load_dwordx16"
                               : (cnt == 8u)  ? "s_load_dwordx8"
                               : (cnt == 4u)  ? "s_load_dwordx4"
                               : (cnt == 2u)  ? "s_load_dwordx2"
                                              : "s_load_dword";
                std::snprintf(buf, sizeof(buf), "%s s%u, s[%u:%u], 0x%X\n",
                    mn, i.smemSdata, i.smemSbase * 2u, i.smemSbase * 2u + 1u, i.smemOffset);
                out += buf;
                break;
            }
            case InstFormat::Vintrp: {
                const char* mn = (i.opcode == kVintrpP1F32) ? "v_interp_p1_f32"
                               : (i.opcode == kVintrpP2F32) ? "v_interp_p2_f32"
                                                            : "v_interp_mov_f32";
                std::snprintf(buf, sizeof(buf), "%s v%u, attr%u.%c\n",
                    mn, i.vdst, i.vintrpAttr, "xyzw"[i.vintrpChan & 3u]);
                out += buf;
                break;
            }
            default:
                std::snprintf(buf, sizeof(buf), "fmt%u_op0x%03X d%u, %s, %s\n",
                    static_cast<unsigned>(i.format), i.opcode,
                    (i.format == InstFormat::Sop1 || i.format == InstFormat::Sop2 || i.format == InstFormat::Sopk) ? i.sdst : i.vdst,
                    FormatOperandAsm(i.src0, i.hasLiteral, i.literal32).c_str(),
                    FormatOperandAsm(i.src1, false, 0).c_str());
                out += buf;
                break;
        }
    }
    return out;
}

} // namespace sc::d3d12::rdna2

#include "rdna2_lifter.hpp"
