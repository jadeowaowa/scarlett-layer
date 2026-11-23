#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

extern "C" {

BOOL WINAPI XFrontPanelIsSupported() {
    return FALSE;
}

HRESULT WINAPI XFrontPanelGetScreenDimensions(uint32_t* height, uint32_t* width) {
    if (height) *height = 0;
    if (width)  *width  = 0;
    return E_NOTIMPL;
}

HRESULT WINAPI XFrontPanelPresentBuffer(uint32_t /*bufferSize*/, const uint8_t* /*buffer*/) {
    return E_NOTIMPL;
}

HRESULT WINAPI XFrontPanelGetLightStates(uint32_t* lights) {
    if (lights) *lights = 0;
    return E_NOTIMPL;
}

HRESULT WINAPI XFrontPanelSetLightStates(uint32_t /*lights*/) {
    return E_NOTIMPL;
}

HRESULT WINAPI XFrontPanelGetButtonStates(uint32_t* buttons) {
    if (buttons) *buttons = 0;
    return E_NOTIMPL;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
