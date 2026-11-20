#include "shims/xgameruntime/gdk_types.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>

extern "C" {
    __declspec(dllimport) HRESULT WINAPI XGameRuntimeInitialize();
    __declspec(dllimport) void    WINAPI XGameRuntimeUninitialize();
    __declspec(dllimport) XSystemDeviceType WINAPI XSystemGetDeviceType();
    __declspec(dllimport) HRESULT WINAPI XTaskQueueCreate(
        XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle* queue);
    __declspec(dllimport) bool    WINAPI XTaskQueueDispatch(
        XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeoutInMs);
    __declspec(dllimport) void    WINAPI XTaskQueueCloseHandle(XTaskQueueHandle queue);
    __declspec(dllimport) HRESULT WINAPI XUserAddAsync(XUserAddOptions options, XAsyncBlock* async);
    __declspec(dllimport) HRESULT WINAPI XUserAddResult(XAsyncBlock* async, XUserHandle* newUser);
    __declspec(dllimport) HRESULT WINAPI XUserGetGamertag(
        XUserHandle user, XUserGamertagComponent comp, size_t size, char* out, size_t* used);
    __declspec(dllimport) void    WINAPI XUserCloseHandle(XUserHandle user);
}

struct SignInContext {
    std::atomic<bool> done{false};
    XUserHandle user{nullptr};
};

static void CALLBACK OnUserAdded(XAsyncBlock* async) {
    auto* ctx = static_cast<SignInContext*>(async->context);
    HRESULT hr = XUserAddResult(async, &ctx->user);
    std::printf("[TITLE][T:%05lu] OnUserAdded fired! hr=0x%08lX\n", GetCurrentThreadId(), hr);

    if (SUCCEEDED(hr) && ctx->user) {
        char tag[64]{};
        size_t used = 0;
        if (SUCCEEDED(XUserGetGamertag(ctx->user, XUserGamertagComponent::Classic,
                                       sizeof(tag), tag, &used))) {
            std::printf("[TITLE][T:%05lu] Signed in as: %s\n", GetCurrentThreadId(), tag);
        }
    }
    ctx->done.store(true, std::memory_order_release);
}

int main() {
    std::printf("[TITLE][T:%05lu] Booting synthetic Series X title...\n", GetCurrentThreadId());

    if (FAILED(XGameRuntimeInitialize())) {
        return 1;
    }

    auto device = XSystemGetDeviceType();
    std::printf("[TITLE] Device type: 0x%02X\n", static_cast<uint32_t>(device));

    XTaskQueueHandle queue = nullptr;
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool,
                                XTaskQueueDispatchMode::Manual, &queue))) {
        return 1;
    }

    SignInContext ctx{};
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &ctx;
    async.callback = OnUserAdded;

    HRESULT hr = XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &async);
    if (SUCCEEDED(hr)) {
        while (!ctx.done.load(std::memory_order_acquire)) {
            XTaskQueueDispatch(queue, XTaskQueuePort::Completion, 50);
        }
    }

    if (ctx.user) {
        XUserCloseHandle(ctx.user);
    }
    XTaskQueueCloseHandle(queue);
    XGameRuntimeUninitialize();

    std::printf("[TITLE] Shutdown complete.\n");
    return 0;
}