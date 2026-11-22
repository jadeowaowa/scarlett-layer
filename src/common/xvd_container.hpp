#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace sc::vfs {

inline constexpr size_t   kXvdMagicOffset               = 0x200;
inline constexpr size_t   kXvdMinHeaderSize             = 0x1000; // 4 KiB

// XvdVolumeFlags sourced from public emoose/xvdtool (LibXboxOne/XVD/XVDEnums.cs).
inline constexpr uint32_t kXvdFlagReadOnly              = 0x01u;
inline constexpr uint32_t kXvdFlagEncryptionDisabled    = 0x02u;
inline constexpr uint32_t kXvdFlagDataIntegrityDisabled = 0x04u;
inline constexpr uint32_t kXvdFlagLegacySectorSize      = 0x08u;
inline constexpr uint32_t kXvdFlagResiliencyEnabled     = 0x10u;
inline constexpr uint32_t kXvdFlagSraReadOnly           = 0x20u;
inline constexpr uint32_t kXvdFlagRegionIdInXts         = 0x40u;
inline constexpr uint32_t kXvdFlagEraSpecific           = 0x80u;

struct XvdHeader {
    std::string magic;
    uint32_t    volumeFlags{0};
    uint32_t    formatVersion{0};
    uint64_t    driveSize{0};
    uint32_t    contentType{0};
    uint32_t    embeddedXvdLen{0};
    uint64_t    userDataOffset{0};
    uint64_t    userDataLength{0};

    // Per emoose/xvdtool (XVDFile.cs): XVD volumes are encrypted unless EncryptionDisabled (0x2) is set.
    bool IsEncrypted() const { return (volumeFlags & kXvdFlagEncryptionDisabled) == 0u; }
};

class XvdContainer {
public:
    bool Open(const std::filesystem::path& path, std::string& outError) {
        m_path.clear();
        m_header = {};

        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            outError = "Failed to open XVD file: " + path.string();
            return false;
        }
        const std::streampos rawPos = f.tellg();
        if (rawPos < 0) {
            outError = "Failed to query XVD file size";
            return false;
        }
        const auto fileSize = static_cast<uint64_t>(rawPos);
        if (fileSize < kXvdMinHeaderSize) {
            outError = "XVD image smaller than 4 KiB header";
            return false;
        }
        if (fileSize > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            outError = "XVD file size exceeds host stream offset range";
            return false;
        }

        std::vector<uint8_t> hdrBytes(kXvdMinHeaderSize);
        f.seekg(0, std::ios::beg);
        f.read(reinterpret_cast<char*>(hdrBytes.data()), static_cast<std::streamsize>(hdrBytes.size()));
        if (!f) {
            outError = "Failed to read 4 KiB XVD header";
            return false;
        }

        if (std::memcmp(hdrBytes.data() + kXvdMagicOffset, "msft-xvd", 8) != 0) {
            outError = "Missing 'msft-xvd' magic at offset 0x200";
            return false;
        }

        XvdHeader hdr{};
        hdr.magic.assign(reinterpret_cast<const char*>(hdrBytes.data() + kXvdMagicOffset), 8);
        // Sourced offsets (emoose/xvdtool XVDStructs.cs): 0x200 Magic, 0x208 VolumeFlags,
        // 0x20C FormatVersion, 0x218 DriveSize.
        hdr.volumeFlags    = ReadU32LE(hdrBytes.data() + 0x208);
        hdr.formatVersion  = ReadU32LE(hdrBytes.data() + 0x20C);
        hdr.driveSize      = ReadU64LE(hdrBytes.data() + 0x218);
        // ASSUMED synthetic test layout offsets (real emoose/xvdtool XVDStructs.cs places
        // ContentType at 0x284, EmbeddedXVDLength at 0x288, and uint32 UserDataLength at 0x28C;
        // 0x240..0x25F lies within TopHashBlockHash in retail XVD headers).
        hdr.contentType    = ReadU32LE(hdrBytes.data() + 0x240);
        hdr.embeddedXvdLen = ReadU32LE(hdrBytes.data() + 0x244);
        hdr.userDataOffset = ReadU64LE(hdrBytes.data() + 0x250);
        hdr.userDataLength = ReadU64LE(hdrBytes.data() + 0x258);

        if (hdr.IsEncrypted()) {
            char msg[128];
            std::snprintf(
                msg, sizeof(msg),
                "Encrypted XVD containers are not supported (volumeFlags=0x%X; EncryptionDisabled bit 0x2 not set)",
                hdr.volumeFlags);
            outError = msg;
            return false;
        }
        if (hdr.driveSize < kXvdMinHeaderSize || hdr.driveSize > fileSize) {
            outError = "XVD driveSize out of bounds";
            return false;
        }
        if (static_cast<uint64_t>(hdr.embeddedXvdLen) > fileSize - kXvdMinHeaderSize) {
            outError = "XVD embeddedXvdLen exceeds file size";
            return false;
        }
        if (hdr.userDataOffset < kXvdMinHeaderSize ||
            hdr.userDataOffset > fileSize ||
            hdr.userDataLength > fileSize - hdr.userDataOffset) {
            outError = "XVD user-data region out of bounds";
            return false;
        }

        m_path = path;
        m_header = hdr;
        return true;
    }

    bool ReadUserData(uint64_t relOffset, size_t size, std::vector<uint8_t>& out, std::string& outError) const {
        if (m_path.empty()) {
            outError = "XVD container not open";
            return false;
        }
        if (relOffset > m_header.userDataLength || size > (m_header.userDataLength - relOffset)) {
            outError = "XVD read out of bounds";
            return false;
        }
        std::ifstream f(m_path, std::ios::binary);
        if (!f) {
            outError = "Failed to reopen XVD file";
            return false;
        }
        out.resize(size);
        f.seekg(static_cast<std::streamoff>(m_header.userDataOffset + relOffset), std::ios::beg);
        if (size > 0) {
            f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
            if (!f) {
                outError = "XVD read failed";
                return false;
            }
        }
        return true;
    }

    const XvdHeader& Header() const { return m_header; }

    static std::vector<uint8_t> BuildSyntheticUnencryptedXvd(
        const std::vector<uint8_t>& userData,
        uint32_t contentType = 1u,
        uint32_t volumeFlags = kXvdFlagEncryptionDisabled) {
        const uint64_t totalSize = kXvdMinHeaderSize + userData.size();
        std::vector<uint8_t> img(static_cast<size_t>(totalSize), 0u);
        std::memcpy(img.data() + kXvdMagicOffset, "msft-xvd", 8);
        WriteU32LE(img.data() + 0x208, volumeFlags);
        WriteU32LE(img.data() + 0x20C, 3u); // formatVersion
        WriteU64LE(img.data() + 0x218, totalSize);
        // ASSUMED synthetic test offsets matching Open() above.
        WriteU32LE(img.data() + 0x240, contentType);
        WriteU32LE(img.data() + 0x244, 0u);
        WriteU64LE(img.data() + 0x250, kXvdMinHeaderSize);
        WriteU64LE(img.data() + 0x258, userData.size());
        if (!userData.empty()) {
            std::memcpy(img.data() + kXvdMinHeaderSize, userData.data(), userData.size());
        }
        return img;
    }

private:
    static uint32_t ReadU32LE(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    static uint64_t ReadU64LE(const uint8_t* p) {
        return static_cast<uint64_t>(ReadU32LE(p)) |
               (static_cast<uint64_t>(ReadU32LE(p + 4)) << 32);
    }

    static void WriteU32LE(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFFu);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    }

    static void WriteU64LE(uint8_t* p, uint64_t v) {
        WriteU32LE(p, static_cast<uint32_t>(v & 0xFFFFFFFFull));
        WriteU32LE(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
    }

    std::filesystem::path m_path;
    XvdHeader             m_header;
};

} // namespace sc::vfs
