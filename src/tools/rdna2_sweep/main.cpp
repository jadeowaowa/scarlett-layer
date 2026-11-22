#include "shims/d3d12_xs/rdna2_isa.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool ExtractElfText(const std::vector<uint8_t>& data, std::vector<uint8_t>& outText) {
    if (data.size() < 64 || std::memcmp(data.data(), "\x7F""ELF", 4) != 0 || data[4] != 2) {
        outText = data;
        return !outText.empty();
    }
    uint64_t shoff = 0; uint16_t shentsize = 0, shnum = 0, shstrndx = 0;
    std::memcpy(&shoff, data.data() + 40, 8);
    std::memcpy(&shentsize, data.data() + 58, 2);
    std::memcpy(&shnum, data.data() + 60, 2);
    std::memcpy(&shstrndx, data.data() + 62, 2);
    if (shentsize < 64 || shstrndx >= shnum || shoff + uint64_t(shnum) * shentsize > data.size()) return false;

    uint64_t strOff = 0, strSize = 0;
    const size_t strHdr = static_cast<size_t>(shoff + uint64_t(shstrndx) * shentsize);
    std::memcpy(&strOff, data.data() + strHdr + 24, 8);
    std::memcpy(&strSize, data.data() + strHdr + 32, 8);
    if (strOff + strSize > data.size()) return false;

    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t h = static_cast<size_t>(shoff + uint64_t(i) * shentsize);
        uint32_t nameOff = 0; uint64_t off = 0, sz = 0;
        std::memcpy(&nameOff, data.data() + h, 4);
        std::memcpy(&off, data.data() + h + 24, 8);
        std::memcpy(&sz, data.data() + h + 32, 8);
        if (nameOff < strSize && off + sz <= data.size()) {
            const char* nm = reinterpret_cast<const char*>(data.data() + strOff + nameOff);
            if (std::strcmp(nm, ".text") == 0 && sz >= 4) {
                outText.assign(data.begin() + static_cast<ptrdiff_t>(off),
                               data.begin() + static_cast<ptrdiff_t>(off + sz));
                return true;
            }
        }
    }
    return false;
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n' || c == '\r') out.push_back(' ');
        else if (static_cast<unsigned char>(c) >= 0x20u) out.push_back(c);
        if (out.size() >= 180) break;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string stage = "cs", path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stage") == 0 && i + 1 < argc) stage = argv[++i];
        else path = argv[i];
    }
    if (path.empty()) return 2;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) { std::printf("{\"decode_ok\":false,\"error\":\"open failed\"}\n"); return 1; }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> text;
    if (!ExtractElfText(raw, text)) { std::printf("{\"decode_ok\":false,\"error\":\"no .text\"}\n"); return 1; }

    using namespace sc::d3d12::rdna2;
    DecodedProgram prog{};
    std::string decErr;
    if (!DecodeProgram(text.data(), text.size(), prog, decErr)) {
        std::printf("{\"decode_ok\":false,\"insts\":0,\"error\":\"%s\"}\n", EscapeJson(decErr).c_str());
        return 0;
    }

    auto tryLift = [&](bool allowFallback, bool& liftOk, bool& compOk, std::string& err) {
        LiftOptions opts{};
        opts.byteAddressedBuffers = (stage == "cs");
        opts.allowFallbackOps = allowFallback;
        std::string hlsl;
        const char* entry = (stage == "ps") ? "PSMain" : (stage == "vs") ? "VSMain" : "CSMain";
        const char* target = (stage == "ps") ? "ps_5_0" : (stage == "vs") ? "vs_5_0" : "cs_5_0";
        if (stage == "ps") liftOk = LiftPixelShaderToHlsl(prog, hlsl, err, opts);
        else if (stage == "vs") liftOk = LiftVertexShaderToHlsl(prog, hlsl, err, opts);
        else liftOk = LiftComputeShaderToHlsl(prog, hlsl, err, opts);
        if (!liftOk) { compOk = false; return; }
        Microsoft::WRL::ComPtr<ID3DBlob> blob, cErr;
        const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), "sweep.hlsl", nullptr, nullptr, entry, target, 0, 0, &blob, &cErr);
        compOk = SUCCEEDED(hr) && blob != nullptr;
        if (!compOk) err = cErr ? std::string(static_cast<const char*>(cErr->GetBufferPointer()), cErr->GetBufferSize()) : "D3DCompile failed";
    };

    bool sLift = false, sComp = false, cLift = false, cComp = false;
    std::string sErr, cErr;
    tryLift(false, sLift, sComp, sErr);
    tryLift(true, cLift, cComp, cErr);

    std::printf("{\"decode_ok\":true,\"insts\":%zu,\"strict_lift\":%s,\"strict_compile\":%s,\"compat_lift\":%s,\"compat_compile\":%s,\"strict_err\":\"%s\",\"compat_err\":\"%s\"}\n",
        prog.insts.size(), sLift ? "true" : "false", sComp ? "true" : "false",
        cLift ? "true" : "false", cComp ? "true" : "false",
        EscapeJson(sErr).c_str(), EscapeJson(cErr).c_str());
    return 0;
}
