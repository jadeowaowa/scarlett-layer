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

// Real XVD/XVC header geometry sourced from public emoose/xvdtool
// (LibXboxOne/XVD/XVDFile.cs & XVDStructs.cs):
//   0x000..0x1FF : 0x200-byte RSA signature
//   0x200..0x2FFF: 0x2E00-byte XvdHeader (total 0x3000 = 12 KiB = 3 pages)
inline constexpr size_t   kXvdMagicOffset               = 0x200;
inline constexpr size_t   kXvdPageSize                  = 0x1000; // 4 KiB
inline constexpr size_t   kXvdMinHeaderSize             = 0x3000; // 12 KiB (signature + header)
inline constexpr uint64_t kXvdHashEntriesPerPage        = 0xAAu;  // 4096 / 24 = 170 entries/page

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
    uint32_t    xvdType{0};
    uint32_t    contentType{0};
    uint32_t    embeddedXvdLen{0};
    uint8_t     mutableDataPageCount{0};
    uint64_t    hashTreeOffset{0};
    uint64_t    hashTreeLength{0};
    uint64_t    userDataOffset{0};
    uint64_t    userDataLength{0};

    // Per emoose/xvdtool (XVDFile.cs): XVD volumes are encrypted unless EncryptionDisabled (0x2) is set.
    bool IsEncrypted() const { return (volumeFlags & kXvdFlagEncryptionDisabled) == 0u; }
    bool IsDataIntegrityEnabled() const { return (volumeFlags & kXvdFlagDataIntegrityDisabled) == 0u; }
};

class XvdContainer {
public:
    static uint64_t BytesToPages(uint64_t bytes) {
        if (bytes == 0u) {
            return 0u;
        }
        return (bytes - 1u) / kXvdPageSize + 1u;
    }

    static uint64_t CalculateHashTreePages(uint64_t hashedPages, bool resiliencyEnabled) {
        if (hashedPages == 0u) {
            return 0u;
        }
        uint64_t total = 0u;
        uint64_t levelPages = hashedPages;
        while (levelPages > 1u) {
            levelPages = (levelPages - 1u) / kXvdHashEntriesPerPage + 1u;
            total += levelPages;
        }
        if (total == 0u) {
            total = 1u;
        }
        if (resiliencyEnabled) {
            total *= 2u;
        }
        return total;
    }

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
            outError = "XVD image smaller than 12 KiB (0x3000) header";
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
            outError = "Failed to read 12 KiB XVD header";
            return false;
        }

        if (std::memcmp(hdrBytes.data() + kXvdMagicOffset, "msft-xvd", 8) != 0) {
            outError = "Missing 'msft-xvd' magic at offset 0x200";
            return false;
        }

        XvdHeader hdr{};
        hdr.magic.assign(reinterpret_cast<const char*>(hdrBytes.data() + kXvdMagicOffset), 8);
        // Sourced offsets (emoose/xvdtool XVDStructs.cs; header starts at 0x200):
        //   0x200 (+0x000) Magic, 0x208 (+0x008) VolumeFlags, 0x20C (+0x00C) FormatVersion,
        //   0x218 (+0x018) DriveSize, 0x280 (+0x080) Type, 0x284 (+0x084) ContentType,
        //   0x288 (+0x088) EmbeddedXVDLength, 0x28C (+0x08C) UserDataLength (uint32),
        //   0x470 (+0x270) MutableDataPageCount (uint8).
        hdr.volumeFlags          = ReadU32LE(hdrBytes.data() + 0x208);
        hdr.formatVersion        = ReadU32LE(hdrBytes.data() + 0x20C);
        hdr.driveSize            = ReadU64LE(hdrBytes.data() + 0x218);
        hdr.xvdType              = ReadU32LE(hdrBytes.data() + 0x280);
        hdr.contentType          = ReadU32LE(hdrBytes.data() + 0x284);
        hdr.embeddedXvdLen       = ReadU32LE(hdrBytes.data() + 0x288);
        hdr.userDataLength       = ReadU32LE(hdrBytes.data() + 0x28C);
        hdr.mutableDataPageCount = hdrBytes[0x470];

        if (hdr.IsEncrypted()) {
            char msg[128];
            std::snprintf(
                msg, sizeof(msg),
                "Encrypted XVD containers are not supported (volumeFlags=0x%X; EncryptionDisabled bit 0x2 not set)",
                hdr.volumeFlags);
            outError = msg;
            return false;
        }
        if (hdr.driveSize < kXvdPageSize || hdr.driveSize > fileSize) {
            outError = "XVD driveSize out of bounds";
            return false;
        }

        // Compute real XVD section layout per emoose/xvdtool (XVDFile.cs):
        //   EmbeddedXvdOffset = 0x3000
        //   MduOffset         = EmbeddedXvdOffset + PageAlign(EmbeddedXVDLength)
        //   HashTreeOffset    = MduOffset + MutableDataPageCount * 0x1000
        //   UserDataOffset    = HashTreeOffset + (IsDataIntegrityEnabled ? HashTreePages * 0x1000 : 0)
        const uint64_t embeddedXvdBytes = BytesToPages(hdr.embeddedXvdLen) * kXvdPageSize;
        if (embeddedXvdBytes > fileSize - kXvdMinHeaderSize) {
            outError = "XVD embeddedXvdLen exceeds file size";
            return false;
        }
        const uint64_t mduOffset = kXvdMinHeaderSize + embeddedXvdBytes;

        const uint64_t mutableDataBytes = static_cast<uint64_t>(hdr.mutableDataPageCount) * kXvdPageSize;
        if (mutableDataBytes > fileSize - mduOffset) {
            outError = "XVD mutableDataPageCount exceeds file size";
            return false;
        }
        hdr.hashTreeOffset = mduOffset + mutableDataBytes;

        if (hdr.IsDataIntegrityEnabled()) {
            const uint64_t drivePages = BytesToPages(hdr.driveSize);
            const uint64_t userDataPages = BytesToPages(hdr.userDataLength);
            const uint64_t hashedPages = drivePages + userDataPages;
            const bool resilient = (hdr.volumeFlags & kXvdFlagResiliencyEnabled) != 0u;
            const uint64_t hashTreePages = CalculateHashTreePages(hashedPages, resilient);
            if (hashTreePages > (fileSize - hdr.hashTreeOffset) / kXvdPageSize) {
                outError = "XVD hash-tree region exceeds file size";
                return false;
            }
            hdr.hashTreeLength = hashTreePages * kXvdPageSize;
        } else {
            hdr.hashTreeLength = 0u;
        }

        hdr.userDataOffset = hdr.hashTreeOffset + hdr.hashTreeLength;
        if (hdr.userDataLength > fileSize - hdr.userDataOffset) {
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
        uint32_t volumeFlags = kXvdFlagEncryptionDisabled,
        uint32_t embeddedXvdLen = 0u,
        uint8_t mutableDataPageCount = 0u) {
        const uint64_t embeddedXvdBytes = BytesToPages(embeddedXvdLen) * kXvdPageSize;
        const uint64_t mutableDataBytes = static_cast<uint64_t>(mutableDataPageCount) * kXvdPageSize;
        const uint64_t hashTreeOffset = kXvdMinHeaderSize + embeddedXvdBytes + mutableDataBytes;

        const uint64_t userDataPages = BytesToPages(userData.size());
        const uint64_t drivePages = (userDataPages > 0u) ? userDataPages : 1u;
        const uint64_t driveSize = drivePages * kXvdPageSize;

        uint64_t hashTreeBytes = 0u;
        if ((volumeFlags & kXvdFlagDataIntegrityDisabled) == 0u) {
            const bool resilient = (volumeFlags & kXvdFlagResiliencyEnabled) != 0u;
            hashTreeBytes = CalculateHashTreePages(drivePages + userDataPages, resilient) * kXvdPageSize;
        }

        const uint64_t userDataOffset = hashTreeOffset + hashTreeBytes;
        const uint64_t totalSize = userDataOffset + userData.size();
        std::vector<uint8_t> img(static_cast<size_t>(totalSize), 0u);
        std::memcpy(img.data() + kXvdMagicOffset, "msft-xvd", 8);
        WriteU32LE(img.data() + 0x208, volumeFlags);
        WriteU32LE(img.data() + 0x20C, 3u); // formatVersion
        WriteU64LE(img.data() + 0x218, driveSize);
        WriteU32LE(img.data() + 0x280, 0u); // Fixed XVD
        WriteU32LE(img.data() + 0x284, contentType);
        WriteU32LE(img.data() + 0x288, embeddedXvdLen);
        WriteU32LE(img.data() + 0x28C, static_cast<uint32_t>(userData.size()));
        img[0x470] = mutableDataPageCount;

        if (!userData.empty()) {
            std::memcpy(img.data() + static_cast<size_t>(userDataOffset), userData.data(), userData.size());
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
