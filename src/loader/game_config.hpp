#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace sc::loader {

struct GameConfig {
    bool valid = false;
    std::string error;

    std::filesystem::path packageRoot;
    std::filesystem::path configPath;
    std::filesystem::path executablePath;

    std::string identityName;
    std::string publisher;
    std::string version;
    std::string titleId;
    std::string displayName;
    std::string executableName;
    std::string executableId;
};

namespace detail {

inline std::string Trim(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

inline std::string ExtractTagText(const std::string& xml, std::string_view tagName) {
    const std::string openPrefix = "<" + std::string(tagName);
    const std::string closeTag = "</" + std::string(tagName) + ">";
    size_t pos = xml.find(openPrefix);
    while (pos != std::string::npos) {
        const size_t afterName = pos + openPrefix.size();
        if (afterName < xml.size() &&
            (xml[afterName] == '>' || std::isspace(static_cast<unsigned char>(xml[afterName])))) {
            const size_t gt = xml.find('>', afterName);
            if (gt == std::string::npos) {
                return {};
            }
            const size_t closePos = xml.find(closeTag, gt + 1);
            if (closePos == std::string::npos) {
                return {};
            }
            return Trim(std::string_view(xml).substr(gt + 1, closePos - (gt + 1)));
        }
        pos = xml.find(openPrefix, afterName);
    }
    return {};
}

inline std::string ExtractTagHeader(const std::string& xml, std::string_view tagName) {
    const std::string openPrefix = "<" + std::string(tagName);
    size_t pos = xml.find(openPrefix);
    while (pos != std::string::npos) {
        const size_t afterName = pos + openPrefix.size();
        if (afterName < xml.size() &&
            (xml[afterName] == '>' || xml[afterName] == '/' ||
             std::isspace(static_cast<unsigned char>(xml[afterName])))) {
            const size_t gt = xml.find('>', afterName);
            if (gt == std::string::npos) {
                return {};
            }
            return xml.substr(pos, gt - pos + 1);
        }
        pos = xml.find(openPrefix, afterName);
    }
    return {};
}

inline std::string ExtractAttr(const std::string& tagHeader, std::string_view attrName) {
    if (tagHeader.empty()) {
        return {};
    }
    const std::string key = std::string(attrName) + "=";
    size_t pos = tagHeader.find(key);
    while (pos != std::string::npos) {
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(tagHeader[pos - 1]))) {
            pos = tagHeader.find(key, pos + key.size());
            continue;
        }
        size_t qStart = pos + key.size();
        while (qStart < tagHeader.size() && std::isspace(static_cast<unsigned char>(tagHeader[qStart]))) {
            ++qStart;
        }
        if (qStart >= tagHeader.size() || (tagHeader[qStart] != '"' && tagHeader[qStart] != '\'')) {
            return {};
        }
        const char quote = tagHeader[qStart];
        const size_t qEnd = tagHeader.find(quote, qStart + 1);
        if (qEnd == std::string::npos) {
            return {};
        }
        return tagHeader.substr(qStart + 1, qEnd - (qStart + 1));
    }
    return {};
}

} // namespace detail

inline GameConfig ParseGameConfig(const std::filesystem::path& pathOrDir) {
    GameConfig cfg{};
    std::error_code ec;
    const auto absInput = std::filesystem::absolute(pathOrDir, ec);
    if (ec || !std::filesystem::exists(absInput, ec)) {
        cfg.error = "path does not exist";
        return cfg;
    }

    if (std::filesystem::is_directory(absInput, ec)) {
        cfg.packageRoot = absInput;
        cfg.configPath = absInput / "MicrosoftGame.Config";
    } else {
        cfg.configPath = absInput;
        cfg.packageRoot = absInput.parent_path();
    }

    if (!std::filesystem::exists(cfg.configPath, ec)) {
        cfg.error = "MicrosoftGame.Config not found";
        return cfg;
    }

    std::ifstream ifs(cfg.configPath, std::ios::binary);
    if (!ifs) {
        cfg.error = "failed to open MicrosoftGame.Config";
        return cfg;
    }
    std::string xml((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (xml.find("<Game") == std::string::npos) {
        cfg.error = "missing <Game> root element";
        return cfg;
    }

    const std::string idTag = detail::ExtractTagHeader(xml, "Identity");
    cfg.identityName = detail::ExtractAttr(idTag, "Name");
    cfg.publisher = detail::ExtractAttr(idTag, "Publisher");
    cfg.version = detail::ExtractAttr(idTag, "Version");

    cfg.titleId = detail::ExtractTagText(xml, "TitleId");

    const std::string shellTag = detail::ExtractTagHeader(xml, "ShellVisuals");
    cfg.displayName = detail::ExtractAttr(shellTag, "DefaultDisplayName");

    const std::string exeTag = detail::ExtractTagHeader(xml, "Executable");
    cfg.executableName = detail::ExtractAttr(exeTag, "Name");
    cfg.executableId = detail::ExtractAttr(exeTag, "Id");

    if (cfg.executableName.empty()) {
        cfg.error = "missing <Executable Name=\"...\"/>";
        return cfg;
    }

    cfg.executablePath = cfg.packageRoot / cfg.executableName;
    cfg.valid = true;
    return cfg;
}

} // namespace sc::loader