#pragma once
#include "gdk_types.hpp"
#include <functional>

namespace sc::runtime {

void InitProcessTaskQueue();
void ShutdownProcessTaskQueue();
XTaskQueueHandle GetProcessTaskQueue();
void SetProcessTaskQueue(XTaskQueueHandle queue);

void AddRefTaskQueue(XTaskQueueHandle queue);
HRESULT DuplicateTaskQueueHandle(XTaskQueueHandle queue, XTaskQueueHandle* outQueue);
HRESULT SubmitTask(XTaskQueueHandle queue, XTaskQueuePort port, std::function<void()> fn);
HRESULT SubmitTaskCallback(
    XTaskQueueHandle queue, XTaskQueuePort port, uint32_t delayMs,
    void* context, void(CALLBACK* callback)(void*, bool));
HRESULT TerminateTaskQueue(
    XTaskQueueHandle queue, bool wait, void* context, void(CALLBACK* callback)(void*));

} // namespace sc::runtime
