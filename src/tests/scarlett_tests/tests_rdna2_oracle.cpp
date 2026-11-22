#include "rdna2_oracle_harness.hpp"

using namespace sc::d3d12;
using namespace sc::d3d12::rdna2;
using sc::test::oracle::ComputeOracle;

// Sprint 71 Differential Oracle Part 1: Cases 1..10 (ALU Edge Cases, Integer/Bitwise, CBV/SMEM)
bool TestRdna2OracleAluAndCbvDiffE2E() {
    ComputeOracle ctx;
    SC_EXPECT(ctx.Init());

    // Case 01: f32 add/subrev/mul/fmac + inline constants
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop2(w, kVop2MulF32, 2, kSrcInlineHalf, 1);
        AppendVop2(w, kVop2AddF32, 2, kSrcInlineOne, 2);
        AppendVop2(w, kVop2SubRevF32, 3, kSrcInlineFour, 2);
        AppendVop1(w, kVop1MovB32, 4, kSrcInlineTwo);
        AppendVop2(w, kVop2FmacF32, 4, VgprSrc(2), 3);
        AppendMubufStoreDword(w, 4, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  float v2 = 0.5f * float(id.x) + 1.0f; float v3 = v2 - 4.0f;\n"
            "  gOut[id.x] = asuint(mad(v2, v3, 2.0f));\n}\n";
        SC_EXPECT(ctx.Run("01_f32_add_sub_mul_fmac", hlsl, w, 0u, true, 1e-5f));
    }

    // Case 02: VOP3 fma_f32 with absMask, negMask, clamp
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop2LiteralF32(w, kVop2SubF32, 2, 32.0f, 1);
        AppendVop1LiteralF32(w, kVop1MovB32, 3, 0.05f);
        AppendVop3(w, kVop3FmaF32, 4, VgprSrc(2), VgprSrc(3), kSrcInlineHalf, true, 1u, 2u);
        AppendMubufStoreDword(w, 4, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  float x = 32.0f - float(id.x);\n"
            "  gOut[id.x] = asuint(saturate(mad(abs(x), -0.05f, 0.5f)));\n}\n";
        SC_EXPECT(ctx.Run("02_f32_fma_abs_neg_clamp", hlsl, w, 0u, true, 1e-5f));
    }

    // Case 03: Real GFX10.3 VOP3 min3/max3/med3 (0x151/0x154/0x157)
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop1LiteralF32(w, kVop1MovB32, 2, 10.0f);
        AppendVop1LiteralF32(w, kVop1MovB32, 3, 40.0f);
        AppendVop3(w, kVop3Med3F32, 4, VgprSrc(1), VgprSrc(2), VgprSrc(3));
        AppendVop3(w, kVop3Min3F32, 5, VgprSrc(1), VgprSrc(2), VgprSrc(3));
        AppendVop3(w, kVop3Max3F32, 6, VgprSrc(1), VgprSrc(2), VgprSrc(3));
        AppendVop2(w, kVop2AddF32, 4, VgprSrc(4), 5);
        AppendVop2(w, kVop2AddF32, 4, VgprSrc(4), 6);
        AppendMubufStoreDword(w, 4, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  float a = float(id.x), b = 10.0f, c = 40.0f;\n"
            "  float mn = min(a, min(b, c)), mx = max(a, max(b, c));\n"
            "  float md = max(min(a, b), min(max(a, b), c));\n"
            "  gOut[id.x] = asuint(md + mn + mx);\n}\n";
        SC_EXPECT(ctx.Run("03_f32_min3_max3_med3", hlsl, w, 0u, true, 1e-5f));
    }

    // Case 04: Transcendentals & rounding (sqrt, rcp, rsq, log2, exp2, frac, floor, ceil)
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop2(w, kVop2AddF32, 1, kSrcInlineOne, 1);
        AppendVop1(w, kVop1SqrtF32, 2, VgprSrc(1));
        AppendVop1(w, kVop1RcpF32, 3, VgprSrc(1));
        AppendVop1(w, kVop1RsqF32, 4, VgprSrc(1));
        AppendVop1(w, kVop1LogF32, 5, VgprSrc(1));
        AppendVop1(w, kVop1ExpF32, 6, VgprSrc(3));
        AppendVop1(w, kVop1FractF32, 7, VgprSrc(2));
        AppendVop1(w, kVop1FloorF32, 8, VgprSrc(2));
        AppendVop1(w, kVop1CeilF32, 9, VgprSrc(2));
        AppendVop2(w, kVop2AddF32, 2, VgprSrc(2), 3);
        AppendVop2(w, kVop2AddF32, 2, VgprSrc(2), 4);
        AppendVop2(w, kVop2AddF32, 2, VgprSrc(2), 5);
        AppendVop2(w, kVop2AddF32, 2, VgprSrc(2), 6);
        AppendVop2(w, kVop2AddF32, 2, VgprSrc(2), 7);
        AppendVop2(w, kVop2AddF32, 2, VgprSrc(2), 8);
        AppendVop2(w, kVop2AddF32, 2, VgprSrc(2), 9);
        AppendMubufStoreDword(w, 2, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  float x = float(id.x) + 1.0f; float sq = sqrt(x); float rc = rcp(x);\n"
            "  gOut[id.x] = asuint(sq + rc + rsqrt(x) + log2(x) + exp2(rc) + frac(sq) + floor(sq) + ceil(sq));\n}\n";
        SC_EXPECT(ctx.Run("04_f32_transcendentals_round", hlsl, w, 0u, true, 1e-3f));
    }

    // Case 05: Mandatory-literal VOP2 v_fmaak_f32 & v_fmamk_f32
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop2FmaakF32(w, 2, kSrcInlineHalf, 1, 0.75f);
        AppendVop2FmamkF32(w, 3, VgprSrc(2), 1, 1.25f);
        AppendMubufStoreDword(w, 3, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  float x = float(id.x); float v2 = mad(0.5f, x, 0.75f);\n"
            "  gOut[id.x] = asuint(mad(v2, 1.25f, x));\n}\n";
        SC_EXPECT(ctx.Run("05_f32_fmaak_fmamk_literals", hlsl, w, 0u, true, 1e-5f));
    }

    // Case 06: u32 mul_lo/mul_hi, shifts, bitwise and/or/xor/not
    {
        std::vector<uint32_t> w;
        AppendMubufLoadDword(w, 1, 0, 0);
        AppendVop3(w, kVop3MulLoU32, 2, VgprSrc(1), 128u + 37u, 128u);
        AppendVop3(w, kVop3MulHiU32, 3, VgprSrc(1), VgprSrc(2), 128u);
        AppendVop2(w, kVop2LshlrevB32, 4, 128u + 3u, 2);
        AppendVop2(w, kVop2LshrrevB32, 5, 128u + 5u, 3);
        AppendVop2(w, kVop2XorB32, 4, VgprSrc(4), 5);
        AppendVop1(w, kVop1NotB32, 5, VgprSrc(1));
        AppendVop2LiteralU32(w, kVop2AndB32, 5, 0x0F0F0F0Fu, 5);
        AppendVop2(w, kVop2OrB32, 4, VgprSrc(4), 5);
        AppendMubufStoreDword(w, 4, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "StructuredBuffer<uint> gIn : register(t0); RWStructuredBuffer<uint> gOut : register(u0);\n"
            "uint UHi(uint a, uint b) { uint a0=a&0xFFFFu,a1=a>>16u,b0=b&0xFFFFu,b1=b>>16u;\n"
            "  uint m=((a0*b0)>>16u)+((a0*b1)&0xFFFFu)+((a1*b0)&0xFFFFu);\n"
            "  return (a1*b1)+((a0*b1)>>16u)+((a1*b0)>>16u)+(m>>16u); }\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint x = gIn[id.x]; uint lo = x * 37u; uint hi = UHi(x, lo);\n"
            "  gOut[id.x] = ((lo << 3u) ^ (hi >> 5u)) | ((~x) & 0x0F0F0F0Fu);\n}\n";
        SC_EXPECT(ctx.Run("06_u32_mul_shift_bitwise", hlsl, w));
    }

    // Case 07: VOP3 compound ops (lshl_add, add_lshl, bfe, bfi, alignbit, xor3)
    {
        std::vector<uint32_t> w;
        AppendMubufLoadDword(w, 1, 0, 0);
        AppendVop3(w, kVop3LshlAddU32, 2, VgprSrc(0), 128u + 4u, VgprSrc(1));
        AppendVop3(w, kVop3AddLshlU32, 3, VgprSrc(1), 128u + 9u, 128u + 2u);
        AppendVop3(w, kVop3BfeU32, 4, VgprSrc(1), 128u + 4u, 128u + 12u);
        AppendVop3(w, kVop3BfiB32, 5, VgprSrc(2), VgprSrc(3), VgprSrc(4));
        AppendVop3(w, kVop3AlignbitB32, 6, VgprSrc(2), VgprSrc(3), 128u + 7u);
        AppendVop3(w, kVop3Xor3B32, 7, VgprSrc(5), VgprSrc(6), VgprSrc(4));
        AppendMubufStoreDword(w, 7, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "StructuredBuffer<uint> gIn : register(t0); RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint x = gIn[id.x]; uint v2 = (id.x << 4u) + x; uint v3 = (x + 9u) << 2u;\n"
            "  uint v4 = (x >> 4u) & 0xFFFu; uint v5 = (v2 & v3) | ((~v2) & v4);\n"
            "  uint v6 = (v2 << 25u) | (v3 >> 7u); gOut[id.x] = v5 ^ v6 ^ v4;\n}\n";
        SC_EXPECT(ctx.Run("07_u32_vop3_compound_bitops", hlsl, w));
    }

    // Case 08: Signed int32 arith, ashr, i24 mul, float<->int conversions
    {
        std::vector<uint32_t> w;
        AppendVop2LiteralU32(w, kVop2SubrevNcU32, 1, 32u, 0);
        AppendVop2(w, kVop2AshrrevI32, 2, 128u + 2u, 1);
        AppendVop2(w, kVop2MulI32I24, 3, 128u + 5u, 2);
        AppendVop2LiteralU32(w, kVop2MaxI32, 3, 0xFFFFFFF0u, 3);
        AppendVop1(w, kVop1CvtF32I32, 4, VgprSrc(3));
        AppendVop2(w, kVop2MulF32, 4, kSrcInlineHalf, 4);
        AppendVop1(w, kVop1CvtI32F32, 5, VgprSrc(4));
        AppendMubufStoreDword(w, 5, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  int s = int(id.x) - 32; int a = max(-16, (s >> 2) * 5);\n"
            "  gOut[id.x] = asuint(int(float(a) * 0.5f));\n}\n";
        SC_EXPECT(ctx.Run("08_i32_signed_arith_and_cvt", hlsl, w));
    }

    // Case 09: CBV SMEM s_load_dwordx4 + scalar SOP2 math feeding vector ALU
    {
        std::vector<uint32_t> w;
        AppendSmemLoad(w, kSmemLoadDwordX4, 4, 0, 0x40u);
        AppendSopp(w, kSoppWaitcnt, 0);
        AppendSop2(w, kSop2AddU32, 8, SgprSrc(5), SgprSrc(6));
        AppendSop2(w, kSop2MulI32, 9, SgprSrc(8), SgprSrc(7));
        AppendSop2(w, kSop2BfeU32, 10, SgprSrc(4), kSrcLiteral32);
        w.push_back((8u << 16) | 8u);
        AppendVop2(w, kVop2AddNcU32, 1, SgprSrc(9), 0);
        AppendVop2(w, kVop2XorB32, 1, SgprSrc(10), 1);
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "cbuffer Cb0 : register(b0) { float4 gCb0[16]; }; RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint s4 = asuint(gCb0[4].x), s5 = asuint(gCb0[4].y), s6 = asuint(gCb0[4].z), s7 = asuint(gCb0[4].w);\n"
            "  uint s9 = (s5 + s6) * s7; uint s10 = (s4 >> 8u) & 0xFFu;\n"
            "  gOut[id.x] = (id.x + s9) ^ s10;\n}\n";
        SC_EXPECT(ctx.Run("09_cbv_smem_scalar_math", hlsl, w));
    }

    // Case 10: SMEM s_load_dword / x2 / x4 across multiple CBV offsets
    {
        std::vector<uint32_t> w;
        AppendSmemLoad(w, kSmemLoadDword, 4, 0, 0x00u);
        AppendSmemLoad(w, kSmemLoadDwordX2, 5, 0, 0x10u);
        AppendSmemLoad(w, kSmemLoadDwordX4, 8, 0, 0x20u);
        AppendSopp(w, kSoppWaitcnt, 0);
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop2(w, kVop2MulF32, 1, SgprSrc(4), 1);
        AppendVop2(w, kVop2AddF32, 1, SgprSrc(5), 1);
        AppendVop2(w, kVop2AddF32, 1, SgprSrc(6), 1);
        AppendVop2(w, kVop2AddF32, 1, SgprSrc(11), 1);
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "cbuffer Cb0 : register(b0) { float4 gCb0[16]; }; RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  gOut[id.x] = asuint(float(id.x) * gCb0[0].x + gCb0[1].x + gCb0[1].y + gCb0[2].w);\n}\n";
        SC_EXPECT(ctx.Run("10_cbv_smem_multi_width", hlsl, w, 0u, true, 1e-5f));
    }

    return true;
}

// Sprint 71 Differential Oracle Part 2: Cases 11..20 (Scalar, SRV/UAV x4, FP16/VOP3P, LDS, Control Flow & Loops)
bool TestRdna2OracleFp16LdsAndCfDiffE2E() {
    ComputeOracle ctx;
    SC_EXPECT(ctx.Init());

    auto patchBranch = [](std::vector<uint32_t>& w, size_t brIdx, size_t tgtIdx) {
        const auto rel = static_cast<int16_t>(static_cast<int32_t>(tgtIdx) - static_cast<int32_t>(brIdx + 1u));
        w[brIdx] = (w[brIdx] & 0xFFFF0000u) | static_cast<uint16_t>(rel);
    };

    // Case 11: Scalar SOP1/SOPK/SOPC + s_cselect_b32 + s_bcnt1 + s_brev
    {
        std::vector<uint32_t> w;
        AppendSopk(w, kSopkMovkI32, 4, 15);
        AppendSopk(w, kSopkAddkI32, 4, 7);                     // s4 = 22
        AppendSop1(w, kSop1Bcnt1I32B32, 5, SgprSrc(4));        // s5 = popcnt(22) = 3
        AppendSop1(w, kSop1BrevB32, 6, SgprSrc(4));            // s6 = revbits(22)
        AppendSopc(w, kSopcCmpGtU32, SgprSrc(5), 128u + 2u);   // scc = (3 > 2) = 1
        AppendSop2(w, kSop2CselectB32, 7, SgprSrc(6), SgprSrc(4));
        AppendVop2(w, kVop2XorB32, 1, SgprSrc(7), 0);
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint s4 = 22u; uint s7 = (countbits(s4) > 2u) ? reversebits(s4) : s4;\n"
            "  gOut[id.x] = s7 ^ id.x;\n}\n";
        SC_EXPECT(ctx.Run("11_scalar_sop1_sopk_cselect", hlsl, w));
    }

    // Case 12: Multi-dword SRV/UAV (buffer_load_dwordx4 + buffer_store_dwordx4)
    {
        std::vector<uint32_t> w;
        AppendVop2(w, kVop2LshlrevB32, 1, 128u + 2u, 0);       // v1 = tid * 4
        AppendMubufOp(w, kMubufLoadDwordX4, 4, 1, 0);          // v4..v7 = gIn[v1..v1+3]
        AppendSopp(w, kSoppWaitcnt, 0);
        AppendVop2(w, kVop2XorB32, 4, VgprSrc(4), 5);
        AppendVop2(w, kVop2AddNcU32, 6, VgprSrc(6), 7);
        AppendMubufOp(w, kMubufStoreDwordX4, 4, 1, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "StructuredBuffer<uint> gIn : register(t0); RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint b = id.x * 4u; uint a0=gIn[b], a1=gIn[b+1], a2=gIn[b+2], a3=gIn[b+3];\n"
            "  gOut[b] = a0 ^ a1; gOut[b+1] = a1; gOut[b+2] = a2 + a3; gOut[b+3] = a3;\n}\n";
        SC_EXPECT(ctx.Run("12_mubuf_load_store_x4", hlsl, w, 0u, false, 0.0f, 256u));
    }

    // Case 13: FP16 scalar cvt + v_cvt_pkrtz_f16_f32 + v_pack_b32_f16
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop2(w, kVop2MulF32, 1, kSrcInlineHalf, 1);       // v1 = tid * 0.5
        AppendVop2(w, kVop2AddF32, 2, kSrcInlineOne, 1);        // v2 = v1 + 1.0
        AppendVop3(w, kVop3CvtPkrtzF16F32, 3, VgprSrc(1), VgprSrc(2), 128u);
        AppendVop1(w, kVop1CvtF16F32, 4, VgprSrc(2));
        AppendVop1(w, kVop1CvtF16F32, 5, VgprSrc(1));
        AppendVop3(w, kVop3PackB32F16, 6, VgprSrc(4), VgprSrc(5), 128u);
        AppendVop2(w, kVop2XorB32, 3, VgprSrc(3), 6);
        AppendMubufStoreDword(w, 3, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  float a = float(id.x) * 0.5f, b = a + 1.0f;\n"
            "  uint p0 = (f32tof16(a) & 0xFFFFu) | ((f32tof16(b) & 0xFFFFu) << 16u);\n"
            "  uint p1 = (f32tof16(b) & 0xFFFFu) | ((f32tof16(a) & 0xFFFFu) << 16u);\n"
            "  gOut[id.x] = p0 ^ p1;\n}\n";
        SC_EXPECT(ctx.Run("13_fp16_cvt_pkrtz_pack", hlsl, w));
    }

    // Case 14: VOP3P packed FP16 (v_pk_add_f16, v_pk_mul_f16, v_pk_fma_f16 clamp)
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1CvtF32U32, 1, VgprSrc(0));
        AppendVop1LiteralF32(w, kVop1MovB32, 2, 0.015625f);
        AppendVop2(w, kVop2MulF32, 1, VgprSrc(1), 2);           // tid / 64
        AppendVop2(w, kVop2SubF32, 2, kSrcInlineOne, 1);        // 1 - tid/64
        AppendVop3(w, kVop3CvtPkrtzF16F32, 3, VgprSrc(1), VgprSrc(2), 128u);
        AppendVop1LiteralU32(w, kVop1MovB32, 4, 0x3C003800u);   // {0.5h, 1.0h}
        AppendVop1LiteralU32(w, kVop1MovB32, 5, 0x34003400u);   // {0.25h, 0.25h}
        AppendVop3p(w, kVop3pPkAddF16, 6, VgprSrc(3), VgprSrc(4));
        AppendVop3p(w, kVop3pPkMulF16, 7, VgprSrc(6), VgprSrc(4));
        AppendVop3p(w, kVop3pPkFmaF16, 8, VgprSrc(7), VgprSrc(3), VgprSrc(5), 0u, 0x7u, true);
        AppendMubufStoreDword(w, 8, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  float a0 = f16tof32(f32tof16(float(id.x) * 0.015625f));\n"
            "  float a1 = f16tof32(f32tof16(1.0f - float(id.x) * 0.015625f));\n"
            "  float r0 = saturate(mad(f16tof32(f32tof16(f16tof32(f32tof16(a0 + 0.5f)) * 0.5f)), a0, 0.25f));\n"
            "  float r1 = saturate(mad(f16tof32(f32tof16(f16tof32(f32tof16(a1 + 1.0f)) * 1.0f)), a1, 0.25f));\n"
            "  gOut[id.x] = (f32tof16(r0) & 0xFFFFu) | ((f32tof16(r1) & 0xFFFFu) << 16u);\n}\n";
        SC_EXPECT(ctx.Run("14_vop3p_pk_add_mul_fma_f16", hlsl, w));
    }

    // Case 15: VOP3P packed INT16 (pk_add_i16, pk_sub_i16, pk_mul_lo_u16, pk_mad_i16)
    {
        std::vector<uint32_t> w;
        AppendMubufLoadDword(w, 1, 0, 0);
        AppendVop1LiteralU32(w, kVop1MovB32, 2, 0x00050003u);   // {3, 5}
        AppendVop1LiteralU32(w, kVop1MovB32, 3, 0x00110007u);   // {7, 17}
        AppendVop3p(w, kVop3pPkAddI16, 4, VgprSrc(1), VgprSrc(2));
        AppendVop3p(w, kVop3pPkSubI16, 5, VgprSrc(4), VgprSrc(3));
        AppendVop3p(w, kVop3pPkMadI16, 6, VgprSrc(5), VgprSrc(2), VgprSrc(3));
        AppendMubufStoreDword(w, 6, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "StructuredBuffer<uint> gIn : register(t0); RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint x = gIn[id.x]; uint lo = (x & 0xFFFFu), hi = (x >> 16u);\n"
            "  lo = ((((lo + 3u) - 7u) * 3u) + 7u) & 0xFFFFu;\n"
            "  hi = ((((hi + 5u) - 17u) * 5u) + 17u) & 0xFFFFu;\n"
            "  gOut[id.x] = lo | (hi << 16u);\n}\n";
        SC_EXPECT(ctx.Run("15_vop3p_pk_int16_arith", hlsl, w));
    }

    // Case 16: LDS ds_write_b32 + s_barrier + ds_read_b32 reverse-lane exchange
    {
        std::vector<uint32_t> w;
        AppendMubufLoadDword(w, 1, 0, 0);
        AppendVop2(w, kVop2LshlrevB32, 2, 128u + 2u, 0);        // addr = tid * 4
        AppendDs(w, 0x0Du, 0, 2, 1);                            // ds_write_b32
        AppendSopp(w, kSoppWaitcnt, 0);
        AppendSopp(w, kSoppBarrier, 0);
        AppendVop2LiteralU32(w, kVop2SubNcU32, 3, 63u, 0);      // 63 - tid
        AppendVop2(w, kVop2LshlrevB32, 3, 128u + 2u, 3);
        AppendDs(w, 0x36u, 4, 3);                               // ds_read_b32
        AppendSopp(w, kSoppWaitcnt, 0);
        AppendMubufStoreDword(w, 4, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "StructuredBuffer<uint> gIn : register(t0); RWStructuredBuffer<uint> gOut : register(u0);\n"
            "groupshared uint sMem[64];\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  sMem[id.x] = gIn[id.x]; GroupMemoryBarrierWithGroupSync(); gOut[id.x] = sMem[63u - id.x];\n}\n";
        SC_EXPECT(ctx.Run("16_lds_write_barrier_read_reverse", hlsl, w));
    }

    // Case 17: LDS ds_write2_b32 + ds_read2_b32 + ds_add_u32 atomic reduction
    {
        std::vector<uint32_t> w;
        AppendVop2(w, kVop2LshlrevB32, 1, 128u + 3u, 0);        // tid * 8 bytes
        AppendVop2(w, kVop2AddNcU32, 2, 128u + 10u, 0);         // tid + 10
        AppendVop2(w, kVop2AddNcU32, 3, 128u + 20u, 0);         // tid + 20
        AppendDs(w, 0x0Eu, 0, 1, 2, 3, 0x0100u);                // write offsets 0, 1 dwords
        AppendSopp(w, kSoppBarrier, 0);
        AppendDs(w, 0x37u, 4, 1, 0, 0, 0x0100u);                // read back into v4, v5
        AppendVop1LiteralU32(w, kVop1MovB32, 6, 1024u);         // slot 256 in LDS
        AppendVop1(w, kVop1MovB32, 7, 128u + 1u);
        AppendDs(w, 0x00u, 0, 6, 7);                            // ds_add_u32 (+1 * 64 lanes)
        AppendSopp(w, kSoppBarrier, 0);
        AppendDs(w, 0x36u, 8, 6);                               // read atomic sum = 64
        AppendVop2(w, kVop2AddNcU32, 4, VgprSrc(4), 5);
        AppendVop2(w, kVop2AddNcU32, 4, VgprSrc(4), 8);
        AppendMubufStoreDword(w, 4, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  gOut[id.x] = (id.x + 10u) + (id.x + 20u) + 64u;\n}\n";
        SC_EXPECT(ctx.Run("17_lds_write2_read2_atomic_add", hlsl, w));
    }

    // Case 18: VOPC float & uint comparisons + v_cndmask_b32 predicated select
    {
        std::vector<uint32_t> w;
        AppendVopc(w, 0xC1u, 128u + 31u, 0);                    // vcc = (31 < tid)
        AppendVop1LiteralU32(w, kVop1MovB32, 1, 0x11111111u);
        AppendVop1LiteralU32(w, kVop1MovB32, 2, 0x22222222u);
        AppendVop2(w, kVop2CndmaskB32, 3, VgprSrc(1), 2);
        AppendVop1(w, kVop1CvtF32U32, 4, VgprSrc(0));
        AppendVopc(w, 0x03u, VgprSrc(4), 4);                    // f32 le (always true)
        AppendVop2(w, kVop2AndB32, 5, 128u + 1u, 0);            // tid & 1
        AppendVopc(w, 0xC2u, 128u + 0u, 5);                     // vcc = (0 == (tid & 1))
        AppendVop2(w, kVop2CndmaskB32, 6, VgprSrc(0), 3);
        AppendMubufStoreDword(w, 6, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint v3 = (31u < id.x) ? 0x22222222u : 0x11111111u;\n"
            "  gOut[id.x] = ((id.x & 1u) == 0u) ? v3 : id.x;\n}\n";
        SC_EXPECT(ctx.Run("18_vopc_and_cndmask_select", hlsl, w));
    }

    // Case 19: Uniform scalar branch (s_cmp_eq_u32 + s_cbranch_scc1 + s_branch)
    {
        std::vector<uint32_t> w;
        AppendSmemLoad(w, kSmemLoadDword, 4, 0, 0x4Cu);         // s4 = cbU[19] = 3
        AppendSopc(w, kSopcCmpEqU32, SgprSrc(4), 128u + 3u);
        const size_t brTrue = w.size();
        AppendSopp(w, kSoppCbranchScc1, 0);
        AppendVop1LiteralU32(w, kVop1MovB32, 1, 0xBAD00000u);
        const size_t brEnd = w.size();
        AppendSopp(w, kSoppBranch, 0);
        const size_t lblTrue = w.size();
        AppendVop2LiteralU32(w, kVop2AddNcU32, 1, 0x600D0000u, 0);
        const size_t lblEnd = w.size();
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        patchBranch(w, brTrue, lblTrue);
        patchBranch(w, brEnd, lblEnd);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  gOut[id.x] = 0x600D0000u + id.x;\n}\n";
        SC_EXPECT(ctx.Run("19_uniform_scalar_if_else_branch", hlsl, w));
    }

    // Case 20: Counted scalar loop (s_movk_i32 + s_addk_i32 + s_cmpk_lt_i32 + backward s_cbranch_scc1)
    {
        std::vector<uint32_t> w;
        AppendVop1(w, kVop1MovB32, 1, 128u + 0u);               // acc = 0
        AppendSopk(w, kSopkMovkI32, 4, 0);                      // s4 = 0
        const size_t lblLoop = w.size();
        AppendVop2(w, kVop2AddNcU32, 1, VgprSrc(1), 0);         // acc += tid
        AppendVop2(w, kVop2AddNcU32, 1, SgprSrc(4), 1);         // acc += s4
        AppendSopk(w, kSopkAddkI32, 4, 1);                      // ++s4
        AppendSopk(w, kSopkCmpkLtI32, 4, 8);                    // scc = (s4 < 8)
        const size_t brLoop = w.size();
        AppendSopp(w, kSoppCbranchScc1, 0);
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        patchBranch(w, brLoop, lblLoop);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint acc = 0u; for (uint k = 0u; k < 8u; ++k) acc += id.x + k; gOut[id.x] = acc;\n}\n";
        SC_EXPECT(ctx.Run("20_counted_scalar_loop_backedge", hlsl, w));
    }

    return true;
}

// Sprint 71 Differential Oracle Part 3: Cases 21..25 (Wave32 Divergence, Early Exit, 64-bit Carry, Real RGA gfx1030 CS)
bool TestRdna2OracleControlFlowAndRealRgaCsDiffE2E() {
    ComputeOracle ctx;
    SC_EXPECT(ctx.Init());

    auto patchBranch = [](std::vector<uint32_t>& w, size_t brIdx, size_t tgtIdx) {
        const auto rel = static_cast<int16_t>(static_cast<int32_t>(tgtIdx) - static_cast<int32_t>(brIdx + 1u));
        w[brIdx] = (w[brIdx] & 0xFFFF0000u) | static_cast<uint16_t>(rel);
    };

    // Case 21: Real RDNA2 Wave32 divergent if/else (s_and_saveexec_b32 / s_cbranch_execz / s_andn2_saveexec_b32 / s_or_b32)
    {
        std::vector<uint32_t> w;
        AppendVopc(w, 0xC1u, 128u + 31u, 0);                        // vcc = (31 < tid)
        AppendSop1(w, kSop1AndSaveexecB32, 4, kSrcVccLo);           // s4 = exec; exec &= vcc
        const size_t brElse = w.size();
        AppendSopp(w, kSoppCbranchExecz, 0);
        AppendVop2LiteralU32(w, kVop2AddNcU32, 1, 1000u, 0);        // then: v1 = tid + 1000
        const size_t lblElse = w.size();
        AppendSop1(w, kSop1Andn2SaveexecB32, 4, SgprSrc(4));        // flip active mask for else
        const size_t brMerge = w.size();
        AppendSopp(w, kSoppCbranchExecz, 0);
        AppendVop3(w, kVop3MulLoU32, 1, VgprSrc(0), 128u + 7u, 128u);// else: v1 = tid * 7
        const size_t lblMerge = w.size();
        AppendSop2(w, kSop2OrB32, static_cast<uint8_t>(kSrcExecLo), kSrcExecLo, SgprSrc(4));
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        patchBranch(w, brElse, lblElse);
        patchBranch(w, brMerge, lblMerge);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  gOut[id.x] = (id.x > 31u) ? (id.x + 1000u) : (id.x * 7u);\n}\n";
        SC_EXPECT(ctx.Run("21_wave32_divergent_if_else", hlsl, w));
    }

    // Case 22: Early-exit branch to mid-stream s_endpgm
    {
        std::vector<uint32_t> w;
        AppendVopc(w, 0xC1u, 128u + 15u, 0);                        // vcc = (15 < tid)
        const size_t brHigh = w.size();
        AppendSopp(w, kSoppCbranchVccnz, 0);
        AppendVop2LiteralU32(w, kVop2XorB32, 1, 0xAAAA0000u, 0);
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);                                 // early return for tid <= 15
        const size_t lblHigh = w.size();
        AppendVop2LiteralU32(w, kVop2XorB32, 1, 0x55550000u, 0);
        AppendMubufStoreDword(w, 1, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        patchBranch(w, brHigh, lblHigh);
        const char* hlsl =
            "RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  if (id.x <= 15u) { gOut[id.x] = id.x ^ 0xAAAA0000u; return; }\n"
            "  gOut[id.x] = id.x ^ 0x55550000u;\n}\n";
        SC_EXPECT(ctx.Run("22_early_exit_mid_endpgm", hlsl, w));
    }

    // Case 23: 64-bit carry-chain addition (v_add_co_u32 + v_add_co_ci_u32)
    {
        std::vector<uint32_t> w;
        AppendMubufLoadDword(w, 1, 0, 0);                           // lo
        AppendVop1LiteralU32(w, kVop1MovB32, 2, 0x80000000u);       // addend lo
        // VOP3B v_add_co_u32 v3, vcc_lo, v1, v2 (sdst = 106 = kSrcVccLo)
        w.push_back((0x35u << 26) | (static_cast<uint32_t>(kVop3AddCoU32) << 16) | (106u << 8) | 3u);
        w.push_back((static_cast<uint32_t>(VgprSrc(2)) << 9) | VgprSrc(1));
        AppendVop2(w, kVop2AddCoCiU32, 4, 128u + 5u, 0);            // hi = 5 + tid + carry
        AppendVop2(w, kVop2XorB32, 3, VgprSrc(3), 4);
        AppendMubufStoreDword(w, 3, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "StructuredBuffer<uint> gIn : register(t0); RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint a = gIn[id.x]; uint lo = a + 0x80000000u; uint c = (lo < a) ? 1u : 0u;\n"
            "  gOut[id.x] = lo ^ (5u + id.x + c);\n}\n";
        SC_EXPECT(ctx.Run("23_u64_carry_add_co_ci", hlsl, w));
    }

    // Case 24: v_mad_u64_u32 (64-bit lo + hi product-accumulate) + v_add3_u32 + v_and_or_b32 + v_or3_b32
    {
        std::vector<uint32_t> w;
        AppendMubufLoadDword(w, 1, 0, 0);
        AppendVop3(w, kVop3MadU64U32, 2, VgprSrc(1), VgprSrc(1), VgprSrc(0)); // v[2:3] = v1*v1 + tid
        AppendVop3(w, kVop3Add3U32, 4, VgprSrc(2), VgprSrc(3), 128u + 13u);
        AppendVop3(w, kVop3AndOrB32, 5, VgprSrc(4), 128u + 63u, VgprSrc(3));
        AppendVop3(w, kVop3Or3B32, 6, VgprSrc(4), VgprSrc(5), VgprSrc(0));
        AppendMubufStoreDword(w, 6, 0, 0);
        AppendSopp(w, kSoppEndpgm);
        const char* hlsl =
            "StructuredBuffer<uint> gIn : register(t0); RWStructuredBuffer<uint> gOut : register(u0);\n"
            "uint UHi(uint a, uint b) { uint a0=a&0xFFFFu,a1=a>>16u,b0=b&0xFFFFu,b1=b>>16u;\n"
            "  uint m=((a0*b0)>>16u)+((a0*b1)&0xFFFFu)+((a1*b0)&0xFFFFu);\n"
            "  return (a1*b1)+((a0*b1)>>16u)+((a1*b0)>>16u)+(m>>16u); }\n"
            "[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) {\n"
            "  uint x = gIn[id.x]; uint pLo = x * x; uint pHi = UHi(x, x);\n"
            "  uint v2 = pLo + id.x; uint v3 = pHi + ((v2 < pLo) ? 1u : 0u);\n"
            "  uint v4 = v2 + v3 + 13u; uint v5 = (v4 & 63u) | v3; gOut[id.x] = v4 | v5 | id.x;\n}\n";
        SC_EXPECT(ctx.Run("24_vop3_mad_u64_and_triops", hlsl, w));
    }

    // Case 25: VERBATIM RGA 2.9.1 (gfx1030) machine words compiled from HLSL via dxc -spirv -> rga-rdna2!
    {
        const std::vector<uint32_t> rgaGfx1030Cs = {
            0xBE841F00u,             // s_getpc_b64 s[4:5]
            0xBE800301u,             // s_mov_b32 s0, s1
            0xBE810305u,             // s_mov_b32 s1, s5
            0xD7460002u, 0x04010C02u,// v_lshl_add_u32 v2, s2, 6, v0
            0xF4080100u, 0xFA000000u,// s_load_dwordx4 s[4:7], s[0:1], null
            0xD5767D00u, 0x021D0702u,// v_mad_u64_u32 v[0:1], null, v2, 3, 7
            0x34020482u,             // v_lshlrev_b32_e32 v1, 2, v2
            0xBF8CC07Fu,             // s_waitcnt lgkmcnt(0)
            0xE0701000u, 0x80010001u,// buffer_store_dword v0, v1, s[4:7], 0 offen
            0xBF810000u              // s_endpgm
        };
        const char* hlsl =
            "RWByteAddressBuffer gOut : register(u0);\n"
            "[numthreads(64,1,1)] void CSMain(uint3 dtid : SV_DispatchThreadID) {\n"
            "  gOut.Store(dtid.x * 4u, dtid.x * 3u + 7u);\n}\n";
        SC_EXPECT(ctx.Run("25_real_rga_gfx1030_compute_elf", hlsl, rgaGfx1030Cs, kScxsFlagByteAddressed));
    }

    return true;
}

// Sprint 71 Differential Oracle Part 4: Cases 26..28 (Real RGA gfx1030 PS, Compressed FP16 EXP, Full VS+PS CBV)
bool TestRdna2OracleGraphicsPipelineDiffE2E() {
    struct ScopedEvent {
        HANDLE h{nullptr};
        ~ScopedEvent() { if (h) CloseHandle(h); }
    };

    D3D12XBOX_CREATE_DEVICE_PARAMETERS devParams{}; devParams.Version = 1;
    Microsoft::WRL::ComPtr<ID3D12DeviceX> dev;
    SC_EXPECT(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &devParams, IID_ID3D12DeviceX, reinterpret_cast<void**>(dev.GetAddressOf()))));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueueX> queue;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandQueueX(&qd, &queue)));
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    SC_EXPECT(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    SC_EXPECT(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    ScopedEvent fEv{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    SC_EXPECT(fEv.h != nullptr);
    uint64_t fVal = 0;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHd{}; rtvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvHd.NumDescriptors = 2;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    SC_EXPECT(SUCCEEDED(dev->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&rtvHeap))));
    const UINT rtvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto makeRt = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& tex, UINT idx) -> bool {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 16; rd.Height = 16; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex)))) return false;
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * rtvStep;
        dev->CreateRenderTargetView(tex.Get(), nullptr, h);
        return true;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> rtO, rtL, rbO, rbL, vb, cbv;
    SC_EXPECT(makeRt(rtO, 0));
    SC_EXPECT(makeRt(rtL, 1));

    auto makeBuf = [&](uint64_t sz, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_STATES st, Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sz; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&out)));
    };
    SC_EXPECT(makeBuf(256 * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, rbO));
    SC_EXPECT(makeBuf(256 * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, rbL));
    SC_EXPECT(makeBuf(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, vb));
    SC_EXPECT(makeBuf(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, cbv));

    struct Vert { float pos[4]; float col[4]; };
    const Vert tri[3] = {
        { { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.2f, 0.4f, 0.8f, 1.0f } },
        { { -1.0f,  3.0f, 0.0f, 1.0f }, { 0.2f, 0.4f, 0.8f, 1.0f } },
        { {  3.0f, -1.0f, 0.0f, 1.0f }, { 0.2f, 0.4f, 0.8f, 1.0f } },
    };
    void* p = nullptr;
    SC_EXPECT(SUCCEEDED(vb->Map(0, nullptr, &p))); std::memcpy(p, tri, sizeof(tri)); vb->Unmap(0, nullptr);
    const float cbData[8] = { 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.75f, 0.25f, 1.0f };
    SC_EXPECT(SUCCEEDED(cbv->Map(0, nullptr, &p))); std::memcpy(p, cbData, sizeof(cbData)); cbv->Unmap(0, nullptr);

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> rsb, rse;
    SC_EXPECT(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse)));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
    SC_EXPECT(SUCCEEDED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs))));

    const char* passVsHlsl =
        "struct I { float4 p:POSITION; float4 c:COLOR; }; struct O { float4 p:SV_POSITION; float4 t:TEXCOORD0; };\n"
        "O VSMain(I i) { O o; o.p = i.p; o.t = i.c; return o; }\n";
    Microsoft::WRL::ComPtr<ID3DBlob> passVs;
    SC_EXPECT(SUCCEEDED(D3DCompile(passVsHlsl, std::strlen(passVsHlsl), "passVs", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &passVs, nullptr)));

    auto runGfx = [&](const char* tag, D3D12_SHADER_BYTECODE vsO, D3D12_SHADER_BYTECODE psO,
                      D3D12_SHADER_BYTECODE vsL, D3D12_SHADER_BYTECODE psL) -> bool {
        const D3D12_INPUT_ELEMENT_DESC il[2] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        auto mkPso = [&](D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{}; d.pRootSignature = rs.Get(); d.VS = vs; d.PS = ps;
            d.InputLayout = { il, 2 }; d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.SampleMask = UINT_MAX; d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            return SUCCEEDED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)));
        };
        Microsoft::WRL::ComPtr<ID3D12PipelineState> psoO, psoL;
        if (!mkPso(vsO, psO, psoO) || !mkPso(vsL, psL, psoL)) { std::printf("  [GFX PSO FAIL] %s\n", tag); return false; }

        if (FAILED(alloc->Reset())) return false;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd)))) return false;

        auto drawOne = [&](ID3D12PipelineState* pso, ID3D12Resource* rt, UINT rtvIdx, ID3D12Resource* rb) {
            D3D12_RESOURCE_BARRIER b0{}; b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b0.Transition = { rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET };
            cmd->ResourceBarrier(1, &b0);
            auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(rtvIdx) * rtvStep;
            cmd->OMSetRenderTargets(1, &h, FALSE, nullptr);
            D3D12_VIEWPORT vp{ 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f };
            D3D12_RECT sc{ 0, 0, 16, 16 };
            cmd->RSSetViewports(1, &vp); cmd->RSSetScissorRects(1, &sc);
            cmd->SetGraphicsRootSignature(rs.Get()); cmd->SetPipelineState(pso);
            cmd->SetGraphicsRootConstantBufferView(0, cbv->GetGPUVirtualAddress());
            D3D12_VERTEX_BUFFER_VIEW vbv{ vb->GetGPUVirtualAddress(), static_cast<UINT>(sizeof(tri)), static_cast<UINT>(sizeof(Vert)) };
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cmd->IASetVertexBuffers(0, 1, &vbv);
            cmd->DrawInstanced(3, 1, 0, 0);
            D3D12_RESOURCE_BARRIER b1{}; b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b1.Transition = { rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
            cmd->ResourceBarrier(1, &b1);
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 256 };
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER b2{}; b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b2.Transition = { rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON };
            cmd->ResourceBarrier(1, &b2);
        };
        drawOne(psoO.Get(), rtO.Get(), 0, rbO.Get());
        drawOne(psoL.Get(), rtL.Get(), 1, rbL.Get());
        if (FAILED(cmd->Close())) return false;
        ID3D12CommandList* lists[] = { cmd.Get() }; queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++fVal))) return false;
        if (fence->GetCompletedValue() < fVal) { fence->SetEventOnCompletion(fVal, fEv.h); WaitForSingleObject(fEv.h, 5000); }

        void *pO = nullptr, *pL = nullptr; D3D12_RANGE rr{ 0, 256 * 16 };
        if (FAILED(rbO->Map(0, &rr, &pO)) || FAILED(rbL->Map(0, &rr, &pL))) return false;
        const auto* bO = static_cast<const uint8_t*>(pO); const auto* bL = static_cast<const uint8_t*>(pL);
        bool ok = true;
        for (int y = 0; y < 16 && ok; ++y) {
            for (int x = 0; x < 16 * 4; ++x) {
                const int d = std::abs(static_cast<int>(bO[y * 256 + x]) - static_cast<int>(bL[y * 256 + x]));
                if (d > 1) { std::printf("  [GFX DIFF FAIL] %s (%d,%d) o=%u l=%u\n", tag, x, y, bO[y * 256 + x], bL[y * 256 + x]); ok = false; break; }
            }
        }
        D3D12_RANGE wr{ 0, 0 }; rbO->Unmap(0, &wr); rbL->Unmap(0, &wr);
        return ok;
    };

    // Case 26: VERBATIM RGA 2.9.1 (gfx1030) pixel shader (v_interp_p1/p2_f32 + v_fmaak_f32)
    {
        const std::vector<uint32_t> rgaPs = {
            0xBEFC0301u, 0xC8080000u, 0xC8000100u, 0xC8090001u, 0xC8010101u,
            0x5A0204F0u, 0x3E800000u, 0x7E0402F2u, 0xF800180Fu, 0x02020001u, 0xBF810000u
        };
        const auto scxsPs = BuildScxsContainer(ScxsShaderStage::Pixel, ScxsTargetArch::Rdna2Scarlett, ScxsPayloadFormat::Rdna2Isa, rgaPs.data(), rgaPs.size() * sizeof(uint32_t), 6, 6);
        const char* oPs = "float4 PSMain(float4 p:SV_POSITION, float4 uv:TEXCOORD0):SV_TARGET { return float4(uv.x*0.5f+0.25f, uv.y, 1.0f, 1.0f); }\n";
        Microsoft::WRL::ComPtr<ID3DBlob> oBlob;
        SC_EXPECT(SUCCEEDED(D3DCompile(oPs, std::strlen(oPs), "26_ps", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &oBlob, nullptr)));
        SC_EXPECT(runGfx("26_real_rga_gfx1030_ps", { passVs->GetBufferPointer(), passVs->GetBufferSize() },
                         { oBlob->GetBufferPointer(), oBlob->GetBufferSize() },
                         { passVs->GetBufferPointer(), passVs->GetBufferSize() }, { scxsPs.data(), scxsPs.size() }));
    }

    // Case 27: Compressed FP16 EXP MRT0 (v_cvt_pkrtz_f16_f32 + exp mrt0 compr)
    {
        std::vector<uint32_t> psW;
        AppendVintrpMov(psW, 0, 0, 0); AppendVintrpMov(psW, 1, 0, 1); AppendVintrpMov(psW, 2, 0, 2);
        AppendVop1(psW, kVop1MovB32, 3, kSrcInlineOne);
        AppendVop3(psW, kVop3CvtPkrtzF16F32, 4, VgprSrc(0), VgprSrc(1), 128u);
        AppendVop3(psW, kVop3CvtPkrtzF16F32, 5, VgprSrc(2), VgprSrc(3), 128u);
        AppendExp(psW, kExpTargetMrt0, 4, 5, 0, 0, 0xFu, true, true, true);
        AppendSopp(psW, kSoppEndpgm);
        const auto scxsPs = BuildScxsContainer(ScxsShaderStage::Pixel, ScxsTargetArch::Rdna2Scarlett, ScxsPayloadFormat::Rdna2Isa, psW.data(), psW.size() * sizeof(uint32_t), 6, 6);
        const char* oPs = "float4 PSMain(float4 p:SV_POSITION, float4 c:TEXCOORD0):SV_TARGET { return float4(c.x, c.y, c.z, 1.0f); }\n";
        Microsoft::WRL::ComPtr<ID3DBlob> oBlob;
        SC_EXPECT(SUCCEEDED(D3DCompile(oPs, std::strlen(oPs), "27_ps", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &oBlob, nullptr)));
        SC_EXPECT(runGfx("27_compressed_fp16_exp_mrt0", { passVs->GetBufferPointer(), passVs->GetBufferSize() },
                         { oBlob->GetBufferPointer(), oBlob->GetBufferSize() },
                         { passVs->GetBufferPointer(), passVs->GetBufferSize() }, { scxsPs.data(), scxsPs.size() }));
    }

    // Case 28: Full RDNA2 VS + PS pipeline with SMEM CBV tint
    {
        std::vector<uint32_t> vsW, psW;
        AppendExp(vsW, kExpTargetPos0, 0, 1, 2, 3, 0xFu, false, false);
        AppendExp(vsW, kExpTargetParam0, 4, 5, 6, 7, 0xFu, true, false);
        AppendSopp(vsW, kSoppEndpgm);
        AppendSmemLoad(psW, kSmemLoadDwordX4, 4, 0, 0x10u);
        AppendSopp(psW, kSoppWaitcnt, 0);
        AppendVintrpMov(psW, 0, 0, 0); AppendVintrpMov(psW, 1, 0, 1); AppendVintrpMov(psW, 2, 0, 2);
        AppendVop2(psW, kVop2MulF32, 0, SgprSrc(4), 0);
        AppendVop2(psW, kVop2AddF32, 1, SgprSrc(5), 1);
        AppendVop2(psW, kVop2MulF32, 2, SgprSrc(6), 2);
        AppendVop1(psW, kVop1MovB32, 3, SgprSrc(7));
        AppendExpMrt0(psW, 0, 1, 2, 3);
        AppendSopp(psW, kSoppEndpgm);
        const auto scxsVs = BuildScxsContainer(ScxsShaderStage::Vertex, ScxsTargetArch::Rdna2Scarlett, ScxsPayloadFormat::Rdna2Isa, vsW.data(), vsW.size() * sizeof(uint32_t), 6, 6);
        const auto scxsPs = BuildScxsContainer(ScxsShaderStage::Pixel, ScxsTargetArch::Rdna2Scarlett, ScxsPayloadFormat::Rdna2Isa, psW.data(), psW.size() * sizeof(uint32_t), 6, 6);
        const char* oPs = "cbuffer Cb0:register(b0){float4 gCb0[16];};\nfloat4 PSMain(float4 p:SV_POSITION, float4 c:TEXCOORD0):SV_TARGET { return float4(c.x*gCb0[1].x, c.y+gCb0[1].y, c.z*gCb0[1].z, gCb0[1].w); }\n";
        Microsoft::WRL::ComPtr<ID3DBlob> oBlob;
        SC_EXPECT(SUCCEEDED(D3DCompile(oPs, std::strlen(oPs), "28_ps", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &oBlob, nullptr)));
        SC_EXPECT(runGfx("28_full_rdna2_vs_ps_cbv_diff", { passVs->GetBufferPointer(), passVs->GetBufferSize() },
                         { oBlob->GetBufferPointer(), oBlob->GetBufferSize() },
                         { scxsVs.data(), scxsVs.size() }, { scxsPs.data(), scxsPs.size() }));
    }

    return true;
}
