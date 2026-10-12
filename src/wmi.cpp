#include "wmi.hpp"

#include "smart.hpp"   // PS_PHYSICAL / run_powershell_json (fallback) - before <windows.h>

namespace hdmon {

std::string msft_health(int v) {
    switch (v) { case 0: return "Healthy"; case 1: return "Warning"; case 2: return "Unhealthy"; default: return "Unknown"; }
}
std::string msft_operational(int v) {
    switch (v) {
        case 0: return "Unknown"; case 1: return "Other"; case 2: return "OK"; case 3: return "Degraded";
        case 4: return "Stressed"; case 5: return "Predictive Failure"; case 6: return "Error";
        case 7: return "Non-Recoverable Error"; case 8: return "Starting"; case 9: return "Stopping";
        case 10: return "Stopped"; case 11: return "In Service"; case 12: return "No Contact";
        case 13: return "Lost Communication"; case 14: return "Aborted"; case 15: return "Dormant";
        case 16: return "Supporting Entity in Error"; case 17: return "Completed"; case 18: return "Power Mode";
        case 0xD010: return "Online"; case 0xD011: return "Not Ready"; case 0xD012: return "No Media";
        case 0xD013: return "Offline"; case 0xD014: return "Failed"; case 0xD00C: return "Detached";
        case 0xD00D: return "Incomplete"; case 0xD002: return "No Redundancy";
        default: return "Code " + std::to_string(v);
    }
}
std::string msft_usage(int v) {
    switch (v) { case 1: return "Auto-Select"; case 2: return "Manual-Select"; case 3: return "Hot Spare";
                 case 4: return "Retired"; case 5: return "Journal"; default: return "Unknown"; }
}
std::string msft_media(int v) {
    switch (v) { case 3: return "HDD"; case 4: return "SSD"; case 5: return "SCM"; default: return "Unspecified"; }
}
std::string msft_bus(int v) {
    switch (v) {
        case 1: return "SCSI"; case 2: return "ATAPI"; case 3: return "ATA"; case 4: return "1394"; case 5: return "SSA";
        case 6: return "Fibre Channel"; case 7: return "USB"; case 8: return "RAID"; case 9: return "iSCSI"; case 10: return "SAS";
        case 11: return "SATA"; case 12: return "SD"; case 13: return "MMC"; case 14: return "Virtual";
        case 15: return "File Backed Virtual"; case 16: return "Storage Spaces"; case 17: return "NVMe"; default: return "Unknown";
    }
}
std::string msft_job_state(int v) {
    switch (v) { case 2: return "New"; case 3: return "Starting"; case 4: return "Running"; case 5: return "Suspended";
                 case 6: return "Shutting Down"; case 7: return "Completed"; case 8: return "Terminated"; case 9: return "Killed";
                 case 10: return "Exception"; case 11: return "Service"; default: return "Unknown"; }
}

static const char* PS_SPACES = R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$pools = @(Get-StoragePool -IsPrimordial $false | ForEach-Object { [pscustomobject]@{
  FriendlyName = $_.FriendlyName; ObjectId = $_.ObjectId; HealthStatus = [string]$_.HealthStatus;
  OperationalStatus = ([string[]]$_.OperationalStatus) -join ', '; Size = $_.Size; IsPrimordial = $_.IsPrimordial } })
$vds = @(Get-StoragePool -IsPrimordial $false | ForEach-Object { $p = $_.FriendlyName; $_ | Get-VirtualDisk | ForEach-Object {
  [pscustomobject]@{ FriendlyName = $_.FriendlyName; PoolFriendlyName = $p; HealthStatus = [string]$_.HealthStatus;
  OperationalStatus = ([string[]]$_.OperationalStatus) -join ', '; ResiliencySettingName = $_.ResiliencySettingName; Size = $_.Size } } })
$pds = @(Get-StoragePool -IsPrimordial $false | ForEach-Object { $p = $_.FriendlyName; $_ | Get-PhysicalDisk | ForEach-Object {
  [pscustomobject]@{ DeviceId = $_.DeviceId; FriendlyName = $_.FriendlyName; SerialNumber = $_.SerialNumber; PoolFriendlyName = $p;
  HealthStatus = [string]$_.HealthStatus; OperationalStatus = ([string[]]$_.OperationalStatus) -join ', ';
  Usage = [string]$_.Usage; Size = $_.Size } } })
$jobs = @(Get-StorageJob | ForEach-Object { [pscustomobject]@{ Name = $_.Name; JobState = [string]$_.JobState;
  PercentComplete = $_.PercentComplete; IsBackgroundTask = $_.IsBackgroundTask } })
[pscustomobject]@{ StoragePools = $pools; VirtualDisks = $vds; PhysicalDisks = $pds; StorageJobs = $jobs } | ConvertTo-Json -Depth 5 -Compress
)PS";

}  // namespace hdmon

#if defined(_WIN32)
#include <windows.h>
#include <oleauto.h>
#include <wbemidl.h>

namespace hdmon {
namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

json from_variant(const VARIANT& v) {
    if (v.vt & VT_ARRAY) {
        json a = json::array();
        SAFEARRAY* sa = v.parray;
        if (!sa) return a;
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(sa, 1, &lo);
        SafeArrayGetUBound(sa, 1, &hi);
        VARTYPE t = v.vt & ~VT_ARRAY;
        for (LONG i = lo; i <= hi; ++i) {
            if (t == VT_BSTR) { BSTR b = nullptr; SafeArrayGetElement(sa, &i, &b); a.push_back(narrow(b)); SysFreeString(b); }
            else if (t == VT_I4 || t == VT_UI4) { LONG x = 0; SafeArrayGetElement(sa, &i, &x); a.push_back(static_cast<int64_t>(x)); }
            else if (t == VT_I2 || t == VT_UI2) { SHORT x = 0; SafeArrayGetElement(sa, &i, &x); a.push_back(static_cast<int64_t>(static_cast<USHORT>(x))); }
            else if (t == VT_UI1) { BYTE x = 0; SafeArrayGetElement(sa, &i, &x); a.push_back(static_cast<int64_t>(x)); }
        }
        return a;
    }
    switch (v.vt) {
        case VT_BSTR: return narrow(v.bstrVal);
        case VT_BOOL: return v.boolVal != VARIANT_FALSE;
        case VT_I1: return static_cast<int64_t>(v.cVal);
        case VT_UI1: return static_cast<int64_t>(v.bVal);
        case VT_I2: return static_cast<int64_t>(v.iVal);
        case VT_UI2: return static_cast<int64_t>(v.uiVal);
        case VT_I4: return static_cast<int64_t>(v.lVal);
        case VT_UI4: return static_cast<int64_t>(v.ulVal);
        case VT_I8: return static_cast<int64_t>(v.llVal);
        case VT_UI8: return static_cast<int64_t>(v.ullVal);
        case VT_R4: return static_cast<double>(v.fltVal);
        case VT_R8: return v.dblVal;
        default: return nullptr;
    }
}

struct Com {
    bool ok = false, uninit = false;
    IWbemServices* svc = nullptr;
    Com() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninit = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                             EOAC_NONE, nullptr);   // RPC_E_TOO_LATE is fine: the process already chose
        IWbemLocator* loc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                    reinterpret_cast<void**>(&loc))))
            return;
        BSTR ns = SysAllocString(L"ROOT\\Microsoft\\Windows\\Storage");
        hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
        SysFreeString(ns);
        loc->Release();
        if (FAILED(hr) || !svc) return;
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        ok = true;
    }
    ~Com() {
        if (svc) svc->Release();
        if (uninit) CoUninitialize();
    }
    // Each row: requested properties as JSON (numbers stay numbers; uint64 arrive as strings and are converted).
    std::vector<json> query(const std::string& wql, const std::vector<const wchar_t*>& props) {
        std::vector<json> rows;
        if (!svc) return rows;
        BSTR lang = SysAllocString(L"WQL");
        BSTR q = SysAllocString(widen(wql).c_str());
        IEnumWbemClassObject* en = nullptr;
        HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(lang);
        SysFreeString(q);
        if (FAILED(hr) || !en) return rows;
        while (true) {
            IWbemClassObject* obj = nullptr;
            ULONG got = 0;
            if (en->Next(WBEM_INFINITE, 1, &obj, &got) != WBEM_S_NO_ERROR || got == 0) break;
            json row = json::object();
            for (auto p : props) {
                VARIANT v;
                VariantInit(&v);
                CIMTYPE ct = 0;
                if (SUCCEEDED(obj->Get(p, 0, &v, &ct, nullptr))) {
                    json x = from_variant(v);
                    if ((ct == CIM_UINT64 || ct == CIM_SINT64) && x.is_string()) {
                        try { x = static_cast<int64_t>(std::stoull(x.get<std::string>())); } catch (...) {}
                    }
                    row[narrow(p)] = x;
                }
                VariantClear(&v);
            }
            obj->Release();
            rows.push_back(row);
        }
        en->Release();
        return rows;
    }
};

std::string wql_escape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
    return o;
}

std::string ops(const json& v) {
    std::vector<std::string> parts;
    if (v.is_array()) for (auto& x : v) parts.push_back(msft_operational(static_cast<int>(opt_int(x).value_or(0))));
    else if (auto n = opt_int(v)) parts.push_back(msft_operational(static_cast<int>(*n)));
    return join(parts, ", ");
}
int num(const json& r, const char* k) { return static_cast<int>(opt_int(jget(r, k)).value_or(-1)); }

}  // namespace

json windows_storage_spaces(std::string& how) {
    Com com;
    if (com.ok) {
        how = "WMI";
        json pools = json::array(), vds = json::array(), pds = json::array(), jobs = json::array();
        for (auto& p : com.query("SELECT FriendlyName, ObjectId, HealthStatus, OperationalStatus, Size, IsPrimordial FROM MSFT_StoragePool",
                                 {L"FriendlyName", L"ObjectId", L"HealthStatus", L"OperationalStatus", L"Size", L"IsPrimordial"})) {
            if (truthy(jget(p, "IsPrimordial"))) continue;
            std::string name = jstr(p, "FriendlyName").value_or("");
            std::string oid = jstr(p, "ObjectId").value_or("");
            pools.push_back({{"FriendlyName", name}, {"ObjectId", oid}, {"HealthStatus", msft_health(num(p, "HealthStatus"))},
                             {"OperationalStatus", ops(jget(p, "OperationalStatus"))}, {"Size", jget(p, "Size")}, {"IsPrimordial", false}});
            std::string ref = "{MSFT_StoragePool.ObjectId=\"" + wql_escape(oid) + "\"}";
            for (auto& v : com.query("ASSOCIATORS OF " + ref + " WHERE AssocClass = MSFT_StoragePoolToVirtualDisk",
                                     {L"FriendlyName", L"HealthStatus", L"OperationalStatus", L"ResiliencySettingName", L"Size"}))
                vds.push_back({{"FriendlyName", jget(v, "FriendlyName")}, {"PoolFriendlyName", name},
                               {"HealthStatus", msft_health(num(v, "HealthStatus"))}, {"OperationalStatus", ops(jget(v, "OperationalStatus"))},
                               {"ResiliencySettingName", jget(v, "ResiliencySettingName")}, {"Size", jget(v, "Size")}});
            for (auto& d : com.query("ASSOCIATORS OF " + ref + " WHERE AssocClass = MSFT_StoragePoolToPhysicalDisk",
                                     {L"DeviceId", L"FriendlyName", L"SerialNumber", L"HealthStatus", L"OperationalStatus", L"Usage", L"Size"}))
                pds.push_back({{"DeviceId", jget(d, "DeviceId")}, {"FriendlyName", jget(d, "FriendlyName")},
                               {"SerialNumber", jget(d, "SerialNumber")}, {"PoolFriendlyName", name},
                               {"HealthStatus", msft_health(num(d, "HealthStatus"))}, {"OperationalStatus", ops(jget(d, "OperationalStatus"))},
                               {"Usage", msft_usage(num(d, "Usage"))}, {"Size", jget(d, "Size")}});
        }
        for (auto& j : com.query("SELECT Name, JobState, PercentComplete, IsBackgroundTask FROM MSFT_StorageJob",
                                 {L"Name", L"JobState", L"PercentComplete", L"IsBackgroundTask"}))
            jobs.push_back({{"Name", jget(j, "Name")}, {"JobState", msft_job_state(num(j, "JobState"))},
                            {"PercentComplete", jget(j, "PercentComplete")}, {"IsBackgroundTask", jget(j, "IsBackgroundTask")}});
        return {{"StoragePools", pools}, {"VirtualDisks", vds}, {"PhysicalDisks", pds}, {"StorageJobs", jobs}};
    }
    how = "PowerShell";
    return run_powershell_json(PS_SPACES);
}

json windows_physical_inventory(std::string& how) {
    Com com;
    if (com.ok) {
        how = "WMI";
        json pds = json::array(), parts = json::array(), disks = json::array();
        for (auto& d : com.query("SELECT ObjectId, DeviceId, FriendlyName, SerialNumber, MediaType, BusType, Size, HealthStatus, "
                                 "OperationalStatus, FirmwareVersion FROM MSFT_PhysicalDisk",
                                 {L"ObjectId", L"DeviceId", L"FriendlyName", L"SerialNumber", L"MediaType", L"BusType", L"Size",
                                  L"HealthStatus", L"OperationalStatus", L"FirmwareVersion"})) {
            json rel = nullptr;
            std::string ref = "{MSFT_PhysicalDisk.ObjectId=\"" + wql_escape(jstr(d, "ObjectId").value_or("")) + "\"}";
            auto rc = com.query("ASSOCIATORS OF " + ref + " WHERE AssocClass = MSFT_PhysicalDiskToStorageReliabilityCounter",
                                {L"Temperature", L"TemperatureMax", L"Wear", L"PowerOnHours", L"StartStopCycleCount",
                                 L"ReadErrorsTotal", L"ReadErrorsCorrected", L"ReadErrorsUncorrected", L"WriteErrorsTotal",
                                 L"WriteErrorsCorrected", L"WriteErrorsUncorrected", L"ReadLatencyMax", L"WriteLatencyMax"});
            if (!rc.empty()) rel = rc[0];
            json dev = jget(d, "DeviceId");
            if (dev.is_string()) try { dev = std::stoll(dev.get<std::string>()); } catch (...) {}
            pds.push_back({{"DeviceId", dev}, {"FriendlyName", jget(d, "FriendlyName")}, {"SerialNumber", jget(d, "SerialNumber")},
                           {"MediaType", msft_media(num(d, "MediaType"))}, {"BusType", msft_bus(num(d, "BusType"))},
                           {"Size", jget(d, "Size")}, {"HealthStatus", msft_health(num(d, "HealthStatus"))},
                           {"OperationalStatus", ops(jget(d, "OperationalStatus"))}, {"FirmwareVersion", jget(d, "FirmwareVersion")},
                           {"Reliability", rel}});
        }
        for (auto& p : com.query("SELECT DiskNumber, DriveLetter FROM MSFT_Partition", {L"DiskNumber", L"DriveLetter"})) {
            auto l = opt_int(jget(p, "DriveLetter"));
            if (!l || *l == 0) continue;
            parts.push_back({{"DiskNumber", jget(p, "DiskNumber")}, {"DriveLetter", std::string(1, static_cast<char>(*l))}});
        }
        for (auto& d : com.query("SELECT Number, SerialNumber, FriendlyName, BusType, Size FROM MSFT_Disk",
                                 {L"Number", L"SerialNumber", L"FriendlyName", L"BusType", L"Size"}))
            disks.push_back({{"Number", jget(d, "Number")}, {"SerialNumber", jget(d, "SerialNumber")},
                             {"FriendlyName", jget(d, "FriendlyName")}, {"BusType", msft_bus(num(d, "BusType"))}, {"Size", jget(d, "Size")}});
        return {{"PhysicalDisks", pds}, {"Partitions", parts}, {"Disks", disks}};
    }
    how = "PowerShell";
    return run_powershell_json(PS_PHYSICAL);
}

}  // namespace hdmon
#else
namespace hdmon {
json windows_storage_spaces(std::string& how) { how = "n/a"; (void)PS_SPACES; return json::object(); }
json windows_physical_inventory(std::string& how) { how = "n/a"; return json::object(); }
}  // namespace hdmon
#endif
