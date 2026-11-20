#include "dstorage_xs.hpp"
#include "common/codecs.hpp"
#include "common/stub.hpp"
#include "common/vfs.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

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

    DSTORAGE_REQUEST_OPTIONS options{};
    ComPtr<ScarlettStorageFileX> file;
    uint64_t fileOffset{0};
    const void* srcMemory{nullptr};
    uint32_t srcSize{0};
    void* dstMemory{nullptr};
    uint32_t dstSize{0};
    ComPtr<ID3D12Resource> dstBuffer;
    uint64_t dstBufferOffset{0};
    uint32_t dstBufferSize{0};
    uint32_t uncompressedSize{0};
    std::string reqName;

    ComPtr<ScarlettStatusArrayX> statusArray;
    uint32_t statusIndex{0};

    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue{0};
};

class ScarlettStorageQueueX final : public IDStorageQueueX {
public:
    ScarlettStorageQueueX(const DSTORAGE_QUEUE_DESC& desc, uint32_t stagingSize)
        : m_desc(desc)
        , m_name(desc.Name ? desc.Name : "unnamed_queue")
        , m_stagingSize(stagingSize > 0 ? stagingSize : (4u * 1024u * 1024u)) {
        if (desc.Device) {
            auto* unk = static_cast<IUnknown*>(desc.Device);
            unk->QueryInterface(__uuidof(ID3D12Device),
                                reinterpret_cast<void**>(m_device.GetAddressOf()));
        }
        m_worker = std::thread([this]() { WorkerLoop(); });
        char buf[192];
        std::snprintf(buf, sizeof(buf), "-> created queue '%s' (capacity=%u, priority=%d, gpu=%s)",
                      m_name.c_str(), static_cast<unsigned>(m_desc.Capacity),
                      static_cast<int>(m_desc.Priority),
                      m_device ? "yes" : "no");
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
        ReleaseGpuCopyContext();
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
        op.uncompressedSize = request->UncompressedSize;
        op.reqName = request->Name ? request->Name : "request";

        if (request->Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
            op.dstBuffer = request->Destination.Buffer.Resource;
            op.dstBufferOffset = request->Destination.Buffer.Offset;
            op.dstBufferSize = request->Destination.Buffer.Size;
        } else {
            op.dstMemory = request->Destination.Memory.Buffer;
            op.dstSize = request->Destination.Memory.Size;
        }

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
    void ReleaseGpuCopyContext() {
        if (m_stagingBuffer && m_stagingMapped) {
            D3D12_RANGE empty{ 0, 0 };
            m_stagingBuffer->Unmap(0, &empty);
            m_stagingMapped = nullptr;
        }
        m_stagingCapacity = 0;
        m_stagingBuffer.Reset();
        m_copyList.Reset();
        m_copyAllocator.Reset();
        m_copyFence.Reset();
        m_copyQueue.Reset();
        m_device.Reset();
        if (m_copyEvent) {
            CloseHandle(m_copyEvent);
            m_copyEvent = nullptr;
        }
    }

    HRESULT EnsureGpuCopyContext(uint64_t requiredBytes) {
        if (!m_device) {
            return E_INVALIDARG;
        }
        if (!m_copyQueue) {
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            HRESULT hr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_copyQueue));
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_copyAllocator));
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_COPY, m_copyAllocator.Get(), nullptr,
                IID_PPV_ARGS(&m_copyList));
            if (FAILED(hr)) {
                return hr;
            }
            hr = m_copyList->Close();
            if (FAILED(hr)) {
                return hr;
            }

            hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence));
            if (FAILED(hr)) {
                return hr;
            }

            m_copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!m_copyEvent) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }

        if (!m_stagingBuffer || m_stagingCapacity < requiredBytes) {
            if (m_stagingBuffer && m_stagingMapped) {
                D3D12_RANGE empty{ 0, 0 };
                m_stagingBuffer->Unmap(0, &empty);
                m_stagingMapped = nullptr;
            }
            m_stagingBuffer.Reset();
            m_stagingCapacity = 0;

            const uint64_t defaultCap = std::min<uint32_t>(m_stagingSize, 4u * 1024u * 1024u);
            const uint64_t allocSize = std::max<uint64_t>(requiredBytes, std::max<uint64_t>(defaultCap, 65536u));

            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = allocSize;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            HRESULT hr = m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&m_stagingBuffer));
            if (FAILED(hr)) {
                return hr;
            }

            D3D12_RANGE readRange{ 0, 0 };
            void* mapped = nullptr;
            hr = m_stagingBuffer->Map(0, &readRange, &mapped);
            if (FAILED(hr)) {
                m_stagingBuffer.Reset();
                return hr;
            }
            m_stagingMapped = static_cast<uint8_t*>(mapped);
            m_stagingCapacity = allocSize;
        }
        return S_OK;
    }

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

    HRESULT ReadRawSource(const QueuedOp& op, void* dst, uint32_t size, uint32_t& outRead) {
        outRead = 0;
        if (op.options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!op.file) {
                return E_HANDLE;
            }
            return op.file->ReadSlice(op.fileOffset, dst, size, outRead);
        }
        if (!op.srcMemory) {
            return E_INVALIDARG;
        }
        std::memcpy(dst, op.srcMemory, size);
        outRead = size;
        return S_OK;
    }

    HRESULT ExecuteBufferRequest(const QueuedOp& op, uint64_t& outReadBytes, uint64_t& outProducedBytes) {
        if (!m_device || !op.dstBuffer || op.dstBufferSize == 0) {
            return E_INVALIDARG;
        }

        uint32_t bytesToCopy = 0;

        if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_NONE) {
            if (op.srcSize > op.dstBufferSize) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            HRESULT hr = EnsureGpuCopyContext(op.srcSize);
            if (FAILED(hr)) {
                return hr;
            }
            uint32_t bytesRead = 0;
            hr = ReadRawSource(op, m_stagingMapped, op.srcSize, bytesRead);
            if (FAILED(hr)) {
                return hr;
            }
            bytesToCopy = bytesRead;
            outReadBytes = bytesRead;
            outProducedBytes = bytesRead;
        } else if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ) {
            const uint32_t expectedOut = (op.uncompressedSize > 0) ? op.uncompressedSize : op.dstBufferSize;
            if (expectedOut > op.dstBufferSize) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            HRESULT hr = EnsureGpuCopyContext(expectedOut);
            if (FAILED(hr)) {
                return hr;
            }
            std::vector<uint8_t> compressedStaging(op.srcSize);
            uint32_t bytesRead = 0;
            hr = ReadRawSource(op, compressedStaging.data(), op.srcSize, bytesRead);
            if (FAILED(hr)) {
                return hr;
            }
            outReadBytes = bytesRead;

            size_t decompressedBytes = 0;
            if (!sc::codec::DecompressLzBlock(
                    compressedStaging.data(), bytesRead,
                    m_stagingMapped, expectedOut, decompressedBytes) ||
                decompressedBytes != expectedOut) {
                return E_FAIL;
            }
            bytesToCopy = static_cast<uint32_t>(decompressedBytes);
            outProducedBytes = decompressedBytes;
        } else {
            return E_NOTIMPL;
        }

        HRESULT hr = m_copyAllocator->Reset();
        if (FAILED(hr)) {
            return hr;
        }
        hr = m_copyList->Reset(m_copyAllocator.Get(), nullptr);
        if (FAILED(hr)) {
            return hr;
        }

        m_copyList->CopyBufferRegion(
            op.dstBuffer.Get(), op.dstBufferOffset,
            m_stagingBuffer.Get(), 0, bytesToCopy);

        hr = m_copyList->Close();
        if (FAILED(hr)) {
            return hr;
        }

        ID3D12CommandList* lists[] = { m_copyList.Get() };
        m_copyQueue->ExecuteCommandLists(1, lists);

        ++m_copyFenceValue;
        hr = m_copyQueue->Signal(m_copyFence.Get(), m_copyFenceValue);
        if (FAILED(hr)) {
            return hr;
        }
        if (m_copyFence->GetCompletedValue() < m_copyFenceValue) {
            hr = m_copyFence->SetEventOnCompletion(m_copyFenceValue, m_copyEvent);
            if (FAILED(hr)) {
                return hr;
            }
            WaitForSingleObject(m_copyEvent, INFINITE);
        }
        return S_OK;
    }

    HRESULT ExecuteReadRequest(const QueuedOp& op, uint64_t& outReadBytes, uint64_t& outProducedBytes) {
        outReadBytes = 0;
        outProducedBytes = 0;

        if (op.options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
            return ExecuteBufferRequest(op, outReadBytes, outProducedBytes);
        }

        if (op.options.DestinationType != DSTORAGE_REQUEST_DESTINATION_MEMORY || !op.dstMemory) {
            return E_INVALIDARG;
        }

        if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_NONE) {
            if (op.srcSize > op.dstSize) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            uint32_t bytesRead = 0;
            HRESULT hr = ReadRawSource(op, op.dstMemory, op.srcSize, bytesRead);
            outReadBytes = bytesRead;
            outProducedBytes = bytesRead;
            return hr;
        }

        if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ) {
            const uint32_t expectedOut = (op.uncompressedSize > 0) ? op.uncompressedSize : op.dstSize;
            if (expectedOut > op.dstSize) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }

            std::vector<uint8_t> compressedStaging(op.srcSize);
            uint32_t bytesRead = 0;
            HRESULT hr = ReadRawSource(op, compressedStaging.data(), op.srcSize, bytesRead);
            if (FAILED(hr)) {
                return hr;
            }
            outReadBytes = bytesRead;

            size_t decompressedBytes = 0;
            if (!sc::codec::DecompressLzBlock(
                    compressedStaging.data(), bytesRead,
                    op.dstMemory, op.dstSize, decompressedBytes) ||
                decompressedBytes != expectedOut) {
                return E_FAIL;
            }

            outProducedBytes = decompressedBytes;
            return S_OK;
        }

        return E_NOTIMPL;
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
            uint64_t totalRead = 0;
            uint64_t totalProduced = 0;
            uint32_t readCount = 0;
            uint32_t gpuReadCount = 0;

            for (const auto& op : batch) {
                switch (op.kind) {
                    case QueueOpKind::ReadRequest: {
                        uint64_t bytesRead = 0;
                        uint64_t bytesProduced = 0;
                        HRESULT hr = ExecuteReadRequest(op, bytesRead, bytesProduced);
                        totalRead += bytesRead;
                        totalProduced += bytesProduced;
                        ++readCount;
                        if (op.options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
                            ++gpuReadCount;
                        }
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
                const double effMbps = (static_cast<double>(totalProduced) / (1024.0 * 1024.0)) / sec;

                char buf[256];
                if (totalProduced > totalRead && totalRead > 0) {
                    const double ratio = static_cast<double>(totalProduced) / static_cast<double>(totalRead);
                    std::snprintf(buf, sizeof(buf),
                                  "-> batch done on '%s': %u req(s) (%u GPU), %llu B -> %llu B decompressed (%.2fx) in %.2f ms (%.1f MB/s eff)",
                                  m_name.c_str(), readCount, gpuReadCount,
                                  static_cast<unsigned long long>(totalRead),
                                  static_cast<unsigned long long>(totalProduced),
                                  ratio, static_cast<double>(elapsedUs) / 1000.0, effMbps);
                } else {
                    std::snprintf(buf, sizeof(buf),
                                  "-> batch done on '%s': %u req(s) (%u GPU), %llu B in %.2f ms (%.1f MB/s)",
                                  m_name.c_str(), readCount, gpuReadCount,
                                  static_cast<unsigned long long>(totalProduced),
                                  static_cast<double>(elapsedUs) / 1000.0, effMbps);
                }
                sc::log::Write(sc::log::Level::Info, "dstorage_xs", "Worker[Submit]", buf);
            }
        }
    }

    std::atomic<ULONG> m_refs{1};
    DSTORAGE_QUEUE_DESC m_desc{};
    std::string m_name;
    uint32_t m_stagingSize{4u * 1024u * 1024u};

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_copyQueue;
    ComPtr<ID3D12CommandAllocator> m_copyAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_copyList;
    ComPtr<ID3D12Fence> m_copyFence;
    uint64_t m_copyFenceValue{0};
    HANDLE m_copyEvent{nullptr};
    ComPtr<ID3D12Resource> m_stagingBuffer;
    uint8_t* m_stagingMapped{nullptr};
    uint64_t m_stagingCapacity{0};

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
        auto* queue = new ScarlettStorageQueueX(*desc, m_stagingSize);
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

        bool wasVirtual = false;
        const std::filesystem::path hostPath = sc::vfs::TranslatePath(path, &wasVirtual);

        HANDLE hFile = CreateFileW(
            hostPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        std::string utf8Host = WideToUtf8(hostPath.c_str());
        std::string msg = wasVirtual
            ? ("-> opened asset [VFS]: " + WideToUtf8(path) + " -> " + utf8Host)
            : ("-> opened asset: " + utf8Host);
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "OpenFile", msg.c_str());

        auto* fileObj = new ScarlettStorageFileX(hFile, std::move(utf8Host));
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