// Hard Drive Monitor (hdmon) entry point.
#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "util.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
static std::vector<std::string> utf8_args() {
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    for (int i = 1; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(w);
    return out;
}
#endif

#if defined(_WIN32) && defined(HDMON_GUI)
// hdmonw.exe: GUI-subsystem twin used by the scheduled task so no console window flashes.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return hdmon::run_cli(utf8_args(), std::cout, std::cerr);
}
#else
int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    std::vector<std::string> args = utf8_args();
    hdmon::console_init(false);
#else
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    std::ios::sync_with_stdio(true);
    return hdmon::run_cli(args, std::cout, std::cerr);
}
#endif
