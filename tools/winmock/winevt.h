// Declaration-only event API surface; native tests verify the real SDK and behaviour.
#pragma once
#include "windows.h"
using EVT_HANDLE = HANDLE;
struct EVT_VARIANT {
    union { LPCWSTR StringVal; std::uint64_t FileTimeVal; };
    DWORD Count;
    DWORD Type;
};
constexpr DWORD EvtVarTypeString = 1, EvtVarTypeFileTime = 17;
constexpr DWORD EvtRenderEventValues = 0, EvtRenderContextValues = 0;
constexpr DWORD EvtQueryChannelPath = 1, EvtQueryReverseDirection = 0x200;
WINMOCK_FN(BOOL, EvtClose)
WINMOCK_FN(EVT_HANDLE, EvtQuery)
WINMOCK_FN(EVT_HANDLE, EvtCreateRenderContext)
WINMOCK_FN(BOOL, EvtRender)
WINMOCK_FN(BOOL, EvtNext)
