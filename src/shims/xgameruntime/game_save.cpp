#include "common/config.hpp"
#include "common/stub.hpp"
#include "gdk_types.hpp"
#include "runtime_internal.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct XGameSaveProvider {
    uint64_t xuid{0};
    std::string scid;
    std::filesystem::path rootDir;
};

struct XGameSaveContainer {
    std::string name;
    std::filesystem::path dirPath;
};

struct BlobOperation {
    bool isDelete{false};
    std::string name;
    std::vector<uint8_t> data;
};

struct XGameSaveUpdate {
    std::filesystem::path containerDir;
    std::string displayName;
    std::mutex mtx;
    std::vector<BlobOperation> ops;
};

namespace {

std::string SanitizePathComponent(const char* raw) {
    if (!raw || !*raw) {
        return {};
    }
    std::string out;
    for (const char* p = raw; *p; ++p) {
        unsigned char ch = static_cast<unsigned char>(*p);
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    if (out == "." || out == ".." || out.find("..") != std::string::npos) {
        return {};
    }
    return out;
}

std::filesystem::path ResolveSavesBaseDir() {
    return sc::config::ResolveDataDir(L"saves");
}

size_t AlignUp8(size_t value) {
    return (value + 7ULL) & ~7ULL;
}

struct ProviderInitPayload {
    XGameSaveProviderHandle provider{nullptr};
    ~ProviderInitPayload() {
        delete provider;
    }
};

struct LoadedBlob {
    std::string name;
    std::vector<uint8_t> bytes;
};

struct ReadBlobsPayload {
    std::vector<LoadedBlob> blobs;
};

} // namespace

extern "C" {

HRESULT WINAPI XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool /*syncOnDemand*/,
    XAsyncBlock* async) {
    if (!configurationId || !*configurationId || !async) {
        return E_INVALIDARG;
    }

    uint64_t xuid = sc::runtime::GetUserXuid(requestingUser);
    std::string cleanScid = SanitizePathComponent(configurationId);
    if (cleanScid.empty()) {
        return E_INVALIDARG;
    }

    return sc::runtime::LaunchAsync(async, [xuid, cleanScid](std::shared_ptr<void>& outPayload) -> HRESULT {
        char xuidHex[24];
        std::snprintf(xuidHex, sizeof(xuidHex), "%016llX", static_cast<unsigned long long>(xuid));

        std::filesystem::path root = ResolveSavesBaseDir() / xuidHex / cleanScid;
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) {
            sc::log::Write(sc::log::Level::Error, "xgameruntime",
                           "XGameSaveInitializeProviderAsync[Work]",
                           "-> failed to create local save directory");
            return E_FAIL;
        }

        auto* provider = new XGameSaveProvider();
        provider->xuid = xuid;
        provider->scid = cleanScid;
        provider->rootDir = root;

        auto holder = std::make_shared<ProviderInitPayload>();
        holder->provider = provider;
        outPayload = std::move(holder);

        std::string msg = "-> mounted local save root: " + root.string();
        sc::log::Write(sc::log::Level::Info, "xgameruntime",
                       "XGameSaveInitializeProviderAsync[Work]", msg.c_str());
        return S_OK;
    });
}

HRESULT WINAPI XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider) {
    if (!async || !provider) {
        return E_INVALIDARG;
    }
    auto* state = sc::runtime::LoadAsyncState(async);
    if (!state) {
        return E_INVALIDARG;
    }

    HRESULT hr = E_PENDING;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        if (!state->completed) {
            return E_PENDING;
        }
        hr = state->status;
        if (SUCCEEDED(hr) && state->payload) {
            auto holder = std::static_pointer_cast<ProviderInitPayload>(state->payload);
            *provider = holder->provider;
            holder->provider = nullptr;
        }
    }

    sc::runtime::DetachAsyncState(async, state);
    return hr;
}

void WINAPI XGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    if (provider) {
        delete provider;
        sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> closed save provider");
    }
}

HRESULT WINAPI XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* result) {
    if (!provider || !name || !result) {
        return E_INVALIDARG;
    }
    std::string cleanName = SanitizePathComponent(name);
    if (cleanName.empty()) {
        return E_GS_INVALID_CONTAINER_NAME;
    }

    std::filesystem::path containerDir = provider->rootDir / cleanName;
    std::error_code ec;
    std::filesystem::create_directories(containerDir, ec);
    if (ec) {
        return E_FAIL;
    }

    auto* container = new XGameSaveContainer();
    container->name = std::move(cleanName);
    container->dirPath = std::move(containerDir);
    *result = container;

    std::string msg = "-> opened container '" + container->name + "'";
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, msg.c_str());
    return S_OK;
}

void WINAPI XGameSaveCloseContainer(XGameSaveContainerHandle context) {
    if (context) {
        delete context;
    }
}

HRESULT WINAPI XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* updateContext) {
    if (!container || !updateContext) {
        return E_INVALIDARG;
    }
    auto* upd = new XGameSaveUpdate();
    upd->containerDir = container->dirPath;
    upd->displayName = containerDisplayName ? containerDisplayName : container->name;
    *updateContext = upd;
    return S_OK;
}

void WINAPI XGameSaveCloseUpdate(XGameSaveUpdateHandle context) {
    delete context;
}

HRESULT WINAPI XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount) {
    if (!updateContext || !blobName || (!data && byteCount > 0)) {
        return E_INVALIDARG;
    }
    std::string cleanBlob = SanitizePathComponent(blobName);
    if (cleanBlob.empty()) {
        return E_INVALIDARG;
    }

    BlobOperation op{};
    op.isDelete = false;
    op.name = std::move(cleanBlob);
    if (byteCount > 0) {
        op.data.assign(data, data + byteCount);
    }

    std::lock_guard<std::mutex> lock(updateContext->mtx);
    updateContext->ops.push_back(std::move(op));
    return S_OK;
}

HRESULT WINAPI XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName) {
    if (!updateContext || !blobName) {
        return E_INVALIDARG;
    }
    std::string cleanBlob = SanitizePathComponent(blobName);
    if (cleanBlob.empty()) {
        return E_INVALIDARG;
    }

    BlobOperation op{};
    op.isDelete = true;
    op.name = std::move(cleanBlob);

    std::lock_guard<std::mutex> lock(updateContext->mtx);
    updateContext->ops.push_back(std::move(op));
    return S_OK;
}

HRESULT WINAPI XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async) {
    if (!updateContext || !async) {
        return E_INVALIDARG;
    }

    std::filesystem::path containerDir = updateContext->containerDir;
    std::vector<BlobOperation> opsSnapshot;
    {
        std::lock_guard<std::mutex> lock(updateContext->mtx);
        opsSnapshot = updateContext->ops;
    }

    return sc::runtime::LaunchAsync(async, [containerDir, ops = std::move(opsSnapshot)](
        std::shared_ptr<void>& /*outPayload*/) -> HRESULT {
        std::error_code ec;
        std::filesystem::create_directories(containerDir, ec);
        if (ec) {
            return E_FAIL;
        }

        for (const auto& op : ops) {
            std::filesystem::path finalPath = containerDir / (op.name + ".bin");
            if (op.isDelete) {
                std::filesystem::remove(finalPath, ec);
                continue;
            }

            std::filesystem::path tmpPath = containerDir / (op.name + ".bin.tmp");
            {
                std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    return E_FAIL;
                }
                if (!op.data.empty()) {
                    out.write(reinterpret_cast<const char*>(op.data.data()),
                              static_cast<std::streamsize>(op.data.size()));
                }
                out.flush();
                if (!out.good()) {
                    return E_FAIL;
                }
            }

            if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return HRESULT_FROM_WIN32(GetLastError());
            }

            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> atomically wrote blob '%s' (%zu bytes)",
                          op.name.c_str(), op.data.size());
            sc::log::Write(sc::log::Level::Info, "xgameruntime",
                           "XGameSaveSubmitUpdateAsync[Work]", msg);
        }
        return S_OK;
    });
}

HRESULT WINAPI XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) {
        return E_INVALIDARG;
    }
    auto* state = sc::runtime::LoadAsyncState(async);
    if (!state) {
        return E_INVALIDARG;
    }

    HRESULT hr = E_PENDING;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        if (!state->completed) {
            return E_PENDING;
        }
        hr = state->status;
    }

    sc::runtime::DetachAsyncState(async, state);
    return hr;
}

HRESULT WINAPI XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async) {
    if (!container || !blobNames || countOfBlobs == 0 || !async) {
        return E_INVALIDARG;
    }

    std::filesystem::path containerDir = container->dirPath;
    std::vector<std::string> requested;
    requested.reserve(countOfBlobs);
    for (uint32_t i = 0; i < countOfBlobs; ++i) {
        std::string clean = SanitizePathComponent(blobNames[i]);
        if (clean.empty()) {
            return E_INVALIDARG;
        }
        requested.push_back(std::move(clean));
    }

    return sc::runtime::LaunchAsync(async, [containerDir, names = std::move(requested)](
        std::shared_ptr<void>& outPayload) -> HRESULT {
        auto payload = std::make_shared<ReadBlobsPayload>();
        payload->blobs.reserve(names.size());

        for (const auto& name : names) {
            std::filesystem::path blobPath = containerDir / (name + ".bin");
            std::error_code ec;
            if (!std::filesystem::exists(blobPath, ec)) {
                std::string msg = "-> blob '" + name + "' not found (E_GS_BLOB_NOT_FOUND)";
                sc::log::Write(sc::log::Level::Info, "xgameruntime",
                               "XGameSaveReadBlobDataAsync[Work]", msg.c_str());
                return E_GS_BLOB_NOT_FOUND;
            }

            std::ifstream in(blobPath, std::ios::binary | std::ios::ate);
            if (!in.is_open()) {
                return E_FAIL;
            }
            std::streamsize sz = in.tellg();
            if (sz < 0) {
                return E_FAIL;
            }
            in.seekg(0, std::ios::beg);

            LoadedBlob lb{};
            lb.name = name;
            lb.bytes.resize(static_cast<size_t>(sz));
            if (sz > 0 && !in.read(reinterpret_cast<char*>(lb.bytes.data()), sz)) {
                return E_FAIL;
            }

            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> loaded blob '%s' (%zu bytes)",
                          lb.name.c_str(), lb.bytes.size());
            sc::log::Write(sc::log::Level::Info, "xgameruntime",
                           "XGameSaveReadBlobDataAsync[Work]", msg);
            payload->blobs.push_back(std::move(lb));
        }

        outPayload = std::move(payload);
        return S_OK;
    });
}

HRESULT WINAPI XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs) {
    if (!async || !blobData || !countOfBlobs) {
        return E_INVALIDARG;
    }
    auto* state = sc::runtime::LoadAsyncState(async);
    if (!state) {
        return E_INVALIDARG;
    }

    HRESULT hr = E_PENDING;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        if (!state->completed) {
            return E_PENDING;
        }
        hr = state->status;
        if (SUCCEEDED(hr) && state->payload) {
            auto payload = std::static_pointer_cast<ReadBlobsPayload>(state->payload);
            const size_t count = payload->blobs.size();

            // Compute exact packed size: [XGameSaveBlob * N] [names] [8-byte aligned data]
            size_t cursor = count * sizeof(XGameSaveBlob);
            for (const auto& b : payload->blobs) {
                cursor += b.name.size() + 1;
            }
            for (const auto& b : payload->blobs) {
                cursor = AlignUp8(cursor);
                cursor += b.bytes.size();
            }

            if (blobsSize < cursor) {
                return E_GS_PROVIDED_BUFFER_TOO_SMALL; // keep AsyncState alive for retry
            }

            auto* rawBuf = reinterpret_cast<uint8_t*>(blobData);
            size_t writePos = count * sizeof(XGameSaveBlob);

            for (size_t i = 0; i < count; ++i) {
                const auto& b = payload->blobs[i];
                char* nameDst = reinterpret_cast<char*>(rawBuf + writePos);
                std::memcpy(nameDst, b.name.c_str(), b.name.size() + 1);
                writePos += b.name.size() + 1;

                blobData[i].info.name = nameDst;
                blobData[i].info.size = static_cast<uint32_t>(b.bytes.size());
            }

            for (size_t i = 0; i < count; ++i) {
                const auto& b = payload->blobs[i];
                writePos = AlignUp8(writePos);
                uint8_t* dataDst = rawBuf + writePos;
                if (!b.bytes.empty()) {
                    std::memcpy(dataDst, b.bytes.data(), b.bytes.size());
                }
                writePos += b.bytes.size();
                blobData[i].data = dataDst;
            }

            *countOfBlobs = static_cast<uint32_t>(count);
        }
    }

    sc::runtime::DetachAsyncState(async, state);
    return hr;
}

} // extern "C"