#include "common/stub.hpp"
#include "gdk_types.hpp"
#include "task_queue.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

struct XUser {
    uint64_t xuid{0x0009000000000001ULL};
    std::string gamertag{"ScarlettPlayer"};
    std::atomic<uint32_t> refs{1};
};

namespace {

struct AsyncState {
    std::mutex mtx;
    std::condition_variable cv;
    HRESULT status{E_PENDING};
    bool completed{false};
    void* payload{nullptr};
    std::atomic<uint32_t> refs{2};
};

void ReleaseAsyncState(AsyncState* s) {
    if (s && s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete s;
    }
}

void StoreAsyncState(XAsyncBlock* block, AsyncState* state) {
    std::memset(block->internal, 0, sizeof(block->internal));
    std::memcpy(block->internal, &state, sizeof(state));
}

AsyncState* LoadAsyncState(XAsyncBlock* block) {
    AsyncState* state = nullptr;
    std::memcpy(&state, block->internal, sizeof(state));
    return state;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "DllMain", path);
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI XGameRuntimeInitialize() {
    sc::runtime::InitProcessTaskQueue();
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> S_OK");
    return S_OK;
}

void WINAPI XGameRuntimeUninitialize() {
    sc::runtime::ShutdownProcessTaskQueue();
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> clean shutdown");
}

XSystemDeviceType WINAPI XSystemGetDeviceType() {
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__,
                   "-> spoofing XboxScarlettAnaconda (0x07)");
    return XSystemDeviceType::XboxScarlettAnaconda;
}

HRESULT WINAPI XAsyncGetStatus(XAsyncBlock* asyncBlock, bool wait) {
    if (!asyncBlock) {
        return E_INVALIDARG;
    }
    AsyncState* state = LoadAsyncState(asyncBlock);
    if (!state) {
        return E_INVALIDARG;
    }
    std::unique_lock<std::mutex> lock(state->mtx);
    if (wait && !state->completed) {
        state->cv.wait(lock, [state]() { return state->completed; });
    }
    return state->status;
}

HRESULT WINAPI XUserAddAsync(XUserAddOptions /*options*/, XAsyncBlock* async) {
    if (!async) {
        return E_INVALIDARG;
    }

    auto* state = new AsyncState();
    StoreAsyncState(async, state);
    XTaskQueueHandle queue = async->queue;

    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__,
                   "-> queued mock local profile sign-in");

    return sc::runtime::SubmitTask(queue, XTaskQueuePort::Work, [async, state, queue]() {
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "XUserAddAsync[Work]",
                       "-> creating offline XUser handle");

        auto* user = new XUser();
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->payload = user;
            state->status = S_OK;
            state->completed = true;
        }
        state->cv.notify_all();

        sc::runtime::SubmitTask(queue, XTaskQueuePort::Completion, [async, state]() {
            if (async->callback) {
                async->callback(async);
            }
            ReleaseAsyncState(state);
        });
    });
}

HRESULT WINAPI XUserAddResult(XAsyncBlock* async, XUserHandle* newUser) {
    if (!async || !newUser) {
        return E_INVALIDARG;
    }
    AsyncState* state = LoadAsyncState(async);
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
        if (SUCCEEDED(hr)) {
            *newUser = static_cast<XUserHandle>(state->payload);
            state->payload = nullptr;
        }
    }

    std::memset(async->internal, 0, sizeof(async->internal));
    ReleaseAsyncState(state);
    return hr;
}

HRESULT WINAPI XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent /*gamertagComponent*/,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed) {
    if (!user || !gamertag) {
        return E_INVALIDARG;
    }
    const size_t needed = user->gamertag.size() + 1;
    if (gamertagUsed) {
        *gamertagUsed = needed;
    }
    if (gamertagSize < needed) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    std::memcpy(gamertag, user->gamertag.c_str(), needed);
    return S_OK;
}

void WINAPI XUserCloseHandle(XUserHandle user) {
    if (user && user->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete user;
        sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__, "-> closed XUser");
    }
}

} // extern "C"