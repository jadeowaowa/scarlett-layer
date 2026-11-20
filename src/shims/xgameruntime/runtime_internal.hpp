#pragma once

#include "gdk_types.hpp"
#include "task_queue.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>

namespace sc::runtime {

struct AsyncState {
    std::mutex mtx;
    std::condition_variable cv;
    HRESULT status{E_PENDING};
    bool completed{false};
    std::shared_ptr<void> payload;
    std::atomic<uint32_t> refs{2}; // 1 = XAsyncBlock owner, 1 = in-flight pipeline
};

inline void ReleaseAsyncState(AsyncState* s) {
    if (s && s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete s;
    }
}

inline void StoreAsyncState(XAsyncBlock* block, AsyncState* state) {
    std::memset(block->internal, 0, sizeof(block->internal));
    std::memcpy(block->internal, &state, sizeof(state));
}

inline AsyncState* LoadAsyncState(XAsyncBlock* block) {
    AsyncState* state = nullptr;
    std::memcpy(&state, block->internal, sizeof(state));
    return state;
}

inline void DetachAsyncState(XAsyncBlock* block, AsyncState* state) {
    std::memset(block->internal, 0, sizeof(block->internal));
    ReleaseAsyncState(state);
}

// Runs workFn on XTaskQueuePort::Work, stores result/payload, then fires async->callback on Completion
inline HRESULT LaunchAsync(
    XAsyncBlock* async,
    std::function<HRESULT(std::shared_ptr<void>& outPayload)> workFn) {
    if (!async) {
        return E_INVALIDARG;
    }

    auto* state = new AsyncState();
    StoreAsyncState(async, state);
    XTaskQueueHandle queue = async->queue;

    HRESULT submitHr = SubmitTask(queue, XTaskQueuePort::Work,
        [async, state, queue, fn = std::move(workFn)]() {
            std::shared_ptr<void> payload;
            HRESULT hr = fn(payload);
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                state->payload = std::move(payload);
                state->status = hr;
                state->completed = true;
            }
            state->cv.notify_all();

            SubmitTask(queue, XTaskQueuePort::Completion, [async, state]() {
                if (async->callback) {
                    async->callback(async);
                }
                ReleaseAsyncState(state);
            });
        });

    if (FAILED(submitHr)) {
        std::memset(async->internal, 0, sizeof(async->internal));
        delete state;
    }
    return submitHr;
}

uint64_t GetUserXuid(XUserHandle user);

} // namespace sc::runtime