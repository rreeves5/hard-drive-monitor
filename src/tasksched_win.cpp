// Windows Task Scheduler 2.0 through its COM API (ITaskService), instead of running schtasks.exe.
#include "osnative.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <cstdio>
#include <taskschd.h>

namespace hdmon::osn {
namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string hr_text(const char* what, HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof buf, " failed (HRESULT 0x%08lX)", static_cast<unsigned long>(hr));
    std::string s = std::string(what) + buf;
    if (hr == static_cast<HRESULT>(0x80070005)) s += ": access denied";
    return s;
}
struct ComInit {
    bool uninit = false;
    ComInit() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninit = SUCCEEDED(hr);   // RPC_E_CHANGED_MODE: already initialised differently; just use it
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, 0, nullptr);   // may fail if already set by the host process; harmless
    }
    ~ComInit() { if (uninit) CoUninitialize(); }
};
template <class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};
// Connects and opens the root folder "\".
bool root_folder(Ptr<ITaskService>& svc, Ptr<ITaskFolder>& folder, std::string& err) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                  reinterpret_cast<void**>(&svc));
    if (FAILED(hr)) { err = hr_text("CoCreateInstance(TaskScheduler)", hr); return false; }
    VARIANT empty;
    VariantInit(&empty);
    hr = svc->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) { err = hr_text("ITaskService::Connect", hr); return false; }
    BSTR root = SysAllocString(L"\\");
    hr = svc->GetFolder(root, &folder);
    SysFreeString(root);
    if (FAILED(hr)) { err = hr_text("ITaskService::GetFolder", hr); return false; }
    return true;
}
}  // namespace

bool task_register_xml(const std::string& name, const std::string& xml, std::string& err) {
    ComInit com;
    Ptr<ITaskService> svc;
    Ptr<ITaskFolder> folder;
    if (!root_folder(svc, folder, err)) return false;
    BSTR bname = SysAllocString(wide(name).c_str());
    BSTR bxml = SysAllocString(wide(xml).c_str());
    VARIANT empty;
    VariantInit(&empty);
    Ptr<IRegisteredTask> task;
    // The principal in the XML (UserId + InteractiveToken + RunLevel) decides who it runs as; no password needed.
    HRESULT hr = folder->RegisterTask(bname, bxml, TASK_CREATE_OR_UPDATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN,
                                      empty, &task);
    SysFreeString(bname);
    SysFreeString(bxml);
    if (FAILED(hr)) { err = hr_text("ITaskFolder::RegisterTask", hr); return false; }
    return true;
}

bool task_delete(const std::string& name, std::string& err) {
    ComInit com;
    Ptr<ITaskService> svc;
    Ptr<ITaskFolder> folder;
    if (!root_folder(svc, folder, err)) return false;
    BSTR bname = SysAllocString(wide(name).c_str());
    HRESULT hr = folder->DeleteTask(bname, 0);
    SysFreeString(bname);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return true;
    if (FAILED(hr)) { err = hr_text("ITaskFolder::DeleteTask", hr); return false; }
    return true;
}

int task_exists(const std::string& name) {
    ComInit com;
    Ptr<ITaskService> svc;
    Ptr<ITaskFolder> folder;
    std::string err;
    if (!root_folder(svc, folder, err)) return -1;
    BSTR bname = SysAllocString(wide(name).c_str());
    Ptr<IRegisteredTask> task;
    HRESULT hr = folder->GetTask(bname, &task);
    SysFreeString(bname);
    if (SUCCEEDED(hr)) return 1;
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? 0 : -1;
}

}  // namespace hdmon::osn
#else
namespace hdmon::osn {
bool task_register_xml(const std::string&, const std::string&, std::string& err) { err = "Task Scheduler is Windows-only"; return false; }
bool task_delete(const std::string&, std::string& err) { err = "Task Scheduler is Windows-only"; return false; }
int task_exists(const std::string&) { return -1; }
}  // namespace hdmon::osn
#endif
