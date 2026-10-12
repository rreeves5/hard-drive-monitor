// Native SMART on macOS: IOKit registry for enumeration, IOATASMARTInterface (SATA) and
// IONVMeSMARTInterface (internal NVMe) user clients for the data. Mount points via getmntinfo().
#include "native_smart.hpp"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/ata/ATASMARTLib.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>

#include <cstring>
#include <map>

// IONVMeSMARTInterface: layout from Apple's NVMeSMARTLibExternal.h (IOKit SDK); only the calls we use.
#define HD_kIONVMeSMARTUserClientTypeID                                                                    \
    CFUUIDGetConstantUUIDWithBytes(nullptr, 0xAA, 0x0F, 0xA6, 0xF9, 0xC2, 0xD6, 0x45, 0x7F, 0xB1, 0x0B, \
                                   0x59, 0xA1, 0x32, 0x53, 0x29, 0x2F)
#define HD_kIONVMeSMARTInterfaceID                                                                         \
    CFUUIDGetConstantUUIDWithBytes(nullptr, 0xCC, 0xD1, 0xDB, 0x19, 0xFD, 0x9A, 0x4D, 0xAF, 0xBF, 0x95, \
                                   0x12, 0x45, 0x4B, 0x23, 0x0A, 0xB6)
struct HdNVMeSMARTInterface {
    IUNKNOWN_C_GUTS;
    UInt16 version;
    UInt16 revision;
    IOReturn (*SMARTReadData)(void* self, void* smart_log_512);
    IOReturn (*GetIdentifyData)(void* self, void* id_ctrl_4096, unsigned int ns);
};

namespace hdmon::native {

const char* backend_name() { return "macOS IOKit SMART"; }

std::string permission_hint() { return "the IOKit SMART user client was refused (no extra rights normally needed)"; }

namespace {
struct Obj {
    io_object_t o = 0;
    explicit Obj(io_object_t x = 0) : o(x) {}
    ~Obj() { if (o) IOObjectRelease(o); }
    Obj(const Obj&) = delete;
    Obj& operator=(const Obj&) = delete;
};

std::string cfstr(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return "";
    char buf[512];
    if (CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8)) return trim(buf);
    return "";
}

std::string prop_str(io_registry_entry_t e, const char* key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    std::string s = cfstr(v);
    if (v) CFRelease(v);
    return s;
}

std::optional<int64_t> prop_num(io_registry_entry_t e, const char* key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    std::optional<int64_t> out;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
        int64_t n = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n)) out = n;
    }
    if (v) CFRelease(v);
    return out;
}

bool prop_true(io_registry_entry_t e, const char* key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    bool t = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(v));
    if (v) CFRelease(v);
    return t;
}

std::string dict_str(io_registry_entry_t e, const char* dict, const char* key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, dict, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    std::string s;
    if (v && CFGetTypeID(v) == CFDictionaryGetTypeID()) {
        CFStringRef kk = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
        s = cfstr(CFDictionaryGetValue(static_cast<CFDictionaryRef>(v), kk));
        CFRelease(kk);
    }
    if (v) CFRelease(v);
    return s;
}

io_registry_entry_t parent_of(io_registry_entry_t e) {
    io_registry_entry_t p = 0;
    if (IORegistryEntryGetParentEntry(e, kIOServicePlane, &p) != KERN_SUCCESS) return 0;
    return p;
}

// Whole physical disk: an IOMedia whose provider is an IOBlockStorageDriver (APFS/RAID synthesized disks are not).
bool is_physical_whole(io_registry_entry_t media) {
    if (!IOObjectConformsTo(media, "IOMedia") || !prop_true(media, "Whole")) return false;
    Obj drv(parent_of(media));
    return drv.o && IOObjectConformsTo(drv.o, "IOBlockStorageDriver");
}

io_service_t media_for_bsd(const std::string& bsd) {
    CFMutableDictionaryRef m = IOBSDNameMatching(MACH_PORT_NULL, 0, bsd.c_str());
    return m ? IOServiceGetMatchingService(MACH_PORT_NULL, m) : 0;   // consumes m
}

// BSD name of the physical whole disk under a (possibly synthesized APFS / RAID) volume.
std::string physical_for(const std::string& bsd) {
    io_registry_entry_t e = media_for_bsd(bsd);
    std::string found;
    for (int depth = 0; e && depth < 64; ++depth) {
        if (is_physical_whole(e)) found = prop_str(e, "BSD Name");
        io_registry_entry_t p = parent_of(e);
        IOObjectRelease(e);
        e = p;
        if (!found.empty()) break;
    }
    if (e) IOObjectRelease(e);
    return found;
}

std::map<std::string, std::vector<std::string>> mount_map() {
    std::map<std::string, std::vector<std::string>> out;
    struct statfs* mnts = nullptr;
    int n = getmntinfo(&mnts, MNT_NOWAIT);
    for (int i = 0; i < n; ++i) {
        std::string from = mnts[i].f_mntfromname;
        if (!starts_with(from, "/dev/disk")) continue;
        std::string phys = physical_for(from.substr(5));
        if (!phys.empty()) out[phys].push_back(mnts[i].f_mntonname);
    }
    return out;
}

// The registry entry (the block storage device or one of its providers) that carries the SMART capability.
io_registry_entry_t smart_entry(io_registry_entry_t media, bool& nvme) {
    io_registry_entry_t e = parent_of(media);
    for (int depth = 0; e && depth < 16; ++depth) {
        if (prop_true(e, "NVMe SMART Capable")) { nvme = true; return e; }
        if (prop_true(e, "SMART Capable")) { nvme = false; return e; }
        io_registry_entry_t p = parent_of(e);
        IOObjectRelease(e);
        e = p;
    }
    if (e) IOObjectRelease(e);
    return 0;
}
}  // namespace

std::vector<Device> list_devices() {
    std::vector<Device> out;
    auto mounts = mount_map();
    CFMutableDictionaryRef match = IOServiceMatching("IOMedia");
    CFDictionarySetValue(match, CFSTR("Whole"), kCFBooleanTrue);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &it) != KERN_SUCCESS) return out;
    while (io_object_t m = IOIteratorNext(it)) {
        Obj media(m);
        if (!is_physical_whole(media.o)) continue;
        std::string bsd = prop_str(media.o, "BSD Name");
        if (bsd.empty()) continue;
        Obj drv(parent_of(media.o));
        Obj dev(parent_of(drv.o));
        std::string location = dict_str(dev.o, "Protocol Characteristics", "Physical Interconnect Location");
        std::string interconnect = dict_str(dev.o, "Protocol Characteristics", "Physical Interconnect");
        if (location == "File" || location == "Virtual" || interconnect == "Virtual Interface") continue;   // disk images
        Device d;
        d.name = "/dev/" + bsd;
        d.path = bsd;
        bool nvme = false;
        Obj se(smart_entry(media.o, nvme));
        d.protocol = se.o ? (nvme ? "NVMe" : "ATA") : (interconnect == "PCI-Express" || interconnect == "Apple Fabric" ? "NVMe" : "SCSI");
        if (!interconnect.empty()) d.transport = interconnect;
        std::string product = dict_str(dev.o, "Device Characteristics", "Product Name");
        std::string vendor = dict_str(dev.o, "Device Characteristics", "Vendor Name");
        std::string model = vendor.empty() || starts_with(product, vendor) ? product : vendor + " " + product;
        if (!model.empty()) d.model = model;
        std::string serial = dict_str(dev.o, "Device Characteristics", "Serial Number");
        if (!serial.empty()) d.serial = serial;
        std::string medium = dict_str(dev.o, "Device Characteristics", "Medium Type");
        if (medium == "Solid State") d.rotational = false;
        else if (medium == "Rotational") d.rotational = true;
        d.capacity_bytes = prop_num(media.o, "Size");
        if (auto mi = mounts.find(bsd); mi != mounts.end()) d.volumes = mi->second;
        out.push_back(d);
    }
    IOObjectRelease(it);
    std::sort(out.begin(), out.end(), [](const Device& a, const Device& b) {
        return a.name.size() != b.name.size() ? a.name.size() < b.name.size() : a.name < b.name; });
    return out;
}

ReadResult read_device(const Device& dev) {
    ReadResult r;
    Obj media(media_for_bsd(dev.path));
    if (!media.o) { r.error = "no IOMedia for " + dev.path; return r; }
    bool nvme = false;
    Obj se(smart_entry(media.o, nvme));
    if (!se.o) {
        r.unsupported = true;
        r.error = "no SMART user client for this device (USB/Thunderbolt enclosures usually do not offer one)";
        return r;
    }
    IOCFPlugInInterface** plugin = nullptr;
    SInt32 score = 0;
    CFUUIDRef type = nvme ? HD_kIONVMeSMARTUserClientTypeID : kIOATASMARTUserClientTypeID;
    IOReturn kr = IOCreatePlugInInterfaceForService(se.o, type, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != kIOReturnSuccess || !plugin) {
        r.error = "IOCreatePlugInInterfaceForService failed (0x" + std::to_string(kr) + ")";
        r.permission_denied = kr == kIOReturnNotPermitted || kr == kIOReturnNotPrivileged;
        return r;
    }
    if (nvme) {
        HdNVMeSMARTInterface** smart = nullptr;
        (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(HD_kIONVMeSMARTInterfaceID), reinterpret_cast<LPVOID*>(&smart));
        if (!smart) { IODestroyPlugInInterface(plugin); r.error = "IONVMeSMARTInterface not available"; return r; }
        alignas(8) static thread_local uint8_t id[4096];
        alignas(8) static thread_local uint8_t log[512];
        std::memset(id, 0, sizeof id);
        std::memset(log, 0, sizeof log);
        bool have_id = (*smart)->GetIdentifyData(smart, id, 0) == kIOReturnSuccess;
        IOReturn lr = (*smart)->SMARTReadData(smart, log);
        (*smart)->Release(smart);
        IODestroyPlugInInterface(plugin);
        if (lr != kIOReturnSuccess) { r.error = "NVMe SMARTReadData failed"; return r; }
        r.ok = true;
        r.method = "IOKit NVMe SMART";
        r.data = assemble(dev.name, "NVMe", have_id ? nvme_identify_json(id) : json::object(), nvme_health_json(log));
        return r;
    }
    IOATASMARTInterface** smart = nullptr;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOATASMARTInterfaceID), reinterpret_cast<LPVOID*>(&smart));
    if (!smart) { IODestroyPlugInInterface(plugin); r.error = "IOATASMARTInterface not available"; return r; }
    uint8_t id[512] = {0};
    ATASMARTData data;
    ATASMARTDataThresholds thr;
    std::memset(&data, 0, sizeof data);
    std::memset(&thr, 0, sizeof thr);
    UInt32 got = 0;
    bool have_id = (*smart)->GetATAIdentifyData(smart, id, sizeof id, &got) == kIOReturnSuccess;
    (*smart)->SMARTEnableDisableOperations(smart, true);
    IOReturn dr = (*smart)->SMARTReadData(smart, &data);
    bool have_thr = (*smart)->SMARTReadDataThresholds(smart, &thr) == kIOReturnSuccess;
    Boolean exceeded = false;
    bool have_status = (*smart)->SMARTReturnStatus(smart, &exceeded) == kIOReturnSuccess;
    uint8_t stlog[512] = {0};
    bool have_log = (*smart)->SMARTReadLogAtAddress(smart, 0x06, stlog, sizeof stlog) == kIOReturnSuccess;
    (*smart)->Release(smart);
    IODestroyPlugInInterface(plugin);
    if (dr != kIOReturnSuccess) { r.error = "SMARTReadData failed"; return r; }
    static_assert(sizeof(ATASMARTData) == 512 && sizeof(ATASMARTDataThresholds) == 512, "ATA SMART sector size");
    r.ok = true;
    r.method = "IOKit ATA SMART";
    json ident = have_id ? ata_identify_json(id) : json::object();
    r.data = assemble(dev.name, "ATA", ident,
                      ata_smart_json(reinterpret_cast<const uint8_t*>(&data), have_thr ? reinterpret_cast<const uint8_t*>(&thr) : nullptr,
                                     have_status ? std::optional<bool>(!exceeded) : std::nullopt));
    if (have_log) r.data["ata_smart_self_test_log"] = {{"standard", ata_selftest_log_json(stlog)}};
    return r;
}

SelfTestResult start_self_test(const Device& dev, const std::string& kind) {
    SelfTestResult r;
    Obj media(media_for_bsd(dev.path));
    if (!media.o) { r.error = "no IOMedia for " + dev.path; return r; }
    bool nvme = false;
    Obj se(smart_entry(media.o, nvme));
    if (!se.o) { r.unsupported = true; r.error = "no SMART user client for this device"; return r; }
    if (nvme) { r.unsupported = true; r.error = "macOS IONVMeSMARTInterface has no self-test call"; return r; }
    if (kind != "short" && kind != "long" && kind != "extended") {
        r.unsupported = true;
        r.error = "IOATASMARTInterface only starts short and extended self-tests";
        return r;
    }
    IOCFPlugInInterface** plugin = nullptr;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(se.o, kIOATASMARTUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != kIOReturnSuccess || !plugin) {
        r.error = "IOCreatePlugInInterfaceForService failed";
        r.permission_denied = kr == kIOReturnNotPermitted || kr == kIOReturnNotPrivileged;
        return r;
    }
    IOATASMARTInterface** smart = nullptr;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOATASMARTInterfaceID), reinterpret_cast<LPVOID*>(&smart));
    if (!smart) { IODestroyPlugInInterface(plugin); r.error = "IOATASMARTInterface not available"; return r; }
    IOReturn er = (*smart)->SMARTExecuteOffLineImmediate(smart, kind != "short");
    (*smart)->Release(smart);
    IODestroyPlugInInterface(plugin);
    r.method = "IOKit IOATASMARTInterface SMARTExecuteOffLineImmediate";
    if (er != kIOReturnSuccess) { r.error = "SMARTExecuteOffLineImmediate failed"; return r; }
    r.ok = true;
    return r;
}

}  // namespace hdmon::native
#endif
