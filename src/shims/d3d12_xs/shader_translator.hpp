#pragma once

#include "rdna2_isa.hpp"
#include "shader_inspector.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace sc::d3d12 {

enum class ScxsShaderStage : uint16_t {
    Pixel         = 0,
    Vertex        = 1,
    Geometry      = 2,
    Hull          = 3,
    Domain        = 4,
    Compute       = 5,
    Mesh          = 6,
    Amplification = 7
};

enum class ScxsTargetArch : uint32_t {
    Unknown       = 0,
    Rdna2Scarlett = 1,
    HostReference = 2
};

enum class ScxsPayloadFormat : uint32_t {
    EmbeddedDxbcOrDxil = 1,
    Rdna2Isa           = 2
};

// Clean-room precompiled Scarlett shader container header ("SCXS")
struct ScarlettShaderContainerHeader {
    char     magic[4];           // "SCXS"
    uint16_t containerVersion;   // 1
    uint16_t stage;              // ScxsShaderStage
    uint32_t targetArch;         // ScxsTargetArch
    uint32_t payloadFormat;      // ScxsPayloadFormat
    uint32_t payloadSize;        // Bytes following this 32-byte header
    uint32_t payloadFnv1a;       // FNV-1a 32-bit checksum of payload
    uint32_t shaderModelPacked;  // (major << 8) | minor
    uint32_t flags;
};
static_assert(sizeof(ScarlettShaderContainerHeader) == 32, "SCXS header must be 32 bytes");

inline uint32_t ComputeScxsFnv1a32(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

inline const char* ScxsStageName(uint16_t stage) {
    switch (static_cast<ScxsShaderStage>(stage)) {
        case ScxsShaderStage::Pixel:         return "PS";
        case ScxsShaderStage::Vertex:        return "VS";
        case ScxsShaderStage::Geometry:      return "GS";
        case ScxsShaderStage::Hull:          return "HS";
        case ScxsShaderStage::Domain:        return "DS";
        case ScxsShaderStage::Compute:       return "CS";
        case ScxsShaderStage::Mesh:          return "MS";
        case ScxsShaderStage::Amplification: return "AS";
        default:                             return "UNK";
    }
}

inline bool IsScxsContainer(const D3D12_SHADER_BYTECODE& bc) {
    if (!bc.pShaderBytecode || bc.BytecodeLength < sizeof(ScarlettShaderContainerHeader)) {
        return false;
    }
    return std::memcmp(bc.pShaderBytecode, "SCXS", 4) == 0;
}

inline std::vector<uint8_t> BuildScxsContainer(
    ScxsShaderStage stage,
    ScxsTargetArch arch,
    ScxsPayloadFormat format,
    const void* payload,
    size_t payloadSize,
    uint8_t smMajor,
    uint8_t smMinor,
    uint32_t flags = 0) {
    ScarlettShaderContainerHeader hdr{};
    std::memcpy(hdr.magic, "SCXS", 4);
    hdr.containerVersion = 1;
    hdr.stage = static_cast<uint16_t>(stage);
    hdr.targetArch = static_cast<uint32_t>(arch);
    hdr.payloadFormat = static_cast<uint32_t>(format);
    hdr.payloadSize = static_cast<uint32_t>(payloadSize);
    hdr.payloadFnv1a = (payload && payloadSize > 0) ? ComputeScxsFnv1a32(payload, payloadSize) : 0u;
    hdr.shaderModelPacked = (static_cast<uint32_t>(smMajor) << 8) | static_cast<uint32_t>(smMinor);
    hdr.flags = flags;

    std::vector<uint8_t> out(sizeof(hdr) + payloadSize);
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    if (payload && payloadSize > 0) {
        std::memcpy(out.data() + sizeof(hdr), payload, payloadSize);
    }
    return out;
}

struct TranslatedShader {
    std::vector<uint8_t> bytecode;
    std::string          translatorName;
    std::string          sourceTag;
    bool                 wasTranslated{false};
};

class IShaderTranslator {
public:
    virtual ~IShaderTranslator() = default;
    virtual const char* Name() const = 0;
    virtual bool CanTranslate(const D3D12_SHADER_BYTECODE& input) const = 0;
    virtual HRESULT Translate(
        const D3D12_SHADER_BYTECODE& input,
        TranslatedShader& out,
        std::string& outError) const = 0;
};

// Proof-of-concept binary RDNA2 ISA -> HLSL -> DXBC translator plugin (Pixel Shader subset)
class Rdna2IsaTranslator final : public IShaderTranslator {
public:
    const char* Name() const override { return "Rdna2IsaTranslator"; }

    bool CanTranslate(const D3D12_SHADER_BYTECODE& input) const override {
        if (!IsScxsContainer(input)) {
            return false;
        }
        ScarlettShaderContainerHeader hdr{};
        std::memcpy(&hdr, input.pShaderBytecode, sizeof(hdr));
        const auto stage = static_cast<ScxsShaderStage>(hdr.stage);
        return hdr.containerVersion == 1 &&
               static_cast<ScxsPayloadFormat>(hdr.payloadFormat) == ScxsPayloadFormat::Rdna2Isa &&
               (stage == ScxsShaderStage::Pixel || stage == ScxsShaderStage::Compute);
    }

    HRESULT Translate(
        const D3D12_SHADER_BYTECODE& input,
        TranslatedShader& out,
        std::string& outError) const override {
        if (!input.pShaderBytecode || input.BytecodeLength < sizeof(ScarlettShaderContainerHeader)) {
            outError = "SCXS RDNA2 container truncated";
            return E_INVALIDARG;
        }

        const auto* raw = static_cast<const uint8_t*>(input.pShaderBytecode);
        ScarlettShaderContainerHeader hdr{};
        std::memcpy(&hdr, raw, sizeof(hdr));

        if (hdr.payloadSize == 0 ||
            sizeof(hdr) + static_cast<size_t>(hdr.payloadSize) > input.BytecodeLength) {
            outError = "SCXS RDNA2 payload size out of bounds";
            return E_INVALIDARG;
        }

        const uint8_t* payloadPtr = raw + sizeof(hdr);
        const uint32_t actualCrc = ComputeScxsFnv1a32(payloadPtr, hdr.payloadSize);
        if (actualCrc != hdr.payloadFnv1a) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "SCXS RDNA2 checksum mismatch (expected=0x%08X, actual=0x%08X)",
                          hdr.payloadFnv1a, actualCrc);
            outError = buf;
            return E_INVALIDARG;
        }

        rdna2::DecodedProgram prog{};
        if (!rdna2::DecodeProgram(payloadPtr, hdr.payloadSize, prog, outError)) {
            return E_NOTIMPL;
        }

        const bool isCompute = (static_cast<ScxsShaderStage>(hdr.stage) == ScxsShaderStage::Compute);
        std::string liftedHlsl;
        const bool lifted = isCompute
            ? rdna2::LiftComputeShaderToHlsl(prog, liftedHlsl, outError)
            : rdna2::LiftPixelShaderToHlsl(prog, liftedHlsl, outError);
        if (!lifted) {
            return E_NOTIMPL;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> dxbcBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> compileErr;
        HRESULT hr = D3DCompile(
            liftedHlsl.data(), liftedHlsl.size(),
            isCompute ? "rdna2_lifted_cs.hlsl" : "rdna2_lifted_ps.hlsl", nullptr, nullptr,
            isCompute ? "CSMain" : "PSMain", isCompute ? "cs_5_0" : "ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &dxbcBlob, &compileErr);
        if (FAILED(hr) || !dxbcBlob) {
            outError = compileErr
                ? std::string("RDNA2 lifted HLSL compile failed: ") +
                  static_cast<const char*>(compileErr->GetBufferPointer())
                : "RDNA2 lifted HLSL compile failed";
            return hr;
        }

        const auto* dxbcBytes = static_cast<const uint8_t*>(dxbcBlob->GetBufferPointer());
        out.bytecode.assign(dxbcBytes, dxbcBytes + dxbcBlob->GetBufferSize());
        out.translatorName = Name();
        out.wasTranslated = true;

        char tag[64];
        std::snprintf(tag, sizeof(tag), "SCXS(RDNA2-ISA/%zu insts)->HLSL->", prog.insts.size());
        out.sourceTag = tag;
        return S_OK;
    }
};

class ScxsContainerTranslator final : public IShaderTranslator {
public:
    const char* Name() const override { return "ScxsContainerTranslator"; }

    bool CanTranslate(const D3D12_SHADER_BYTECODE& input) const override {
        return IsScxsContainer(input);
    }

    HRESULT Translate(
        const D3D12_SHADER_BYTECODE& input,
        TranslatedShader& out,
        std::string& outError) const override {
        if (!input.pShaderBytecode || input.BytecodeLength < sizeof(ScarlettShaderContainerHeader)) {
            outError = "SCXS container truncated";
            return E_INVALIDARG;
        }

        const auto* raw = static_cast<const uint8_t*>(input.pShaderBytecode);
        ScarlettShaderContainerHeader hdr{};
        std::memcpy(&hdr, raw, sizeof(hdr));

        if (hdr.containerVersion != 1) {
            outError = "unsupported SCXS container version";
            return E_INVALIDARG;
        }
        if (hdr.payloadSize == 0 ||
            sizeof(hdr) + static_cast<size_t>(hdr.payloadSize) > input.BytecodeLength) {
            outError = "SCXS payload size out of bounds";
            return E_INVALIDARG;
        }

        const uint8_t* payloadPtr = raw + sizeof(hdr);
        const uint32_t actualCrc = ComputeScxsFnv1a32(payloadPtr, hdr.payloadSize);
        if (actualCrc != hdr.payloadFnv1a) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "SCXS checksum mismatch (expected=0x%08X, actual=0x%08X)",
                          hdr.payloadFnv1a, actualCrc);
            outError = buf;
            return E_INVALIDARG;
        }

        const auto fmt = static_cast<ScxsPayloadFormat>(hdr.payloadFormat);
        if (fmt == ScxsPayloadFormat::Rdna2Isa) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "SCXS %s RDNA2 ISA payload (%u B, crc=0x%08X) requires RDNA2->DXIL recompiler backend",
                          ScxsStageName(hdr.stage), hdr.payloadSize, actualCrc);
            outError = buf;
            return E_NOTIMPL;
        }

        if (fmt != ScxsPayloadFormat::EmbeddedDxbcOrDxil) {
            outError = "unknown SCXS payload format";
            return E_INVALIDARG;
        }

        out.bytecode.assign(payloadPtr, payloadPtr + hdr.payloadSize);
        out.translatorName = Name();
        out.wasTranslated = true;

        char tag[64];
        std::snprintf(tag, sizeof(tag), "SCXS(%s/SM%u.%u)->",
                      ScxsStageName(hdr.stage),
                      static_cast<unsigned>((hdr.shaderModelPacked >> 8) & 0xFFu),
                      static_cast<unsigned>(hdr.shaderModelPacked & 0xFFu));
        out.sourceTag = tag;
        return S_OK;
    }
};

class DxbcDxilPassthroughTranslator final : public IShaderTranslator {
public:
    const char* Name() const override { return "DxbcDxilPassthrough"; }

    bool CanTranslate(const D3D12_SHADER_BYTECODE& input) const override {
        return input.pShaderBytecode != nullptr &&
               input.BytecodeLength >= 4 &&
               std::memcmp(input.pShaderBytecode, "DXBC", 4) == 0;
    }

    HRESULT Translate(
        const D3D12_SHADER_BYTECODE& /*input*/,
        TranslatedShader& out,
        std::string& /*outError*/) const override {
        out.bytecode.clear();
        out.translatorName = Name();
        out.sourceTag.clear();
        out.wasTranslated = false;
        return S_OK;
    }
};

class ShaderTranslatorRegistry {
public:
    ShaderTranslatorRegistry() {
        Register(std::make_unique<Rdna2IsaTranslator>());
        Register(std::make_unique<ScxsContainerTranslator>());
        Register(std::make_unique<DxbcDxilPassthroughTranslator>());
    }

    void Register(std::unique_ptr<IShaderTranslator> translator) {
        if (translator) {
            m_translators.push_back(std::move(translator));
        }
    }

    HRESULT Translate(
        const D3D12_SHADER_BYTECODE& input,
        TranslatedShader& out,
        std::string& outError) const {
        if (!input.pShaderBytecode || input.BytecodeLength == 0) {
            out.wasTranslated = false;
            return S_OK;
        }
        for (const auto& t : m_translators) {
            if (t->CanTranslate(input)) {
                return t->Translate(input, out, outError);
            }
        }
        outError = "no registered shader translator handles this container format";
        return E_NOTIMPL;
    }

private:
    std::vector<std::unique_ptr<IShaderTranslator>> m_translators;
};

} // namespace sc::d3d12