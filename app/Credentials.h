#import <Foundation/Foundation.h>

// A signed-in account, kept in the Keychain so the app does not ask for the password on every launch.
// Only the access token is stored, never the password.
@interface HSCredentials : NSObject

@property (nonatomic, copy) NSString* homeserver;
@property (nonatomic, copy) NSString* userId;
@property (nonatomic, copy) NSString* deviceId;
@property (nonatomic, copy) NSString* accessToken;

// Returns nil when nothing is stored or the stored item cannot be read
+ (instancetype)load;
- (BOOL)save;
+ (void)remove;

@end
