#include "task_queue.hpp"
#include "common/stub.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

struct QueuePort {
    XTaskQueueDispatchMode mode{XTaskQueueDispatchMode::Manual};
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    bool closed{false};
    std::thread worker;

    void StartIfThreaded() {
        if (mode == XTaskQueueDispatchMode::ThreadPool ||
            mode == XTaskQueueDispatchMode::SerializedThreadPool) {
            worker = std::thread([this]() { WorkerLoop(); });
        }
    }

    void WorkerLoop() {
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this]() { return closed || !tasks.empty(); });
                if (closed && tasks.empty()) {
                    return;
                }
                fn = std::move(tasks.front());
                tasks.pop_front();
            }
            if (fn) {
                fn();
            }
        }
    }

    void Submit(std::function<void()> fn) {
        if (mode == XTaskQueueDispatchMode::Immediate) {
            if (fn) {
                fn();
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (closed) {
                return;
            }
            tasks.push_back(std::move(fn));
        }
        cv.notify_one();
    }

    bool DispatchOne(uint32_t timeoutMs) {
        std::function<void()> fn;
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
            fn = std::move(tasks.front());
            tasks.pop_front();
        }
        if (fn) {
            fn();
        }
        return true;
    }

    void Close() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            closed = true;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }
};

struct XTaskQueueObject {
    QueuePort work;
    QueuePort completion;
};

namespace {
std::mutex g_processQueueMtx;
XTaskQueueHandle g_processQueue = nullptr;
} // namespace

namespace sc::runtime {

void InitProcessTaskQueue() {
    std::lock_guard<std::mutex> lock(g_processQueueMtx);
    if (!g_processQueue) {
        auto* q = new XTaskQueueObject();
        q->work.mode = XTaskQueueDispatchMode::ThreadPool;
        q->completion.mode = XTaskQueueDispatchMode::ThreadPool;
        q->work.StartIfThreaded();
        q->completion.StartIfThreaded();
        g_processQueue = q;
    }
}

void ShutdownProcessTaskQueue() {
    XTaskQueueHandle q = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_processQueueMtx);
        q = g_processQueue;
        g_processQueue = nullptr;
    }
    if (q) {
        q->work.Close();
        q->completion.Close();
        delete q;
    }
}

XTaskQueueHandle GetProcessTaskQueue() {
    std::lock_guard<std::mutex> lock(g_processQueueMtx);
    return g_processQueue;
}

HRESULT SubmitTask(XTaskQueueHandle queue, XTaskQueuePort port, std::function<void()> fn) {
    XTaskQueueHandle target = queue ? queue : GetProcessTaskQueue();
    if (!target) {
        return E_FAIL;
    }
    QueuePort& p = (port == XTaskQueuePort::Work) ? target->work : target->completion;
    p.Submit(std::move(fn));
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
    auto* q = new XTaskQueueObject();
    q->work.mode = workDispatchMode;
    q->completion.mode = completionDispatchMode;
    q->work.StartIfThreaded();
    q->completion.StartIfThreaded();
    *queue = q;
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
    QueuePort& p = (port == XTaskQueuePort::Work) ? queue->work : queue->completion;
    return p.DispatchOne(timeoutInMs);
}

void WINAPI XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    if (!queue) {
        return;
    }
    queue->work.Close();
    queue->completion.Close();
    delete queue;
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> closed queue");
}

} // extern "C"