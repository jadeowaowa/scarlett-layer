#include "dstorage_xs.hpp"
#include "common/stub.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Private internal GUIDs so queue workers can access concrete file/status internals safely
constexpr GUID IID_ScarlettFileInternal =
    { 0x5c4a41F1, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0xF1 } };

constexpr GUID IID_ScarlettStatusInternal =
    { 0x5c4a41F2, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0xF2 } };

std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
    return out;
}

class ScarlettStorageFileX final : public IDStorageFileX {
public:
    ScarlettStorageFileX(HANDLE handle, std::string displayPath)
        : m_handle(handle)
        , m_path(std::move(displayPath)) {}

    ~ScarlettStorageFileX() {
        Close();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, IID_IDStorageFileX) ||
            IsEqualGUID(riid, IID_ScarlettFileInternal)) {
            *ppvObject = static_cast<IDStorageFileX*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    void STDMETHODCALLTYPE Close() override {
        std::lock_guard<std::mutex> lock(m_ioMtx);
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        if (!info) {
            return E_POINTER;
        }
        std::lock_guard<std::mutex> lock(m_ioMtx);
        if (m_handle == INVALID_HANDLE_VALUE) {
            return E_HANDLE;
        }
        if (!GetFileInformationByHandle(m_handle, info)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }

    HRESULT ReadSlice(uint64_t offset, void* dst, uint32_t size, uint32_t& outRead) {
        outRead = 0;
        if (!dst && size > 0) {
            return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(m_ioMtx);
        if (m_handle == INVALID_HANDLE_VALUE) {
            return E_HANDLE;
        }

        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(m_handle, pos, nullptr, FILE_BEGIN)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        uint8_t* cursor = static_cast<uint8_t*>(dst);
        uint32_t remaining = size;
        while (remaining > 0) {
            DWORD chunkRead = 0;
            if (!ReadFile(m_handle, cursor, remaining, &chunkRead, nullptr)) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (chunkRead == 0) {
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            }
            cursor += chunkRead;
            remaining -= chunkRead;
            outRead += chunkRead;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::mutex m_ioMtx;
    HANDLE m_handle{INVALID_HANDLE_VALUE};
    std::string m_path;
};

class ScarlettStatusArrayX final : public IDStorageStatusArrayX {
public:
    ScarlettStatusArrayX(uint32_t capacity, std::string name)
        : m_name(std::move(name))
        , m_slots(capacity) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, IID_IDStorageStatusArrayX) ||
            IsEqualGUID(riid, IID_ScarlettStatusInternal)) {
            *ppvObject = static_cast<IDStorageStatusArrayX*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    bool STDMETHODCALLTYPE IsComplete(uint32_t index) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (index >= m_slots.size()) {
            return false;
        }
        return m_slots[index].complete;
    }

    HRESULT STDMETHODCALLTYPE GetHResult(uint32_t index) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (index >= m_slots.size()) {
            return E_INVALIDARG;
        }
        return m_slots[index].hr;
    }

    void ResetSlot(uint32_t index) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (index < m_slots.size()) {
            m_slots[index].complete = false;
            m_slots[index].hr = E_PENDING;
        }
    }

    void CompleteSlot(uint32_t index, HRESULT hr) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (index < m_slots.size()) {
            m_slots[index].hr = hr;
            m_slots[index].complete = true;
        }
    }

private:
    struct Slot {
        bool complete{true};
        HRESULT hr{S_OK};
    };

    std::atomic<ULONG> m_refs{1};
    std::string m_name;
    std::mutex m_mtx;
    std::vector<Slot> m_slots;
};

enum class QueueOpKind {
    ReadRequest,
    StatusUpdate,
    FenceSignal
};

struct QueuedOp {
    QueueOpKind kind{QueueOpKind::ReadRequest};

    // ReadRequest fields
    DSTORAGE_REQUEST_OPTIONS options{};
    ComPtr<ScarlettStorageFileX> file;
    uint64_t fileOffset{0};
    const void* srcMemory{nullptr};
    uint32_t srcSize{0};
    void* dstMemory{nullptr};
    uint32_t dstSize{0};
    std::string reqName;

    // StatusUpdate fields
    ComPtr<ScarlettStatusArrayX> statusArray;
    uint32_t statusIndex{0};

    // FenceSignal fields
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue{0};
};

class ScarlettStorageQueueX final : public IDStorageQueueX {
public:
    ScarlettStorageQueueX(const DSTORAGE_QUEUE_DESC& desc)
        : m_desc(desc)
        , m_name(desc.Name ? desc.Name : "unnamed_queue") {
        m_worker = std::thread([this]() { WorkerLoop(); });
        char buf[160];
        std::snprintf(buf, sizeof(buf), "-> created queue '%s' (capacity=%u, priority=%d)",
                      m_name.c_str(), static_cast<unsigned>(m_desc.Capacity),
                      static_cast<int>(m_desc.Priority));
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "CreateQueue", buf);
    }

    ~ScarlettStorageQueueX() {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::string msg = "-> closed queue '" + m_name + "'";
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "~ScarlettStorageQueueX", msg.c_str());
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IDStorageQueueX)) {
            *ppvObject = static_cast<IDStorageQueueX*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    void STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        if (!request) {
            return;
        }
        QueuedOp op{};
        op.kind = QueueOpKind::ReadRequest;
        op.options = request->Options;
        op.dstMemory = request->Destination.Memory.Buffer;
        op.dstSize = request->Destination.Memory.Size;
        op.reqName = request->Name ? request->Name : "request";

        if (request->Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            op.fileOffset = request->Source.File.Offset;
            op.srcSize = request->Source.File.Size;
            if (request->Source.File.Source) {
                IDStorageFileX* rawInternal = nullptr;
                if (SUCCEEDED(request->Source.File.Source->QueryInterface(
                        IID_ScarlettFileInternal, reinterpret_cast<void**>(&rawInternal)))) {
                    op.file.Attach(static_cast<ScarlettStorageFileX*>(rawInternal));
                }
            }
        } else {
            op.srcMemory = request->Source.Memory.Source;
            op.srcSize = request->Source.Memory.Size;
        }

        std::lock_guard<std::mutex> lock(m_mtx);
        m_staged.push_back(std::move(op));
    }

    void STDMETHODCALLTYPE EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) override {
        if (!statusArray) {
            return;
        }
        IDStorageStatusArrayX* rawInternal = nullptr;
        if (FAILED(statusArray->QueryInterface(
                IID_ScarlettStatusInternal, reinterpret_cast<void**>(&rawInternal)))) {
            return;
        }

        QueuedOp op{};
        op.kind = QueueOpKind::StatusUpdate;
        op.statusArray.Attach(static_cast<ScarlettStatusArrayX*>(rawInternal));
        op.statusIndex = index;
        op.statusArray->ResetSlot(index);

        std::lock_guard<std::mutex> lock(m_mtx);
        m_staged.push_back(std::move(op));
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, uint64_t value) override {
        if (!fence) {
            return;
        }
        QueuedOp op{};
        op.kind = QueueOpKind::FenceSignal;
        op.fence = fence;
        op.fenceValue = value;

        std::lock_guard<std::mutex> lock(m_mtx);
        m_staged.push_back(std::move(op));
    }

    void STDMETHODCALLTYPE Submit() override {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            if (m_staged.empty()) {
                return;
            }
            m_batches.push_back(std::move(m_staged));
            m_staged.clear();
        }
        m_cv.notify_one();
    }

    void STDMETHODCALLTYPE RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        if (!record) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_errMtx);
        *record = m_errorRecord;
        std::memset(&m_errorRecord, 0, sizeof(m_errorRecord));
    }

    uint32_t STDMETHODCALLTYPE GetPendingRequestCount() override {
        std::lock_guard<std::mutex> lock(m_mtx);
        size_t total = m_staged.size();
        for (const auto& batch : m_batches) {
            total += batch.size();
        }
        return static_cast<uint32_t>(total);
    }

private:
    void RecordError(HRESULT hr, const std::string& reqName) {
        std::lock_guard<std::mutex> lock(m_errMtx);
        if (m_errorRecord.FailureCount == 0) {
            m_errorRecord.FirstFailureHResult = hr;
            std::strncpy(m_errorRecord.RequestName, reqName.c_str(),
                         sizeof(m_errorRecord.RequestName) - 1);
            m_errorRecord.RequestName[sizeof(m_errorRecord.RequestName) - 1] = '\0';
        }
        ++m_errorRecord.FailureCount;
    }

    HRESULT ExecuteReadRequest(const QueuedOp& op, uint64_t& outBytes) {
        outBytes = 0;
        if (op.options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE) {
            return E_NOTIMPL;
        }
        if (op.options.DestinationType != DSTORAGE_REQUEST_DESTINATION_MEMORY || !op.dstMemory) {
            return E_INVALIDARG;
        }
        if (op.srcSize > op.dstSize) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }

        if (op.options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!op.file) {
                return E_HANDLE;
            }
            uint32_t bytesRead = 0;
            HRESULT hr = op.file->ReadSlice(op.fileOffset, op.dstMemory, op.srcSize, bytesRead);
            outBytes = bytesRead;
            return hr;
        }

        if (!op.srcMemory) {
            return E_INVALIDARG;
        }
        std::memcpy(op.dstMemory, op.srcMemory, op.srcSize);
        outBytes = op.srcSize;
        return S_OK;
    }

    void WorkerLoop() {
        while (true) {
            std::vector<QueuedOp> batch;
            {
                std::unique_lock<std::mutex> lock(m_mtx);
                m_cv.wait(lock, [this]() { return m_stopping || !m_batches.empty(); });
                if (m_stopping && m_batches.empty()) {
                    return;
                }
                batch = std::move(m_batches.front());
                m_batches.pop_front();
            }

            const auto startTime = std::chrono::steady_clock::now();
            HRESULT batchHr = S_OK;
            uint64_t totalBytes = 0;
            uint32_t readCount = 0;

            for (const auto& op : batch) {
                switch (op.kind) {
                    case QueueOpKind::ReadRequest: {
                        uint64_t bytes = 0;
                        HRESULT hr = ExecuteReadRequest(op, bytes);
                        totalBytes += bytes;
                        ++readCount;
                        if (FAILED(hr)) {
                            if (SUCCEEDED(batchHr)) {
                                batchHr = hr;
                            }
                            RecordError(hr, op.reqName);
                        }
                        break;
                    }
                    case QueueOpKind::StatusUpdate: {
                        if (op.statusArray) {
                            op.statusArray->CompleteSlot(op.statusIndex, batchHr);
                        }
                        batchHr = S_OK;
                        break;
                    }
                    case QueueOpKind::FenceSignal: {
                        if (op.fence) {
                            op.fence->Signal(op.fenceValue);
                        }
                        break;
                    }
                }
            }

            if (readCount > 0) {
                const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                const double sec = std::max(static_cast<double>(elapsedUs) / 1000000.0, 0.000001);
                const double mbps = (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / sec;

                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "-> batch done on '%s': %u req(s), %llu B in %.2f ms (%.1f MB/s)",
                              m_name.c_str(), readCount,
                              static_cast<unsigned long long>(totalBytes),
                              static_cast<double>(elapsedUs) / 1000.0, mbps);
                sc::log::Write(sc::log::Level::Info, "dstorage_xs", "Worker[Submit]", buf);
            }
        }
    }

    std::atomic<ULONG> m_refs{1};
    DSTORAGE_QUEUE_DESC m_desc{};
    std::string m_name;

    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::vector<QueuedOp> m_staged;
    std::deque<std::vector<QueuedOp>> m_batches;
    bool m_stopping{false};
    std::thread m_worker;

    std::mutex m_errMtx;
    DSTORAGE_ERROR_RECORD m_errorRecord{};
};

class ScarlettStorageFactoryX final : public IDStorageFactoryX {
public:
    ScarlettStorageFactoryX() {
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "ScarlettStorageFactoryX",
                       "-> initialized Velocity async I/O factory");
    }

    ~ScarlettStorageFactoryX() {
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "~ScarlettStorageFactoryX",
                       "-> released factory");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IDStorageFactoryX)) {
            *ppvObject = static_cast<IDStorageFactoryX*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(
        const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) override {
        if (!desc || !ppv) {
            return E_INVALIDARG;
        }
        *ppv = nullptr;
        auto* queue = new ScarlettStorageQueueX(*desc);
        HRESULT hr = queue->QueryInterface(riid, ppv);
        queue->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE OpenFile(
        const wchar_t* path, REFIID riid, void** ppv) override {
        if (!path || !*path || !ppv) {
            return E_INVALIDARG;
        }
        *ppv = nullptr;

        HANDLE hFile = CreateFileW(
            path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        std::string utf8Path = WideToUtf8(path);
        std::string msg = "-> opened asset: " + utf8Path;
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "OpenFile", msg.c_str());

        auto* fileObj = new ScarlettStorageFileX(hFile, std::move(utf8Path));
        HRESULT hr = fileObj->QueryInterface(riid, ppv);
        fileObj->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(
        uint32_t capacity, const char* name, REFIID riid, void** ppv) override {
        if (capacity == 0 || !ppv) {
            return E_INVALIDARG;
        }
        *ppv = nullptr;
        auto* arr = new ScarlettStatusArrayX(capacity, name ? name : "status");
        HRESULT hr = arr->QueryInterface(riid, ppv);
        arr->Release();
        return hr;
    }

    void STDMETHODCALLTYPE SetDebugFlags(uint32_t flags) override {
        m_debugFlags = flags;
    }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(uint32_t size) override {
        m_stagingSize = size;
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    uint32_t m_debugFlags{0};
    uint32_t m_stagingSize{32u * 1024u * 1024u};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "DllMain", path);
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    auto* factory = new ScarlettStorageFactoryX();
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT WINAPI DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* configuration) {
    if (!configuration) {
        return E_INVALIDARG;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "-> submitThreads=%u, decompThreads=%d",
                  configuration->NumSubmitThreads,
                  configuration->NumBuiltInCpuDecompressionThreads);
    sc::log::Write(sc::log::Level::Info, "dstorage_xs", __FUNCTION__, buf);
    return S_OK;
}

} // extern "C"