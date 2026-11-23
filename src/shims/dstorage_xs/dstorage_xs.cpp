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

constexpr GUID IID_ScarlettNativeD3D12Device =
    { 0x5c4a12F0, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0xF0 } };

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
    ComPtr<ID3D12Resource> dstTexture;
    uint32_t dstSubresourceIndex{0};
    D3D12_BOX dstRegion{};
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
        const char* gpuMode = "no";
        if (desc.Device) {
            auto* unk = static_cast<IUnknown*>(desc.Device);
            if (SUCCEEDED(unk->QueryInterface(
                    IID_ScarlettNativeD3D12Device,
                    reinterpret_cast<void**>(m_device.GetAddressOf())))) {
                gpuMode = "yes(d3d12_xs)";
            } else if (SUCCEEDED(unk->QueryInterface(
                    __uuidof(ID3D12Device),
                    reinterpret_cast<void**>(m_device.GetAddressOf())))) {
                gpuMode = "yes(native)";
            }
        }
        m_worker = std::thread([this]() { WorkerLoop(); });
        char buf[192];
        std::snprintf(buf, sizeof(buf), "-> created queue '%s' (capacity=%u, priority=%d, gpu=%s)",
                      m_name.c_str(), static_cast<unsigned>(m_desc.Capacity),
                      static_cast<int>(m_desc.Priority),
                      gpuMode);
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
        } else if (request->Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION) {
            op.dstTexture = request->Destination.Texture.Resource;
            op.dstSubresourceIndex = request->Destination.Texture.SubresourceIndex;
            op.dstRegion = request->Destination.Texture.Region;
        } else if (request->Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES) {
            op.dstTexture = request->Destination.MultipleSubresources.Resource;
            op.dstSubresourceIndex = request->Destination.MultipleSubresources.FirstSubresource;
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

    HRESULT ExecuteTextureRequest(const QueuedOp& op, uint64_t& outReadBytes, uint64_t& outProducedBytes) {
        if (!m_device || !op.dstTexture ||
            op.dstRegion.right <= op.dstRegion.left ||
            op.dstRegion.bottom <= op.dstRegion.top ||
            op.dstRegion.back <= op.dstRegion.front) {
            return E_INVALIDARG;
        }

        const D3D12_RESOURCE_DESC texDesc = op.dstTexture->GetDesc();
        const uint32_t regW = op.dstRegion.right - op.dstRegion.left;
        const uint32_t regH = op.dstRegion.bottom - op.dstRegion.top;
        const uint32_t regD = op.dstRegion.back - op.dstRegion.front;

        D3D12_RESOURCE_DESC regionDesc = texDesc;
        regionDesc.Width = regW;
        regionDesc.Height = regH;
        regionDesc.DepthOrArraySize = static_cast<UINT16>(regD);
        regionDesc.MipLevels = 1;
        regionDesc.Alignment = 0;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalStagingBytes = 0;
        m_device->GetCopyableFootprints(
            &regionDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalStagingBytes);

        if (numRows == 0 || rowSizeInBytes == 0 || totalStagingBytes == 0) {
            return E_INVALIDARG;
        }

        const uint64_t tightBytes = rowSizeInBytes * static_cast<uint64_t>(numRows) * static_cast<uint64_t>(regD);
        HRESULT hr = EnsureGpuCopyContext(totalStagingBytes);
        if (FAILED(hr)) {
            return hr;
        }

        std::vector<uint8_t> decoded;
        const uint8_t* srcPixels = nullptr;

        if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_NONE) {
            if (op.srcSize < tightBytes) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            if (footprint.Footprint.RowPitch == rowSizeInBytes) {
                uint32_t bytesRead = 0;
                hr = ReadRawSource(op, m_stagingMapped, static_cast<uint32_t>(tightBytes), bytesRead);
                if (FAILED(hr)) {
                    return hr;
                }
                outReadBytes = bytesRead;
                outProducedBytes = bytesRead;
            } else {
                decoded.resize(static_cast<size_t>(tightBytes));
                uint32_t bytesRead = 0;
                hr = ReadRawSource(op, decoded.data(), static_cast<uint32_t>(tightBytes), bytesRead);
                if (FAILED(hr)) {
                    return hr;
                }
                outReadBytes = bytesRead;
                outProducedBytes = bytesRead;
                srcPixels = decoded.data();
            }
        } else if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ) {
            const uint64_t expectedOut = (op.uncompressedSize > 0) ? op.uncompressedSize : tightBytes;
            if (expectedOut < tightBytes) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            std::vector<uint8_t> compressedStaging(op.srcSize);
            uint32_t bytesRead = 0;
            hr = ReadRawSource(op, compressedStaging.data(), op.srcSize, bytesRead);
            if (FAILED(hr)) {
                return hr;
            }
            outReadBytes = bytesRead;

            size_t decompressedBytes = 0;
            if (footprint.Footprint.RowPitch == rowSizeInBytes && expectedOut == tightBytes) {
                if (!sc::codec::DecompressLzBlock(
                        compressedStaging.data(), bytesRead,
                        m_stagingMapped, static_cast<size_t>(expectedOut), decompressedBytes) ||
                    decompressedBytes != expectedOut) {
                    return E_FAIL;
                }
            } else {
                decoded.resize(static_cast<size_t>(expectedOut));
                if (!sc::codec::DecompressLzBlock(
                        compressedStaging.data(), bytesRead,
                        decoded.data(), decoded.size(), decompressedBytes) ||
                    decompressedBytes != expectedOut) {
                    return E_FAIL;
                }
                srcPixels = decoded.data();
            }
            outProducedBytes = decompressedBytes;
        } else {
            return E_NOTIMPL;
        }

        if (srcPixels) {
            const uint64_t dstSlicePitch = static_cast<uint64_t>(footprint.Footprint.RowPitch) * numRows;
            const uint64_t srcSlicePitch = rowSizeInBytes * numRows;
            for (uint32_t z = 0; z < regD; ++z) {
                for (UINT y = 0; y < numRows; ++y) {
                    std::memcpy(
                        m_stagingMapped + z * dstSlicePitch + static_cast<uint64_t>(y) * footprint.Footprint.RowPitch,
                        srcPixels + z * srcSlicePitch + static_cast<uint64_t>(y) * rowSizeInBytes,
                        static_cast<size_t>(rowSizeInBytes));
                }
            }
        }

        hr = m_copyAllocator->Reset();
        if (FAILED(hr)) {
            return hr;
        }
        hr = m_copyList->Reset(m_copyAllocator.Get(), nullptr);
        if (FAILED(hr)) {
            return hr;
        }

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = op.dstTexture.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = op.dstSubresourceIndex;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = m_stagingBuffer.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;

        m_copyList->CopyTextureRegion(
            &dstLoc, op.dstRegion.left, op.dstRegion.top, op.dstRegion.front, &srcLoc, nullptr);

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

    HRESULT ExecuteMultipleSubresourcesRequest(
        const QueuedOp& op, uint64_t& outReadBytes, uint64_t& outProducedBytes) {
        if (!m_device || !op.dstTexture) {
            return E_INVALIDARG;
        }

        const D3D12_RESOURCE_DESC texDesc = op.dstTexture->GetDesc();
        const uint32_t arrayCount = (texDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            ? 1u : static_cast<uint32_t>(texDesc.DepthOrArraySize);
        const uint32_t totalSubresources = static_cast<uint32_t>(texDesc.MipLevels) * arrayCount;
        if (totalSubresources == 0 || op.dstSubresourceIndex >= totalSubresources) {
            return E_INVALIDARG;
        }

        const uint32_t numSubresources = totalSubresources - op.dstSubresourceIndex;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(numSubresources);
        std::vector<UINT> numRows(numSubresources);
        std::vector<UINT64> rowSizes(numSubresources);
        UINT64 totalStagingBytes = 0;

        m_device->GetCopyableFootprints(
            &texDesc, op.dstSubresourceIndex, numSubresources, 0,
            footprints.data(), numRows.data(), rowSizes.data(), &totalStagingBytes);

        if (totalStagingBytes == 0) {
            return E_INVALIDARG;
        }

        uint64_t totalTightBytes = 0;
        for (uint32_t i = 0; i < numSubresources; ++i) {
            if (numRows[i] == 0 || rowSizes[i] == 0) {
                return E_INVALIDARG;
            }
            totalTightBytes += rowSizes[i] * static_cast<uint64_t>(numRows[i]) *
                               static_cast<uint64_t>(footprints[i].Footprint.Depth);
        }

        HRESULT hr = EnsureGpuCopyContext(totalStagingBytes);
        if (FAILED(hr)) {
            return hr;
        }

        std::vector<uint8_t> tightData;
        if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_NONE) {
            if (op.srcSize < totalTightBytes) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            tightData.resize(static_cast<size_t>(totalTightBytes));
            uint32_t bytesRead = 0;
            hr = ReadRawSource(op, tightData.data(), static_cast<uint32_t>(totalTightBytes), bytesRead);
            if (FAILED(hr)) {
                return hr;
            }
            outReadBytes = bytesRead;
            outProducedBytes = bytesRead;
        } else if (op.options.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ) {
            const uint64_t expectedOut = (op.uncompressedSize > 0) ? op.uncompressedSize : totalTightBytes;
            if (expectedOut < totalTightBytes) {
                return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            }
            std::vector<uint8_t> compressedStaging(op.srcSize);
            uint32_t bytesRead = 0;
            hr = ReadRawSource(op, compressedStaging.data(), op.srcSize, bytesRead);
            if (FAILED(hr)) {
                return hr;
            }
            outReadBytes = bytesRead;

            tightData.resize(static_cast<size_t>(expectedOut));
            size_t decompressedBytes = 0;
            if (!sc::codec::DecompressLzBlock(
                    compressedStaging.data(), bytesRead,
                    tightData.data(), tightData.size(), decompressedBytes) ||
                decompressedBytes != expectedOut) {
                return E_FAIL;
            }
            outProducedBytes = decompressedBytes;
        } else {
            return E_NOTIMPL;
        }

        uint64_t srcOffset = 0;
        for (uint32_t i = 0; i < numSubresources; ++i) {
            const uint32_t depth = footprints[i].Footprint.Depth;
            const uint64_t dstRowPitch = footprints[i].Footprint.RowPitch;
            const uint64_t srcRowSize = rowSizes[i];
            const uint64_t dstSlicePitch = dstRowPitch * static_cast<uint64_t>(numRows[i]);
            uint8_t* dstBase = m_stagingMapped + footprints[i].Offset;

            for (uint32_t z = 0; z < depth; ++z) {
                for (UINT y = 0; y < numRows[i]; ++y) {
                    std::memcpy(
                        dstBase + z * dstSlicePitch + static_cast<uint64_t>(y) * dstRowPitch,
                        tightData.data() + srcOffset,
                        static_cast<size_t>(srcRowSize));
                    srcOffset += srcRowSize;
                }
            }
        }

        hr = m_copyAllocator->Reset();
        if (FAILED(hr)) {
            return hr;
        }
        hr = m_copyList->Reset(m_copyAllocator.Get(), nullptr);
        if (FAILED(hr)) {
            return hr;
        }

        for (uint32_t i = 0; i < numSubresources; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = op.dstTexture.Get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = op.dstSubresourceIndex + i;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = m_stagingBuffer.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = footprints[i];

            m_copyList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }

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
        if (op.options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION) {
            return ExecuteTextureRequest(op, outReadBytes, outProducedBytes);
        }
        if (op.options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES) {
            return ExecuteMultipleSubresourcesRequest(op, outReadBytes, outProducedBytes);
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
                        if (op.options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER ||
                            op.options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION ||
                            op.options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES) {
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


namespace gdk_ds {

inline constexpr GUID IID_GdkFactoryX2 =
    { 0x62959d11, 0xd45f, 0x4884, { 0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae } };
inline constexpr GUID IID_GdkFileX =
    { 0x43704f9f, 0x27c6, 0x4a9f, { 0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4 } };
inline constexpr GUID IID_GdkStatusArrayX =
    { 0xcd66d752, 0x2786, 0x44f7, { 0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b } };
inline constexpr GUID IID_GdkQueueX2 =
    { 0x3c02cb0c, 0xc050, 0x4195, { 0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5 } };
inline constexpr GUID IID_GdkFileInternal =
    { 0x5c4a42f1, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0xf1 } };
inline constexpr GUID IID_GdkStatusInternal =
    { 0x5c4a42f2, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0xf2 } };

struct GdkQueueDesc {
    uint64_t    SourceType;
    uint16_t    Capacity;
    int8_t      Priority;
    uint8_t     Pad0[5];
    const char* Name;
};
static_assert(sizeof(GdkQueueDesc) == 0x18, "GdkQueueDesc ABI mismatch");

struct GdkQueueDesc1 {
    uint64_t    SourceType;
    uint16_t    Capacity;
    int8_t      Priority;
    uint8_t     Pad0[5];
    const char* Name;
    uint64_t    Options;
};
static_assert(sizeof(GdkQueueDesc1) == 0x20, "GdkQueueDesc1 ABI mismatch");

struct GdkQueueInfo {
    GdkQueueDesc Desc;
    uint16_t     EmptySlotCount;
    uint16_t     RequestCountUntilAutoSubmit;
    uint32_t     Pad;
};

struct GdkQueueInfo1 {
    GdkQueueDesc1 Desc;
    uint16_t      EmptySlotCount;
    uint16_t      RequestCountUntilAutoSubmit;
    uint32_t      Pad;
};

struct GdkRequest {
    uint64_t Options;
    union {
        void* Destination;
        struct {
            uintptr_t* DestinationPageArray;
            uint16_t   DestinationPageOffset;
        };
    };
    uint32_t DestinationSize;
    uint32_t Pad0;
    union {
        struct {
            IUnknown* File;
            uint64_t  FileOffset;
        };
        const void* Source;
        struct {
            uintptr_t* SourcePageArray;
            uint16_t   SourcePageOffset;
        };
    };
    uint32_t    SourceSize;
    uint32_t    IntermediateSize;
    uint64_t    CancellationTag;
    const char* Name;
};
static_assert(sizeof(GdkRequest) == 0x48, "GdkRequest ABI mismatch");

struct GdkErrorRecord {
    uint32_t   FailureCount;
    uint32_t   Pad0;
    uint32_t   CommandType;
    HRESULT    HResult;
    GdkRequest FailedRequest;
};

class GdkStorageFileX final : public IUnknown {
public:
    GdkStorageFileX(HANDLE h, std::string path)
        : m_handle(h), m_path(std::move(path)) {}

    ~GdkStorageFileX() {
        Close();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, IID_GdkFileX) ||
            IsEqualGUID(riid, IID_GdkFileInternal)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    // slot 3 (+0x18)
    virtual void STDMETHODCALLTYPE Close() {
        std::lock_guard<std::mutex> lock(m_ioMtx);
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    // slot 4 (+0x20) — caller owns duplicated HANDLE and closes it via CloseHandle()
    virtual HANDLE STDMETHODCALLTYPE GetHandle() {
        std::lock_guard<std::mutex> lock(m_ioMtx);
        if (m_handle == INVALID_HANDLE_VALUE) {
            return INVALID_HANDLE_VALUE;
        }
        HANDLE dup = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(GetCurrentProcess(), m_handle, GetCurrentProcess(), &dup,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return INVALID_HANDLE_VALUE;
        }
        return dup;
    }

    HRESULT ReadSlice(uint64_t offset, void* dst, uint32_t size, uint32_t& outRead) {
        outRead = 0;
        if (!dst && size > 0) return E_INVALIDARG;
        std::lock_guard<std::mutex> lock(m_ioMtx);
        if (m_handle == INVALID_HANDLE_VALUE) return E_HANDLE;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(m_handle, pos, nullptr, FILE_BEGIN)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        auto* cursor = static_cast<uint8_t*>(dst);
        uint32_t rem = size;
        while (rem > 0) {
            DWORD got = 0;
            if (!ReadFile(m_handle, cursor, rem, &got, nullptr)) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (got == 0) {
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            }
            cursor += got;
            rem -= got;
            outRead += got;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::mutex         m_ioMtx;
    HANDLE             m_handle{INVALID_HANDLE_VALUE};
    std::string        m_path;
};

class GdkStatusArrayX final : public IUnknown {
public:
    explicit GdkStatusArrayX(uint32_t capacity) : m_slots(capacity) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, IID_GdkStatusArrayX) ||
            IsEqualGUID(riid, IID_GdkStatusInternal)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    // slot 3 (+0x18)
    virtual bool STDMETHODCALLTYPE IsComplete(uint32_t index) {
        std::lock_guard<std::mutex> lock(m_mtx);
        return index < m_slots.size() ? m_slots[index].complete : false;
    }

    // slot 4 (+0x20)
    virtual HRESULT STDMETHODCALLTYPE GetHResult(uint32_t index) {
        std::lock_guard<std::mutex> lock(m_mtx);
        return index < m_slots.size() ? m_slots[index].hr : E_INVALIDARG;
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
        bool    complete{true};
        HRESULT hr{S_OK};
    };
    std::atomic<ULONG> m_refs{1};
    std::mutex         m_mtx;
    std::vector<Slot>  m_slots;
};

class GdkStorageQueueX2 final : public IUnknown {
public:
    explicit GdkStorageQueueX2(const GdkQueueDesc1& desc)
        : m_desc(desc)
        , m_name(desc.Name ? desc.Name : "gdk_queue") {
        m_desc.Name = m_name.c_str();
        m_errEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_worker = std::thread([this]() { WorkerLoop(); });
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "-> [GDK] created queue '%s' (src=%u cap=%u prio=%d opts=0x%llX)",
                      m_name.c_str(), static_cast<unsigned>(m_desc.SourceType),
                      static_cast<unsigned>(m_desc.Capacity), static_cast<int>(m_desc.Priority),
                      static_cast<unsigned long long>(m_desc.Options));
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "CreateQueue1", buf);
    }

    ~GdkStorageQueueX2() {
        Close();
        if (m_errEvent) {
            CloseHandle(m_errEvent);
            m_errEvent = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_GdkQueueX2)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        // Accept base IDStorageQueueX / X1 IIDs as well
        *ppv = this;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    // slot 3 (+0x18)
    virtual void STDMETHODCALLTYPE EnqueueRequest(const GdkRequest* req) {
        if (!req) return;
        Op op{};
        op.kind = OpKind::Read;
        op.req = *req;
        const uint64_t srcType = (req->Options >> 22) & 1ULL;
        if (srcType == 0 && req->File) {
            void* raw = nullptr;
            if (SUCCEEDED(req->File->QueryInterface(IID_GdkFileInternal, &raw))) {
                op.file.Attach(static_cast<GdkStorageFileX*>(raw));
            }
        }
        std::lock_guard<std::mutex> lock(m_mtx);
        m_staged.push_back(std::move(op));
    }

    // slot 4 (+0x20)
    virtual void STDMETHODCALLTYPE EnqueueStatus(IUnknown* statusArray, uint32_t index) {
        if (!statusArray) return;
        void* raw = nullptr;
        if (FAILED(statusArray->QueryInterface(IID_GdkStatusInternal, &raw))) return;
        Op op{};
        op.kind = OpKind::Status;
        op.statusArray.Attach(static_cast<GdkStatusArrayX*>(raw));
        op.statusIndex = index;
        op.statusArray->ResetSlot(index);
        std::lock_guard<std::mutex> lock(m_mtx);
        m_staged.push_back(std::move(op));
    }

    // slot 5 (+0x28)
    virtual void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, uint64_t value) {
        if (!fence) return;
        Op op{};
        op.kind = OpKind::Fence;
        op.fence = fence;
        op.fenceValue = value;
        std::lock_guard<std::mutex> lock(m_mtx);
        m_staged.push_back(std::move(op));
    }

    // slot 6 (+0x30)
    virtual void STDMETHODCALLTYPE Submit() {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            if (m_staged.empty()) return;
            m_batches.push_back(std::move(m_staged));
            m_staged.clear();
        }
        m_cv.notify_one();
    }

    // slot 7 (+0x38)
    virtual void STDMETHODCALLTYPE CancelRequestsWithTag(uint64_t /*mask*/, uint64_t /*value*/) {}

    // slot 8 (+0x40)
    virtual void STDMETHODCALLTYPE Close() {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    // slot 9 (+0x48)
    virtual HANDLE STDMETHODCALLTYPE GetErrorEvent() {
        HANDLE dup = nullptr;
        if (m_errEvent) {
            DuplicateHandle(GetCurrentProcess(), m_errEvent, GetCurrentProcess(), &dup,
                            0, FALSE, DUPLICATE_SAME_ACCESS);
        }
        return dup;
    }

    // slot 10 (+0x50)
    virtual void STDMETHODCALLTYPE RetrieveErrorRecord(GdkErrorRecord* record) {
        if (!record) return;
        std::lock_guard<std::mutex> lock(m_errMtx);
        *record = m_errRecord;
        std::memset(&m_errRecord, 0, sizeof(m_errRecord));
        if (m_errEvent) ResetEvent(m_errEvent);
    }

    // slot 11 (+0x58)
    virtual void STDMETHODCALLTYPE Query(GdkQueueInfo* info) {
        if (!info) return;
        std::memset(info, 0, sizeof(*info));
        info->Desc.SourceType = m_desc.SourceType;
        info->Desc.Capacity   = m_desc.Capacity;
        info->Desc.Priority   = m_desc.Priority;
        info->Desc.Name       = m_desc.Name;
        const uint32_t pend = PendingCount();
        info->EmptySlotCount = (m_desc.Capacity > pend)
            ? static_cast<uint16_t>(m_desc.Capacity - pend) : 0u;
        info->RequestCountUntilAutoSubmit = static_cast<uint16_t>(m_desc.Capacity / 2u);
    }

    // slot 12 (+0x60) — IDStorageQueueX1
    virtual void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE hEvent) {
        if (!hEvent) return;
        Op op{};
        op.kind = OpKind::Event;
        op.eventHandle = hEvent;
        std::lock_guard<std::mutex> lock(m_mtx);
        m_staged.push_back(std::move(op));
    }

    // slot 13 (+0x68) — IDStorageQueueX2
    virtual void STDMETHODCALLTYPE Query1(GdkQueueInfo1* info) {
        if (!info) return;
        std::memset(info, 0, sizeof(*info));
        info->Desc = m_desc;
        const uint32_t pend = PendingCount();
        info->EmptySlotCount = (m_desc.Capacity > pend)
            ? static_cast<uint16_t>(m_desc.Capacity - pend) : 0u;
        info->RequestCountUntilAutoSubmit = (m_desc.Options & 1ULL)
            ? 0u : static_cast<uint16_t>(m_desc.Capacity / 2u);
    }

private:
    enum class OpKind { Read, Status, Fence, Event };
    struct Op {
        OpKind                  kind{OpKind::Read};
        GdkRequest              req{};
        ComPtr<GdkStorageFileX> file;
        ComPtr<GdkStatusArrayX> statusArray;
        uint32_t                statusIndex{0};
        ComPtr<ID3D12Fence>     fence;
        uint64_t                fenceValue{0};
        HANDLE                  eventHandle{nullptr};
    };

    uint32_t PendingCount() {
        std::lock_guard<std::mutex> lock(m_mtx);
        size_t n = m_staged.size();
        for (const auto& b : m_batches) n += b.size();
        return static_cast<uint32_t>(n);
    }

    void RecordError(HRESULT hr, const GdkRequest& req) {
        std::lock_guard<std::mutex> lock(m_errMtx);
        if (m_errRecord.FailureCount == 0) {
            m_errRecord.CommandType = 0;
            m_errRecord.HResult = hr;
            m_errRecord.FailedRequest = req;
        }
        ++m_errRecord.FailureCount;
        if (m_errEvent) SetEvent(m_errEvent);
    }

    HRESULT ExecuteRead(const Op& op) {
        const uint64_t opts     = op.req.Options;
        const bool zlib         = (opts & 1ULL) != 0;
        const uint64_t bcpack   = (opts >> 1) & 0xFULL;
        const uint64_t swizzle  = (opts >> 5) & 0xFFFFULL;
        const bool dstPhys      = ((opts >> 21) & 1ULL) != 0;
        const uint64_t srcType  = (opts >> 22) & 1ULL;
        const bool srcPhys      = ((opts >> 23) & 1ULL) != 0;

        if (bcpack != 0 || swizzle != 0 || dstPhys || srcPhys) {
            static std::atomic<uint32_t> s_warned{0};
            if (s_warned.fetch_add(1, std::memory_order_relaxed) < 8) {
                char buf[224];
                std::snprintf(buf, sizeof(buf),
                              "-> [GDK] unsupported flags opts=0x%llX (zlib=%d bcpack=%llu swz=%llu dstP=%d srcP=%d srcSz=%u dstSz=%u)",
                              static_cast<unsigned long long>(opts), zlib ? 1 : 0,
                              static_cast<unsigned long long>(bcpack), static_cast<unsigned long long>(swizzle),
                              dstPhys ? 1 : 0, srcPhys ? 1 : 0, op.req.SourceSize, op.req.DestinationSize);
                sc::log::Write(sc::log::Level::Warn, "dstorage_xs", "GdkRead", buf);
            }
            return E_NOTIMPL;
        }

        if (zlib) {
            if (!op.req.Destination || op.req.SourceSize == 0 || op.req.DestinationSize == 0) {
                return E_INVALIDARG;
            }
            std::vector<uint8_t> compressed;
            const void* srcPtr = nullptr;
            uint32_t srcBytes = op.req.SourceSize;
            if (srcType == 0) {
                if (!op.file) return E_HANDLE;
                compressed.resize(op.req.SourceSize);
                uint32_t got = 0;
                HRESULT hr = op.file->ReadSlice(op.req.FileOffset, compressed.data(), op.req.SourceSize, got);
                if (FAILED(hr)) return hr;
                srcPtr = compressed.data();
                srcBytes = got;
            } else {
                if (!op.req.Source) return E_INVALIDARG;
                srcPtr = op.req.Source;
            }
            size_t decompressedBytes = 0;
            const bool ok = sc::codec::DecompressZlib(
                srcPtr, srcBytes,
                op.req.Destination, op.req.DestinationSize, decompressedBytes);
            if (!ok) {
                static std::atomic<uint32_t> s_zlibErr{0};
                if (s_zlibErr.fetch_add(1, std::memory_order_relaxed) < 8) {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                                  "-> [GDK] zlib decompress FAILED srcSz=%u dstSz=%u out=%zu",
                                  srcBytes, op.req.DestinationSize, decompressedBytes);
                    sc::log::Write(sc::log::Level::Warn, "dstorage_xs", "GdkRead", buf);
                }
                return E_FAIL;
            }
            static std::atomic<uint32_t> s_zlibOk{0};
            if (s_zlibOk.fetch_add(1, std::memory_order_relaxed) < 4) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "-> [GDK] zlib decompressed %u B -> %zu B (dstSz=%u)",
                              srcBytes, decompressedBytes, op.req.DestinationSize);
                sc::log::Write(sc::log::Level::Info, "dstorage_xs", "GdkRead", buf);
            }
            return S_OK;
        }

        if (!op.req.Destination || op.req.SourceSize > op.req.DestinationSize) {
            return E_INVALIDARG;
        }

        if (srcType == 0) {
            if (!op.file) return E_HANDLE;
            uint32_t got = 0;
            return op.file->ReadSlice(op.req.FileOffset, op.req.Destination, op.req.SourceSize, got);
        }
        if (!op.req.Source) return E_INVALIDARG;
        std::memcpy(op.req.Destination, op.req.Source, op.req.SourceSize);
        return S_OK;
    }

    void WorkerLoop() {
        while (true) {
            std::vector<Op> batch;
            {
                std::unique_lock<std::mutex> lock(m_mtx);
                m_cv.wait(lock, [this]() { return m_stopping || !m_batches.empty(); });
                if (m_stopping && m_batches.empty()) return;
                batch = std::move(m_batches.front());
                m_batches.pop_front();
            }
            HRESULT batchHr = S_OK;
            for (const auto& op : batch) {
                switch (op.kind) {
                    case OpKind::Read: {
                        HRESULT hr = ExecuteRead(op);
                        if (FAILED(hr)) {
                            if (SUCCEEDED(batchHr)) batchHr = hr;
                            RecordError(hr, op.req);
                        }
                        break;
                    }
                    case OpKind::Status:
                        if (op.statusArray) op.statusArray->CompleteSlot(op.statusIndex, batchHr);
                        batchHr = S_OK;
                        break;
                    case OpKind::Fence:
                        if (op.fence) op.fence->Signal(op.fenceValue);
                        break;
                    case OpKind::Event:
                        if (op.eventHandle) SetEvent(op.eventHandle);
                        break;
                }
            }
        }
    }

    std::atomic<ULONG>           m_refs{1};
    GdkQueueDesc1                m_desc{};
    std::string                  m_name;
    HANDLE                       m_errEvent{nullptr};
    std::mutex                   m_mtx;
    std::condition_variable      m_cv;
    std::vector<Op>              m_staged;
    std::deque<std::vector<Op>>  m_batches;
    bool                         m_stopping{false};
    std::thread                  m_worker;
    std::mutex                   m_errMtx;
    GdkErrorRecord               m_errRecord{};
};

class GdkStorageFactoryX2 final : public IUnknown {
public:
    GdkStorageFactoryX2() {
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "GdkStorageFactoryX2",
                       "-> initialized Xbox Series IDStorageFactoryX2");
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_GdkFactoryX2)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    // slot 3 (+0x18)
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(const GdkQueueDesc* desc, REFIID riid, void** ppv) {
        if (!desc || !ppv) return E_INVALIDARG;
        GdkQueueDesc1 d1{};
        d1.SourceType = desc->SourceType;
        d1.Capacity   = desc->Capacity;
        d1.Priority   = desc->Priority;
        d1.Name       = desc->Name;
        d1.Options    = 0;
        return CreateQueue1(&d1, riid, ppv);
    }

    // slot 4 (+0x20)
    virtual HRESULT STDMETHODCALLTYPE OpenFile(const wchar_t* path, REFIID riid, void** ppv) {
        if (!path || !*path || !ppv) return E_INVALIDARG;
        *ppv = nullptr;
        bool wasVirtual = false;
        const std::filesystem::path hostPath = sc::vfs::TranslatePath(path, &wasVirtual);
        HANDLE hFile = CreateFileW(
            hostPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            std::string msg = "-> [GDK] OpenFile FAILED: " + WideToUtf8(path) + " -> " +
                              WideToUtf8(hostPath.c_str()) + " (err=" + std::to_string(err) + ")";
            sc::log::Write(sc::log::Level::Warn, "dstorage_xs", "OpenFile", msg.c_str());
            return HRESULT_FROM_WIN32(err);
        }
        std::string utf8Host = WideToUtf8(hostPath.c_str());
        std::string msg = wasVirtual
            ? ("-> [GDK] opened [VFS]: " + WideToUtf8(path) + " -> " + utf8Host)
            : ("-> [GDK] opened: " + utf8Host);
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "OpenFile", msg.c_str());
        auto* f = new GdkStorageFileX(hFile, std::move(utf8Host));
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }

    // slot 5 (+0x28)
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(
        uint32_t capacity, const char* /*name*/, REFIID riid, void** ppv) {
        if (capacity == 0 || !ppv) return E_INVALIDARG;
        *ppv = nullptr;
        auto* arr = new GdkStatusArrayX(capacity);
        HRESULT hr = arr->QueryInterface(riid, ppv);
        arr->Release();
        return hr;
    }

    // slot 6 (+0x30)
    virtual void STDMETHODCALLTYPE SetDebugFlags(uint32_t flags) {
        m_debugFlags = flags;
    }

    // slot 7 (+0x38)
    virtual HRESULT STDMETHODCALLTYPE SetCpuAffinity(uint64_t affinity) {
        m_cpuAffinity = affinity;
        return S_OK;
    }

    // slot 8 (+0x40)
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(uint32_t size) {
        m_stagingSize = size;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "-> [GDK] stagingSize=%u B", size);
        sc::log::Write(sc::log::Level::Info, "dstorage_xs", "SetStagingBufferSize", buf);
        return S_OK;
    }

    // slot 9 (+0x48) — IDStorageFactoryX1
    virtual HRESULT STDMETHODCALLTYPE GetStats(void* stats) {
        if (stats) std::memset(stats, 0, 64);
        return S_OK;
    }

    // slot 10 (+0x50) — IDStorageFactoryX2
    virtual HRESULT STDMETHODCALLTYPE CreateQueue1(const GdkQueueDesc1* desc, REFIID riid, void** ppv) {
        if (!desc || !ppv) return E_INVALIDARG;
        *ppv = nullptr;
        auto* q = new GdkStorageQueueX2(*desc);
        HRESULT hr = q->QueryInterface(riid, ppv);
        q->Release();
        return hr;
    }

private:
    std::atomic<ULONG> m_refs{1};
    uint32_t           m_debugFlags{0};
    uint64_t           m_cpuAffinity{0};
    uint32_t           m_stagingSize{32u * 1024u * 1024u};
};

} // namespace gdk_ds

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
    if (IsEqualGUID(riid, IID_IDStorageFactoryX)) {
        auto* factory = new ScarlettStorageFactoryX();
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    if (IsEqualGUID(riid, gdk_ds::IID_GdkFactoryX2)) {
        auto* factory = new gdk_ds::GdkStorageFactoryX2();
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    char gbuf[128];
    std::snprintf(gbuf, sizeof(gbuf),
                  "-> unmapped factory IID {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  riid.Data1, riid.Data2, riid.Data3,
                  riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                  riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
    sc::log::Write(sc::log::Level::Warn, "dstorage_xs", __FUNCTION__, gbuf);
    return E_NOINTERFACE;
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