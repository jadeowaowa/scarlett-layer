#include "task_queue.hpp"
#include "common/stub.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

struct QueuePort : std::enable_shared_from_this<QueuePort> {
    struct Item {
        std::function<void(bool canceled)> fn;
    };

    XTaskQueueDispatchMode mode{XTaskQueueDispatchMode::Manual};
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Item> tasks;
    bool closed{false};
    std::thread worker;

    void StartIfThreaded() {
        if (mode == XTaskQueueDispatchMode::ThreadPool ||
            mode == XTaskQueueDispatchMode::SerializedThreadPool) {
            auto self = shared_from_this();
            worker = std::thread([self]() { self->WorkerLoop(); });
        }
    }

    bool IsClosed() {
        std::lock_guard<std::mutex> lock(mtx);
        return closed;
    }

    void WorkerLoop() {
        while (true) {
            Item item{};
            bool isCanceled = false;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this]() { return closed || !tasks.empty(); });
                if (tasks.empty()) {
                    if (closed) {
                        return;
                    }
                    continue;
                }
                item = std::move(tasks.front());
                tasks.pop_front();
                isCanceled = closed;
            }
            if (item.fn) {
                item.fn(isCanceled);
            }
        }
    }

    HRESULT SubmitItem(std::function<void(bool)> fn) {
        if (mode == XTaskQueueDispatchMode::Immediate) {
            if (IsClosed()) {
                return E_ABORT;
            }
            if (fn) {
                fn(false);
            }
            return S_OK;
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (closed) {
                return E_ABORT;
            }
            tasks.push_back(Item{std::move(fn)});
        }
        cv.notify_one();
        return S_OK;
    }

    bool DispatchOne(uint32_t timeoutMs) {
        Item item{};
        bool isCanceled = false;
        {
            std::unique_lock<std::mutex> lock(mtx);
            if (tasks.empty() && !closed && timeoutMs > 0) {
                if (timeoutMs == INFINITE) {
                    cv.wait(lock, [this]() { return closed || !tasks.empty(); });
                } else {
                    cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [this]() { return closed || !tasks.empty(); });
                }
            }
            if (tasks.empty()) {
                return false;
            }
            item = std::move(tasks.front());
            tasks.pop_front();
            isCanceled = closed;
        }
        if (item.fn) {
            item.fn(isCanceled);
        }
        return true;
    }

    void Close(bool wait) {
        std::deque<Item> canceledItems;
        std::thread toJoin;
        {
            std::lock_guard<std::mutex> lock(mtx);
            closed = true;
            if (!worker.joinable()) {
                // Manual / Immediate port: drain pending items and fire canceled notifications
                canceledItems.swap(tasks);
            }
            if (wait && worker.joinable()) {
                if (worker.get_id() == std::this_thread::get_id()) {
                    worker.detach();
                } else {
                    toJoin = std::move(worker);
                }
            }
        }
        cv.notify_all();
        for (auto& item : canceledItems) {
            if (item.fn) {
                item.fn(true);
            }
        }
        if (toJoin.joinable()) {
            toJoin.join();
        }
    }

    ~QueuePort() {
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id()) {
                worker.detach();
            } else {
                worker.join();
            }
        }
    }
};

struct XTaskQueueObject {
    std::atomic<uint32_t> refs{1};
    std::shared_ptr<QueuePort> work;
    std::shared_ptr<QueuePort> completion;

    XTaskQueueObject(XTaskQueueDispatchMode workMode, XTaskQueueDispatchMode compMode)
        : work(std::make_shared<QueuePort>()),
          completion(std::make_shared<QueuePort>()) {
        work->mode = workMode;
        completion->mode = compMode;
        work->StartIfThreaded();
        completion->StartIfThreaded();
    }
};

namespace {
std::mutex g_processQueueMtx;
XTaskQueueHandle g_processQueue = nullptr;

void ReleaseQueueRef(XTaskQueueHandle queue) {
    if (!queue) return;
    if (queue->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        queue->work->Close(true);
        queue->completion->Close(true);
        delete queue;
    }
}
} // namespace

namespace sc::runtime {

void InitProcessTaskQueue() {
    std::lock_guard<std::mutex> lock(g_processQueueMtx);
    if (!g_processQueue) {
        g_processQueue = new XTaskQueueObject(
            XTaskQueueDispatchMode::ThreadPool,
            XTaskQueueDispatchMode::ThreadPool);
    }
}

void ShutdownProcessTaskQueue() {
    XTaskQueueHandle q = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_processQueueMtx);
        q = g_processQueue;
        g_processQueue = nullptr;
    }
    ReleaseQueueRef(q);
}

XTaskQueueHandle GetProcessTaskQueue() {
    std::lock_guard<std::mutex> lock(g_processQueueMtx);
    return g_processQueue;
}

void SetProcessTaskQueue(XTaskQueueHandle queue) {
    XTaskQueueHandle old = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_processQueueMtx);
        if (queue) {
            queue->refs.fetch_add(1, std::memory_order_relaxed);
        }
        old = g_processQueue;
        g_processQueue = queue;
    }
    ReleaseQueueRef(old);
}

void AddRefTaskQueue(XTaskQueueHandle queue) {
    if (queue) {
        queue->refs.fetch_add(1, std::memory_order_relaxed);
    }
}

HRESULT DuplicateTaskQueueHandle(XTaskQueueHandle queue, XTaskQueueHandle* outQueue) {
    if (!outQueue) return E_POINTER;
    *outQueue = nullptr;
    if (!queue) return E_INVALIDARG;
    queue->refs.fetch_add(1, std::memory_order_relaxed);
    *outQueue = queue;
    return S_OK;
}

HRESULT SubmitTask(XTaskQueueHandle queue, XTaskQueuePort port, std::function<void()> fn) {
    XTaskQueueHandle target = queue;
    bool borrowedProcessQueue = false;
    if (!target) {
        std::lock_guard<std::mutex> lock(g_processQueueMtx);
        target = g_processQueue;
        if (target) {
            target->refs.fetch_add(1, std::memory_order_relaxed);
            borrowedProcessQueue = true;
        }
    }
    if (!target) {
        return E_FAIL;
    }
    auto p = (port == XTaskQueuePort::Work) ? target->work : target->completion;
    HRESULT hr = p->SubmitItem([f = std::move(fn)](bool canceled) {
        if (!canceled && f) {
            f();
        }
    });
    if (borrowedProcessQueue) {
        ReleaseQueueRef(target);
    }
    return hr;
}

HRESULT SubmitTaskCallback(
    XTaskQueueHandle queue, XTaskQueuePort port, uint32_t delayMs,
    void* context, void(CALLBACK* callback)(void*, bool)) {
    if (!callback) return E_INVALIDARG;
    XTaskQueueHandle target = queue;
    bool borrowed = false;
    if (!target) {
        std::lock_guard<std::mutex> lock(g_processQueueMtx);
        target = g_processQueue;
        if (target) {
            target->refs.fetch_add(1, std::memory_order_relaxed);
            borrowed = true;
        }
    }
    if (!target) return E_HANDLE;
    auto p = (port == XTaskQueuePort::Work) ? target->work : target->completion;
    if (borrowed) {
        ReleaseQueueRef(target);
    }
    if (p->IsClosed()) {
        return E_ABORT;
    }
    if (delayMs == 0) {
        return p->SubmitItem([context, callback](bool canceled) {
            callback(context, canceled);
        });
    }
    std::thread([p, delayMs, context, callback]() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
        while (!p->IsClosed()) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            auto step = std::min(std::chrono::milliseconds(20),
                                 std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            std::this_thread::sleep_for(step.count() > 0 ? step : std::chrono::milliseconds(1));
        }
        if (p->IsClosed()) {
            callback(context, true);
        } else {
            p->SubmitItem([context, callback](bool canceled) {
                callback(context, canceled);
            });
        }
    }).detach();
    return S_OK;
}

HRESULT TerminateTaskQueue(
    XTaskQueueHandle queue, bool wait, void* context, void(CALLBACK* callback)(void*)) {
    if (!queue) return E_HANDLE;
    queue->refs.fetch_add(1, std::memory_order_relaxed);
    auto work = queue->work;
    auto comp = queue->completion;
    work->Close(wait);
    comp->Close(wait);
    if (callback) {
        callback(context);
    }
    ReleaseQueueRef(queue);
    return S_OK;
}

} // namespace sc::runtime

extern "C" {

HRESULT WINAPI XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue) {
    if (!queue) {
        return E_POINTER;
    }
    *queue = new XTaskQueueObject(workDispatchMode, completionDispatchMode);
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> created queue");
    return S_OK;
}

bool WINAPI XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs) {
    if (!queue) {
        return false;
    }
    queue->refs.fetch_add(1, std::memory_order_relaxed);
    auto p = (port == XTaskQueuePort::Work) ? queue->work : queue->completion;
    bool result = p->DispatchOne(timeoutInMs);
    ReleaseQueueRef(queue);
    return result;
}

void WINAPI XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    if (!queue) {
        return;
    }
    const uint32_t prev = queue->refs.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        queue->work->Close(true);
        queue->completion->Close(true);
        delete queue;
        sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> closed queue (ref=0)");
    }
}

} // extern "C"
