#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <vector>
#include "../resources/resource.h"

int main() {
    const auto module = GetModuleHandleW(nullptr);
    int checks = 0, failures = 0;
    for (int id : {IDI_APP, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109}) {
        for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
            if (id != IDI_APP && size > 64) continue;
            ++checks;
            HICON icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
            ICONINFO info{};
            if (!icon || !GetIconInfo(icon, &info) || !info.hbmColor) { ++failures; if (icon) DestroyIcon(icon); continue; }
            BITMAP bitmap{};
            if (!GetObjectW(info.hbmColor, sizeof bitmap, &bitmap) || bitmap.bmWidth != size || bitmap.bmHeight != size) ++failures;
            BITMAPINFO pixels_info{};
            pixels_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            pixels_info.bmiHeader.biWidth = size; pixels_info.bmiHeader.biHeight = -size;
            pixels_info.bmiHeader.biPlanes = 1; pixels_info.bmiHeader.biBitCount = 32;
            pixels_info.bmiHeader.biCompression = BI_RGB;
            std::vector<DWORD> pixels(static_cast<std::size_t>(size * size));
            HDC dc = GetDC(nullptr);
            const int rows = GetDIBits(dc, info.hbmColor, 0, size, pixels.data(), &pixels_info, DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            bool visible = false;
            for (const auto pixel : pixels) visible = visible || ((pixel >> 24) != 0);
            if (rows != size || !visible) ++failures;
            DeleteObject(info.hbmColor); if (info.hbmMask) DeleteObject(info.hbmMask); DestroyIcon(icon);
        }
        HICON metric = nullptr;
        ++checks;
        if (FAILED(LoadIconMetric(module, MAKEINTRESOURCEW(id), LIM_SMALL, &metric)) || !metric) ++failures;
        if (metric) DestroyIcon(metric);
    }
    std::printf("Native icon resources: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
