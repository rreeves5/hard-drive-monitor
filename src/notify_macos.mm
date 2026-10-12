// macOS desktop notification via the UserNotifications framework.
// UNUserNotificationCenter only works for code running inside an app bundle (it needs a bundle identifier and
// the user's permission), so the plain `hdmon` CLI returns false here and alerts.cpp falls back to osascript.
#include "osnative.hpp"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

namespace hdmon::osn {

bool notify(const std::string& title, const std::string& body, std::string& how_or_err) {
    @autoreleasepool {
        NSString* bid = [[NSBundle mainBundle] bundleIdentifier];
        if (bid == nil || [[[NSBundle mainBundle] bundlePath] hasSuffix:@".app"] == NO) {
            how_or_err = "not running inside an app bundle (UserNotifications needs one)";
            return false;
        }
        if (@available(macOS 10.14, *)) {
            UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            __block BOOL granted = NO;
            [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                  completionHandler:^(BOOL ok, NSError* _Nullable) {
                                      granted = ok;
                                      dispatch_semaphore_signal(sem);
                                  }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
            if (!granted) { how_or_err = "notification permission not granted"; return false; }
            UNMutableNotificationContent* c = [[UNMutableNotificationContent alloc] init];
            c.title = [NSString stringWithUTF8String:title.c_str()];
            c.body = [NSString stringWithUTF8String:body.c_str()];
            c.sound = [UNNotificationSound defaultSound];
            UNNotificationRequest* req =
                [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString] content:c trigger:nil];
            __block NSString* errText = nil;
            dispatch_semaphore_t sem2 = dispatch_semaphore_create(0);
            [center addNotificationRequest:req
                     withCompletionHandler:^(NSError* _Nullable e) {
                         if (e) errText = [e localizedDescription];
                         dispatch_semaphore_signal(sem2);
                     }];
            dispatch_semaphore_wait(sem2, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
            if (errText) { how_or_err = [errText UTF8String]; return false; }
            how_or_err = "UserNotifications";
            return true;
        }
        how_or_err = "UserNotifications needs macOS 10.14+";
        return false;
    }
}

}  // namespace hdmon::osn
