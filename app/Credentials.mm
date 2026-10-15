#import "Credentials.h"

#import <Security/Security.h>

namespace {

#if TARGET_OS_SIMULATOR
// The simulator Keychain answers -34018 (errSecMissingEntitlement) to an ad-hoc signed app, and an app signed with
// a keychain-access-groups entitlement but no provisioning profile is not allowed to launch. A build signed in
// Xcode would have the entitlement; the command-line simulator build keeps the session in a file in the app's
// own Library instead. Device builds never take this path.
NSURL* SessionFile() {
    NSURL* library = [NSFileManager.defaultManager URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask].firstObject;
    return [library URLByAppendingPathComponent:@"hoshi-session.json"];
}

NSData* LoadData() {
    return [NSData dataWithContentsOfURL:SessionFile()];
}

BOOL SaveData(NSData* data) {
    return [data writeToURL:SessionFile() options:NSDataWritingAtomic | NSDataWritingFileProtectionComplete error:nil];
}

void RemoveData() {
    [NSFileManager.defaultManager removeItemAtURL:SessionFile() error:nil];
}
#else
NSString* const Service = @"com.github.hoshidayori.hoshi.session";
NSString* const Account = @"default";

NSDictionary* BaseQuery() {
    return @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: Service,
        (__bridge id)kSecAttrAccount: Account,
    };
}

NSData* LoadData() {
    NSMutableDictionary* query = [BaseQuery() mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (status != errSecSuccess || !result) {
        if (status != errSecItemNotFound) {
            NSLog(@"Hoshi: Keychain load failed with OSStatus %d", static_cast<int>(status));
        }
        return nil;
    }
    return (__bridge_transfer NSData*)result;
}

void RemoveData() {
    SecItemDelete((__bridge CFDictionaryRef)BaseQuery());
}

BOOL SaveData(NSData* data) {
    RemoveData();
    NSMutableDictionary* item = [BaseQuery() mutableCopy];
    item[(__bridge id)kSecValueData] = data;
    // Readable after the first unlock, so the app can resume syncing after a restart; never leaves the device
    item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    const OSStatus status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
    if (status != errSecSuccess) {
        NSLog(@"Hoshi: Keychain save failed with OSStatus %d", static_cast<int>(status));
    }
    return status == errSecSuccess;
}
#endif

} // namespace

@implementation HSCredentials

+ (instancetype)load {
    NSData* data = LoadData();
    if (!data) {
        return nil;
    }
    NSDictionary* fields = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![fields isKindOfClass:[NSDictionary class]]) {
        return nil;
    }
    HSCredentials* credentials = [[HSCredentials alloc] init];
    credentials.homeserver = fields[@"homeserver"];
    credentials.userId = fields[@"user_id"];
    credentials.deviceId = fields[@"device_id"];
    credentials.accessToken = fields[@"access_token"];
    if (![credentials.homeserver isKindOfClass:[NSString class]] || ![credentials.accessToken isKindOfClass:[NSString class]] ||
        ![credentials.userId isKindOfClass:[NSString class]]) {
        return nil;
    }
    if (![credentials.deviceId isKindOfClass:[NSString class]]) {
        credentials.deviceId = @"";
    }
    return credentials;
}

- (BOOL)save {
    NSData* data = [NSJSONSerialization dataWithJSONObject:@{
        @"homeserver": self.homeserver ?: @"",
        @"user_id": self.userId ?: @"",
        @"device_id": self.deviceId ?: @"",
        @"access_token": self.accessToken ?: @"",
    } options:0 error:nil];
    return SaveData(data);
}

+ (void)remove {
    RemoveData();
}

@end
