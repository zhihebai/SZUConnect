// Declaration-only support for optional offline-smoke visual QA. Never used by Windows builds.
#pragma once
#include "winmock.h"
struct HBITMAP__ : HGDIOBJ__ {}; using HBITMAP = HBITMAP__*;
struct BITMAPINFOHEADER { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; };
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };
constexpr DWORD BI_RGB = 0, DIB_RGB_COLORS = 0, SRCCOPY = 0x00CC0020;
WINMOCK_FN(HDC, GetDC) WINMOCK_FN(int, ReleaseDC) WINMOCK_FN(HDC, CreateCompatibleDC)
WINMOCK_FN(HBITMAP, CreateDIBSection) WINMOCK_FN(HGDIOBJ, SelectObject) WINMOCK_FN(BOOL, DeleteDC)
WINMOCK_FN(BOOL, BitBlt) WINMOCK_FN(BOOL, GdiFlush) WINMOCK_FN(HRESULT, DwmFlush)
