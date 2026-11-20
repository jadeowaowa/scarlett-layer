#pragma once
#include "gdk_types.hpp"
#include <functional>

namespace sc::runtime {

void InitProcessTaskQueue();
void ShutdownProcessTaskQueue();
XTaskQueueHandle GetProcessTaskQueue();

HRESULT SubmitTask(XTaskQueueHandle queue, XTaskQueuePort port, std::function<void()> fn);

} // namespace sc::runtime