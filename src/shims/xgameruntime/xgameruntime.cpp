#include "common/stub.hpp"
#include "gdk_types.hpp"
#include "runtime_internal.hpp"
#include "task_queue.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

struct XUser {
    uint64_t xuid{0x0009000000000001ULL};
    std::string gamertag{"ScarlettPlayer"};
    std::atomic<uint32_t> refs{1};
};

namespace sc::runtime {

uint64_t GetUserXuid(XUserHandle user) {
    return user ? user->xuid : 0x0009000000000001ULL;
}

XUserHandle AcquireDefaultUser() {
    return new XUser();
}

HRESULT DuplicateUserHandle(XUserHandle user, XUserHandle* outUser) {
    if (!outUser) return E_POINTER;
    *outUser = nullptr;
    if (!user) return E_HANDLE;
    user->refs.fetch_add(1, std::memory_order_relaxed);
    *outUser = user;
    return S_OK;
}

} // namespace sc::runtime

namespace {

struct UserAddPayload {
    XUserHandle user{nullptr};
    ~UserAddPayload() {
        if (user && user->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete user;
        }
    }
};

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
    auto* state = sc::runtime::LoadAsyncState(asyncBlock);
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
    sc::log::Write(sc::log::Level::Info, "xgameruntime", __FUNCTION__,
                   "-> queued mock local profile sign-in");

    return sc::runtime::LaunchAsync(async, [](std::shared_ptr<void>& outPayload) -> HRESULT {
        sc::log::Write(sc::log::Level::Info, "xgameruntime", "XUserAddAsync[Work]",
                       "-> creating offline XUser handle");
        auto result = std::make_shared<UserAddPayload>();
        result->user = new XUser();
        outPayload = std::move(result);
        return S_OK;
    });
}

HRESULT WINAPI XUserAddResult(XAsyncBlock* async, XUserHandle* newUser) {
    if (!async || !newUser) {
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
            auto payload = std::static_pointer_cast<UserAddPayload>(state->payload);
            *newUser = payload->user;
            payload->user = nullptr; // transfer ownership to caller
        }
    }

    sc::runtime::DetachAsyncState(async, state);
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

extern "C" {

HRESULT WINAPI XUserGetId(XUserHandle user, uint64_t* userId) {
    if (!user || !userId) return E_INVALIDARG;
    *userId = sc::runtime::GetUserXuid(user);
    return S_OK;
}

HRESULT WINAPI XUserGetState(XUserHandle user, XUserState* state) {
    if (!user || !state) return E_INVALIDARG;
    *state = XUserState::SignedIn;
    return S_OK;
}

HRESULT WINAPI XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId) {
    if (!user || !userLocalId) return E_INVALIDARG;
    userLocalId->value = sc::runtime::GetUserXuid(user);
    return S_OK;
}

} // extern "C"
