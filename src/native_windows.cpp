// Native SMART on Windows: DeviceIoControl on \\.\PhysicalDriveN.
//   ATA/SATA: IOCTL_ATA_PASS_THROUGH (IDENTIFY, SMART READ DATA/THRESHOLDS, RETURN STATUS),
//             SMART_RCV_DRIVE_DATA / SMART_SEND_DRIVE_COMMAND as the legacy fallback.
//   NVMe:     IOCTL_STORAGE_QUERY_PROPERTY with StorageDeviceProtocolSpecificProperty (identify + log 02h).
//   Drive letters: IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS on \\.\C: etc.
#include "native_smart.hpp"   // before <windows.h>: it defines `interface` as a macro

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <cstddef>
#include <map>

namespace hdmon::native {

const char* backend_name() { return "Windows DeviceIoControl"; }

std::string permission_hint() {
    return "native SMART needs Administrator rights: run hdmon from an elevated terminal "
           "(the desktop app asks for elevation itself)";
}

namespace {
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD error = 0;
    Handle(const std::wstring& path, DWORD access) {
        h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) error = GetLastError();
    }
    ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

std::wstring drive_path(int n) { return L"\\\\.\\PhysicalDrive" + std::to_wstring(n); }

std::string desc_string(const std::vector<uint8_t>& buf, DWORD off) {
    if (!off || off >= buf.size()) return "";
    std::string s;
    for (size_t i = off; i < buf.size() && buf[i]; ++i) s += static_cast<char>(buf[i]);
    return trim(s);
}

std::map<int, std::vector<std::string>> letter_map() {
    std::map<int, std::vector<std::string>> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        std::wstring vol = L"\\\\.\\" + std::wstring(1, wchar_t(L'A' + i)) + L":";
        Handle h(vol, 0);
        if (!h.ok()) continue;
        std::vector<uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT));
        DWORD ret = 0;
        if (!DeviceIoControl(h.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf.data(), (DWORD)buf.size(), &ret, nullptr))
            continue;
        auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
        for (DWORD k = 0; k < ext->NumberOfDiskExtents && k < 16; ++k)
            out[static_cast<int>(ext->Extents[k].DiskNumber)].push_back(std::string(1, char('A' + i)) + ":");
    }
    return out;
}
}  // namespace

std::vector<Device> list_devices() {
    std::vector<Device> out;
    auto letters = letter_map();
    for (int n = 0; n < 64; ++n) {
        Handle h(drive_path(n), 0);
        if (!h.ok()) continue;
        Device d;
        d.name = "PhysicalDrive" + std::to_string(n);
        d.protocol = "Unknown";
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        std::vector<uint8_t> buf(4096);
        DWORD ret = 0;
        if (DeviceIoControl(h.h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), (DWORD)buf.size(), &ret, nullptr)) {
            auto* sd = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
            std::string vendor = desc_string(buf, sd->VendorIdOffset), product = desc_string(buf, sd->ProductIdOffset);
            std::string serial = desc_string(buf, sd->SerialNumberOffset);
            std::string model = (vendor.empty() || vendor == "ATA" || starts_with(product, vendor)) ? product : vendor + " " + product;
            if (!model.empty()) d.model = model;
            if (!serial.empty()) d.serial = serial;
            switch (sd->BusType) {
                case BusTypeNvme: d.protocol = "NVMe"; d.transport = "NVMe"; break;
                case BusTypeAta: case BusTypeSata: d.protocol = "ATA"; d.transport = "SATA"; break;
                case BusTypeUsb: d.protocol = "ATA"; d.transport = "USB"; break;   // try SAT; may be refused
                case BusTypeRAID: d.protocol = "RAID"; d.transport = "RAID"; d.virtual_disk = true; break;
                case BusTypeSpaces: d.protocol = "Spaces"; d.transport = "Storage Spaces"; d.virtual_disk = true; break;
                case BusTypeSas: d.protocol = "SCSI"; d.transport = "SAS"; break;
                case BusTypeVirtual: case BusTypeFileBackedVirtual: d.transport = "Virtual"; d.virtual_disk = true; break;
                default: d.protocol = "SCSI"; d.transport = "SCSI"; break;
            }
        }
        DISK_GEOMETRY_EX geo{};
        if (DeviceIoControl(h.h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo, &ret, nullptr))
            d.capacity_bytes = geo.DiskSize.QuadPart;
        STORAGE_PROPERTY_QUERY sq{};
        sq.PropertyId = StorageDeviceSeekPenaltyProperty;
        sq.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
        if (DeviceIoControl(h.h, IOCTL_STORAGE_QUERY_PROPERTY, &sq, sizeof sq, &sp, sizeof sp, &ret, nullptr) && ret >= sizeof sp)
            d.rotational = sp.IncursSeekPenalty != FALSE;
        if (auto it = letters.find(n); it != letters.end()) d.volumes = it->second;
        d.path = "\\\\.\\PhysicalDrive" + std::to_string(n);
        out.push_back(d);
    }
    return out;
}

namespace {
struct AptBuf {
    ATA_PASS_THROUGH_EX apt;
    ULONG filler;
    UCHAR data[512];
};

// IOCTL_ATA_PASS_THROUGH. tf = features, count, lba low, mid, high, device, command. Returns DeviceIoControl success.
bool ata_pt(HANDLE h, const uint8_t tf[7], uint8_t* buf, uint8_t* mid, uint8_t* high, DWORD& err) {
    AptBuf s{};
    s.apt.Length = sizeof(ATA_PASS_THROUGH_EX);
    s.apt.TimeOutValue = 10;
    s.apt.AtaFlags = ATA_FLAGS_DRDY_REQUIRED | (buf ? ATA_FLAGS_DATA_IN : 0);
    s.apt.DataTransferLength = buf ? 512 : 0;
    s.apt.DataBufferOffset = buf ? offsetof(AptBuf, data) : 0;
    for (int i = 0; i < 7; ++i) s.apt.CurrentTaskFile[i] = tf[i];
    DWORD ret = 0, in_len = buf ? sizeof s : sizeof(ATA_PASS_THROUGH_EX);
    if (!DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH, &s, in_len, &s, in_len, &ret, nullptr)) {
        err = GetLastError();
        return false;
    }
    if (s.apt.CurrentTaskFile[6] & 0x01) { err = ERROR_NOT_SUPPORTED; return false; }   // ERR bit
    if (buf) memcpy(buf, s.data, 512);
    if (mid) *mid = s.apt.CurrentTaskFile[3];
    if (high) *high = s.apt.CurrentTaskFile[4];
    return true;
}

// Legacy SMART IOCTLs (SENDCMDINPARAMS). cmd = 0xEC (IDENTIFY) or 0xB0 with feature D0/D1/DA.
bool smart_ioctl(HANDLE h, uint8_t cmd, uint8_t feature, uint8_t* buf, uint8_t* mid, uint8_t* high, DWORD& err) {
    SENDCMDINPARAMS in{};
    in.cBufferSize = buf ? 512 : 0;
    in.irDriveRegs.bFeaturesReg = feature;
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = 1;
    in.irDriveRegs.bCylLowReg = cmd == 0xB0 ? 0x4F : 0;
    in.irDriveRegs.bCylHighReg = cmd == 0xB0 ? 0xC2 : 0;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = cmd;
    std::vector<uint8_t> out(sizeof(SENDCMDOUTPARAMS) + 512);
    DWORD ret = 0;
    DWORD code = buf ? SMART_RCV_DRIVE_DATA : SMART_SEND_DRIVE_COMMAND;
    if (!DeviceIoControl(h, code, &in, sizeof in - 1, out.data(), (DWORD)out.size(), &ret, nullptr)) {
        err = GetLastError();
        return false;
    }
    auto* o = reinterpret_cast<SENDCMDOUTPARAMS*>(out.data());
    if (buf) memcpy(buf, o->bBuffer, 512);
    if (mid) { *mid = o->bBuffer[3]; *high = o->bBuffer[4]; }
    return true;
}

std::string win_error(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string s;
    if (msg) {
        int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
        s.resize(n > 0 ? n - 1 : 0);
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, msg, -1, &s[0], n, nullptr, nullptr);
        LocalFree(msg);
    }
    return trim(s) + " (" + std::to_string(e) + ")";
}

ReadResult read_ata(const Device& dev, int n) {
    ReadResult r;
    Handle h(drive_path(n), GENERIC_READ | GENERIC_WRITE);
    if (!h.ok()) {
        r.error = "open " + dev.path + ": " + win_error(h.error);
        r.permission_denied = h.error == ERROR_ACCESS_DENIED;
        return r;
    }
    uint8_t id[512] = {0}, data[512] = {0}, thr[512] = {0}, mid = 0, high = 0;
    DWORD err = 0;
    const uint8_t ident_tf[7] = {0, 1, 0, 0, 0, 0xA0, 0xEC};
    bool legacy = false;
    if (!ata_pt(h.h, ident_tf, id, nullptr, nullptr, err)) {
        DWORD err2 = 0;
        if (!smart_ioctl(h.h, 0xEC, 0, id, nullptr, nullptr, err2)) {
            r.error = "ATA pass-through: " + win_error(err);
            r.permission_denied = err == ERROR_ACCESS_DENIED;
            r.unsupported = !r.permission_denied;
            return r;
        }
        legacy = true;
    }
    r.method = legacy ? "SMART_RCV_DRIVE_DATA" : "IOCTL_ATA_PASS_THROUGH";
    json ident = ata_identify_json(id);
    if (!truthy(jget(jget(ident, "smart_support"), "available"))) {
        r.ok = true;
        r.data = assemble(dev.name, "ATA", ident, json::object());
        r.data["smart_support"] = {{"available", false}};
        return r;
    }
    const uint8_t rd_tf[7] = {0xD0, 1, 0, 0x4F, 0xC2, 0xA0, 0xB0};
    const uint8_t th_tf[7] = {0xD1, 1, 0, 0x4F, 0xC2, 0xA0, 0xB0};
    const uint8_t st_tf[7] = {0xDA, 0, 0, 0x4F, 0xC2, 0xA0, 0xB0};
    bool ok_d, ok_t, ok_s;
    if (!legacy) {
        ok_d = ata_pt(h.h, rd_tf, data, nullptr, nullptr, err);
        ok_t = ata_pt(h.h, th_tf, thr, nullptr, nullptr, err);
        ok_s = ata_pt(h.h, st_tf, nullptr, &mid, &high, err);
    } else {
        ok_d = smart_ioctl(h.h, 0xB0, 0xD0, data, nullptr, nullptr, err);
        ok_t = smart_ioctl(h.h, 0xB0, 0xD1, thr, nullptr, nullptr, err);
        ok_s = smart_ioctl(h.h, 0xB0, 0xDA, nullptr, &mid, &high, err);
    }
    uint8_t stlog[512] = {0};
    const uint8_t log_tf[7] = {0xD5, 1, 0x06, 0x4F, 0xC2, 0xA0, 0xB0};   // SMART READ LOG 06h (self-test log)
    DWORD lerr = 0;
    bool ok_l = !legacy && ata_pt(h.h, log_tf, stlog, nullptr, nullptr, lerr);
    if (!ok_d) {
        r.error = "SMART READ DATA: " + win_error(err);
        r.permission_denied = err == ERROR_ACCESS_DENIED;
        return r;
    }
    r.ok = true;
    r.data = assemble(dev.name, "ATA", ident,
                      ata_smart_json(data, ok_t ? thr : nullptr, ok_s ? ata_status_from_lba(mid, high) : std::nullopt));
    if (ok_l) r.data["ata_smart_self_test_log"] = {{"standard", ata_selftest_log_json(stlog)}};
    return r;
}

bool nvme_query(HANDLE h, STORAGE_PROTOCOL_NVME_DATA_TYPE type, DWORD value, DWORD len, std::vector<uint8_t>& out, DWORD& err) {
    size_t hdr = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
    std::vector<uint8_t> buf(hdr + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + len);
    auto* q = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buf.data());
    auto* p = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(q->AdditionalParameters);
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    p->ProtocolType = ProtocolTypeNvme;
    p->DataType = type;
    p->ProtocolDataRequestValue = value;
    p->ProtocolDataRequestSubValue = 0;
    p->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    p->ProtocolDataLength = len;
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf.data(), (DWORD)buf.size(), buf.data(), (DWORD)buf.size(), &ret, nullptr)) {
        err = GetLastError();
        return false;
    }
    auto* d = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(buf.data());
    if (d->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) || d->Size != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) {
        err = ERROR_INVALID_DATA;
        return false;
    }
    auto* pd = &d->ProtocolSpecificData;
    if (pd->ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) || pd->ProtocolDataLength < len) {
        err = ERROR_INVALID_DATA;
        return false;
    }
    const uint8_t* src = reinterpret_cast<const uint8_t*>(pd) + pd->ProtocolDataOffset;
    if (src + len > buf.data() + buf.size()) { err = ERROR_INVALID_DATA; return false; }
    out.assign(src, src + len);
    return true;
}

ReadResult read_nvme(const Device& dev, int n) {
    ReadResult r;
    Handle h(drive_path(n), GENERIC_READ | GENERIC_WRITE);
    Handle h0(drive_path(n), 0);   // the protocol query also works without read/write access on recent Windows
    HANDLE use = h.ok() ? h.h : h0.h;
    if (use == INVALID_HANDLE_VALUE) {
        r.error = "open " + dev.path + ": " + win_error(h.error);
        r.permission_denied = h.error == ERROR_ACCESS_DENIED;
        return r;
    }
    std::vector<uint8_t> id, log;
    DWORD err = 0, err_id = 0;
    bool have_id = nvme_query(use, NVMeDataTypeIdentify, 1 /* CNS controller */, 4096, id, err_id);
    if (!nvme_query(use, NVMeDataTypeLogPage, 2 /* SMART / health */, 512, log, err)) {
        r.error = "NVMe health log: " + win_error(err);
        r.permission_denied = err == ERROR_ACCESS_DENIED;
        r.unsupported = err == ERROR_NOT_SUPPORTED || err == ERROR_INVALID_FUNCTION;
        return r;
    }
    r.ok = true;
    r.method = "IOCTL_STORAGE_QUERY_PROPERTY NVMe";
    r.data = assemble(dev.name, "NVMe", have_id ? nvme_identify_json(id.data()) : json::object(), nvme_health_json(log.data()));
    std::vector<uint8_t> stlog;
    DWORD serr = 0;
    if (nvme_query(use, NVMeDataTypeLogPage, 6 /* Device Self-test */, 564, stlog, serr) && stlog.size() >= 564)
        r.data["nvme_self_test_log"] = nvme_selftest_log_json(stlog.data());
    return r;
}
}  // namespace

SelfTestResult start_self_test(const Device& dev, const std::string& kind) {
    SelfTestResult r;
    int n = std::atoi(dev.name.c_str() + std::string("PhysicalDrive").size());
    Handle h(drive_path(n), GENERIC_READ | GENERIC_WRITE);
    if (!h.ok()) {
        r.error = "open " + dev.path + ": " + win_error(h.error);
        r.permission_denied = h.error == ERROR_ACCESS_DENIED;
        return r;
    }
    DWORD err = 0;
    if (dev.protocol == "NVMe") {
        auto code = nvme_selftest_code(kind);
        if (!code) { r.unsupported = true; r.error = "NVMe has no '" + kind + "' self-test"; return r; }
        size_t off = offsetof(STORAGE_PROTOCOL_COMMAND, Command);
        std::vector<uint8_t> buf(off + STORAGE_PROTOCOL_COMMAND_LENGTH_NVME + 64, 0);
        auto* pc = reinterpret_cast<STORAGE_PROTOCOL_COMMAND*>(buf.data());
        pc->Version = STORAGE_PROTOCOL_STRUCTURE_VERSION;
        pc->Length = sizeof(STORAGE_PROTOCOL_COMMAND);
        pc->ProtocolType = ProtocolTypeNvme;
        pc->Flags = STORAGE_PROTOCOL_COMMAND_FLAG_ADAPTER_REQUEST;
        pc->CommandLength = STORAGE_PROTOCOL_COMMAND_LENGTH_NVME;
        pc->ErrorInfoLength = 64;
        pc->ErrorInfoOffset = static_cast<DWORD>(off + STORAGE_PROTOCOL_COMMAND_LENGTH_NVME);
        pc->TimeOutValue = 30;
        pc->CommandSpecific = STORAGE_PROTOCOL_SPECIFIC_NVME_ADMIN_COMMAND;
        uint32_t* cmd = reinterpret_cast<uint32_t*>(buf.data() + off);   // NVMe SQE: CDW0 opcode, CDW1 NSID, CDW10
        cmd[0] = 0x14;
        cmd[1] = 0xFFFFFFFF;
        cmd[10] = *code;
        DWORD ret = 0;
        r.method = "IOCTL_STORAGE_PROTOCOL_COMMAND (NVMe Device Self-test 14h)";
        if (!DeviceIoControl(h.h, IOCTL_STORAGE_PROTOCOL_COMMAND, buf.data(), (DWORD)buf.size(), buf.data(), (DWORD)buf.size(), &ret, nullptr)) {
            err = GetLastError();
            r.error = win_error(err);
            r.permission_denied = err == ERROR_ACCESS_DENIED;
            r.unsupported = err == ERROR_NOT_SUPPORTED || err == ERROR_INVALID_FUNCTION || err == ERROR_INVALID_PARAMETER;
            return r;
        }
        if (pc->ReturnStatus != STORAGE_PROTOCOL_STATUS_SUCCESS) {
            r.error = "NVMe command status " + std::to_string(pc->ReturnStatus) + " (error code " + std::to_string(pc->ErrorCode) + ")";
            r.unsupported = pc->ReturnStatus == STORAGE_PROTOCOL_STATUS_NOT_SUPPORTED || pc->ReturnStatus == STORAGE_PROTOCOL_STATUS_INVALID_REQUEST;
            return r;
        }
        r.ok = true;
        return r;
    }
    auto sub = ata_selftest_subcommand(kind);
    if (!sub) { r.unsupported = true; r.error = "unknown self-test '" + kind + "'"; return r; }
    const uint8_t tf[7] = {0xD4, 0, *sub, 0x4F, 0xC2, 0xA0, 0xB0};   // features, count, LBA low = subcommand, mid, high
    r.method = "IOCTL_ATA_PASS_THROUGH SMART EXECUTE OFF-LINE IMMEDIATE";
    if (ata_pt(h.h, tf, nullptr, nullptr, nullptr, err)) { r.ok = true; return r; }
    // legacy SMART_SEND_DRIVE_COMMAND (EXECUTE_OFFLINE_DIAGS), subcommand in the sector-number register
    SENDCMDINPARAMS in{};
    in.irDriveRegs.bFeaturesReg = 0xD4;
    in.irDriveRegs.bSectorNumberReg = *sub;
    in.irDriveRegs.bCylLowReg = 0x4F;
    in.irDriveRegs.bCylHighReg = 0xC2;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = 0xB0;
    std::vector<uint8_t> out(sizeof(SENDCMDOUTPARAMS) + 16);
    DWORD ret = 0, err2 = 0;
    if (DeviceIoControl(h.h, SMART_SEND_DRIVE_COMMAND, &in, sizeof in - 1, out.data(), (DWORD)out.size(), &ret, nullptr)) {
        r.ok = true;
        r.method = "SMART_SEND_DRIVE_COMMAND EXECUTE_OFFLINE_DIAGS";
        return r;
    }
    err2 = GetLastError();
    r.error = "ATA pass-through: " + win_error(err) + "; SMART_SEND_DRIVE_COMMAND: " + win_error(err2);
    r.permission_denied = err == ERROR_ACCESS_DENIED || err2 == ERROR_ACCESS_DENIED;
    r.unsupported = !r.permission_denied;
    return r;
}

ReadResult read_device(const Device& dev) {
    int n = std::atoi(dev.name.c_str() + std::string("PhysicalDrive").size());
    if (dev.protocol == "NVMe") return read_nvme(dev, n);
    if (dev.virtual_disk) {
        ReadResult r;
        r.unsupported = true;
        r.error = dev.protocol == "Spaces" ? "Storage Spaces virtual disk (health comes from the pool)"
                                            : "RAID / virtual volume (no SMART of its own)";
        return r;
    }
    return read_ata(dev, n);
}

}  // namespace hdmon::native
#endif
