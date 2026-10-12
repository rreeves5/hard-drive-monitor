// AppleRAID sets and members read from the IOKit registry (no diskutil).
#include "raid.hpp"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

namespace hdmon {
namespace {
json cf_to_json(CFTypeRef v) {
    if (!v) return nullptr;
    CFTypeID t = CFGetTypeID(v);
    if (t == CFStringGetTypeID()) {
        char buf[1024] = {0};
        CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8);
        return std::string(buf);
    }
    if (t == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(v)) != 0;
    if (t == CFNumberGetTypeID()) {
        int64_t n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n);
        return n;
    }
    if (t == CFArrayGetTypeID()) {
        json a = json::array();
        auto arr = static_cast<CFArrayRef>(v);
        for (CFIndex i = 0; i < CFArrayGetCount(arr); ++i) a.push_back(cf_to_json(CFArrayGetValueAtIndex(arr, i)));
        return a;
    }
    if (t == CFDictionaryGetTypeID()) {
        json o = json::object();
        auto d = static_cast<CFDictionaryRef>(v);
        CFIndex n = CFDictionaryGetCount(d);
        std::vector<const void*> keys(static_cast<size_t>(n)), vals(static_cast<size_t>(n));
        CFDictionaryGetKeysAndValues(d, keys.data(), vals.data());
        for (CFIndex i = 0; i < n; ++i) {
            json k = cf_to_json(keys[static_cast<size_t>(i)]);
            if (k.is_string()) o[k.get<std::string>()] = cf_to_json(vals[static_cast<size_t>(i)]);
        }
        return o;
    }
    return nullptr;
}
json props(io_registry_entry_t e) {
    CFMutableDictionaryRef d = nullptr;
    if (IORegistryEntryCreateCFProperties(e, &d, kCFAllocatorDefault, 0) != KERN_SUCCESS || !d) return json::object();
    json j = cf_to_json(d);
    CFRelease(d);
    // keep only what the parser needs (the registry also holds large binary blobs)
    json out = json::object();
    for (auto& [k, v] : j.items())
        if (k.rfind("AppleRAID-", 0) == 0 || k == "BSD Name" || k == "Size") out[k] = v;
    return out;
}
// First IOMedia below (set -> its exported volume) or above (member -> its partition) the entry.
void media_info(io_registry_entry_t e, bool children, json& into) {
    io_iterator_t it = 0;
    if (children) {
        if (IORegistryEntryGetChildIterator(e, kIOServicePlane, &it) != KERN_SUCCESS) return;
    } else {
        if (IORegistryEntryGetParentIterator(e, kIOServicePlane, &it) != KERN_SUCCESS) return;
    }
    for (io_registry_entry_t c; (c = IOIteratorNext(it));) {
        if (IOObjectConformsTo(c, "IOMedia")) {
            json p = props(c);
            if (p.contains("BSD Name")) into["_bsd"] = p["BSD Name"];
            if (p.contains("Size")) into["_size"] = p["Size"];
            IOObjectRelease(c);
            break;
        }
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
}
json collect(const char* cls, bool children) {
    json arr = json::array();
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(cls), &it) != KERN_SUCCESS) return arr;
    for (io_service_t s; (s = IOIteratorNext(it));) {
        json p = props(s);
        media_info(s, children, p);
        arr.push_back(p);
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return arr;
}
}  // namespace

json appleraid_ioreg_snapshot(std::string& err) {
    json snap = json::object();
    snap["sets"] = collect("AppleRAIDSet", true);
    snap["members"] = collect("AppleRAIDMember", false);
    err.clear();
    return snap;
}

}  // namespace hdmon
#else
namespace hdmon {
json appleraid_ioreg_snapshot(std::string& err) { err = "AppleRAID is macOS-only"; return json::object(); }
}  // namespace hdmon
#endif
