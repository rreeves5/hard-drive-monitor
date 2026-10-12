#include "osnative.hpp"

#include "dbus.hpp"
#include "util.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace hdmon::osn {

#if defined(_WIN32)
namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
void copy_trunc(wchar_t* dst, size_t cap, const std::wstring& src) {
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = 0;
}
LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
}  // namespace

bool notify(const std::string& title, const std::string& body, std::string& how_or_err) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HdmonNotifyWindow";
    RegisterClassW(&wc);   // fails harmlessly if already registered
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"hdmon", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!hwnd) { how_or_err = "CreateWindowEx failed (" + std::to_string(GetLastError()) + ")"; return false; }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    nid.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_WARNING));
    copy_trunc(nid.szTip, sizeof nid.szTip / sizeof(wchar_t), L"Hard Drive Monitor");
    copy_trunc(nid.szInfoTitle, sizeof nid.szInfoTitle / sizeof(wchar_t), wide(title));
    copy_trunc(nid.szInfo, sizeof nid.szInfo / sizeof(wchar_t), wide(body));
    nid.dwInfoFlags = NIIF_WARNING;
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        DestroyWindow(hwnd);
        how_or_err = "Shell_NotifyIcon failed (no shell / notification area in this session)";
        return false;
    }
    // Keep the icon long enough for the shell to show the balloon / toast, pumping messages meanwhile.
    DWORD until = GetTickCount() + 8000;
    while (GetTickCount() < until) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(100);
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(hwnd);
    how_or_err = "Shell_NotifyIcon balloon";
    return true;
}
#elif defined(__APPLE__)
// Implemented in notify_macos.mm (UserNotifications framework).
#else
bool notify(const std::string& title, const std::string& body, std::string& how_or_err) {
    std::string err;
    if (dbus::notify("Hard Drive Monitor", title, body, true, err)) { how_or_err = "D-Bus org.freedesktop.Notifications"; return true; }
    how_or_err = err;
    return false;
}
#endif

}  // namespace hdmon::osn
