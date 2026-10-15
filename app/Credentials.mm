#import "Credentials.h"

#import <Security/Security.h>

namespace {

NSString* const Service = @"com.github.hoshidayori.hoshi.session";
NSString* const Account = @"default";

NSDictionary* BaseQuery() {
    return @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: Service,
        (__bridge id)kSecAttrAccount: Account,
    };
}

} // namespace

@implementation HSCredentials

+ (instancetype)load {
    NSMutableDictionary* query = [BaseQuery() mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess || !result) {
        return nil;
    }
    NSData* data = (__bridge_transfer NSData*)result;
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
    [HSCredentials remove];
    NSMutableDictionary* item = [BaseQuery() mutableCopy];
    item[(__bridge id)kSecValueData] = data;
    // Readable after the first unlock, so the app can resume syncing after a restart; never leaves the device
    item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    return SecItemAdd((__bridge CFDictionaryRef)item, nullptr) == errSecSuccess;
}

+ (void)remove {
    SecItemDelete((__bridge CFDictionaryRef)BaseQuery());
}

@end
