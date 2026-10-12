// launchd through the ServiceManagement framework instead of running launchctl.
// SMJobSubmit / SMJobRemove are marked deprecated (Apple points to SMAppService, which needs an app bundle and
// macOS 13), but they remain the only public C API for a plain CLI to load a per-user agent into the running session.
#include "osnative.hpp"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <ServiceManagement/ServiceManagement.h>

#include "util.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace hdmon::osn {
namespace {
std::string cf_err(CFErrorRef e) {
    if (!e) return "unknown error";
    CFStringRef d = CFErrorCopyDescription(e);
    char buf[512] = {0};
    if (d) { CFStringGetCString(d, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(d); }
    std::string s = buf;
    s += " (code " + std::to_string(static_cast<long>(CFErrorGetCode(e))) + ")";
    CFRelease(e);
    return s;
}
CFStringRef cfstr(const std::string& s) { return CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8); }
}  // namespace

bool launchd_submit_plist(const std::string& plist_path, std::string& err) {
    std::string data = read_file(plist_path);
    if (data.empty()) { err = "cannot read " + plist_path; return false; }
    CFDataRef d = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(data.data()), static_cast<CFIndex>(data.size()));
    CFErrorRef perr = nullptr;
    CFPropertyListRef pl = CFPropertyListCreateWithData(nullptr, d, kCFPropertyListImmutable, nullptr, &perr);
    CFRelease(d);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
        if (pl) CFRelease(pl);
        err = "bad plist: " + cf_err(perr);
        return false;
    }
    auto dict = static_cast<CFDictionaryRef>(pl);
    auto label = static_cast<CFStringRef>(CFDictionaryGetValue(dict, CFSTR("Label")));
    if (label) {
        CFErrorRef rerr = nullptr;
        SMJobRemove(kSMDomainUserLaunchd, label, nullptr, true, &rerr);   // replace an older copy; absent is fine
        if (rerr) CFRelease(rerr);
    }
    CFErrorRef serr = nullptr;
    Boolean ok = SMJobSubmit(kSMDomainUserLaunchd, dict, nullptr, &serr);
    CFRelease(pl);
    if (!ok) { err = "SMJobSubmit: " + cf_err(serr); return false; }
    return true;
}

bool launchd_remove(const std::string& label, std::string& err) {
    CFStringRef l = cfstr(label);
    CFErrorRef e = nullptr;
    Boolean ok = SMJobRemove(kSMDomainUserLaunchd, l, nullptr, true, &e);
    CFRelease(l);
    if (!ok) {
        // kSMErrorJobNotFound: nothing loaded, which is what we wanted
        if (e && CFErrorGetCode(e) == kSMErrorJobNotFound) { CFRelease(e); return true; }
        err = "SMJobRemove: " + cf_err(e);
        return false;
    }
    return true;
}

int launchd_loaded(const std::string& label) {
    CFStringRef l = cfstr(label);
    CFDictionaryRef d = SMJobCopyDictionary(kSMDomainUserLaunchd, l);
    CFRelease(l);
    if (!d) return 0;
    CFRelease(d);
    return 1;
}

}  // namespace hdmon::osn
#pragma clang diagnostic pop
#else
namespace hdmon::osn {
bool launchd_submit_plist(const std::string&, std::string& err) { err = "launchd is macOS-only"; return false; }
bool launchd_remove(const std::string&, std::string& err) { err = "launchd is macOS-only"; return false; }
int launchd_loaded(const std::string&) { return -1; }
}  // namespace hdmon::osn
#endif
