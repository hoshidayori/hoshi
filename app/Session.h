#import <Foundation/Foundation.h>

#include "sync.h"

#include <string>

@class HSCredentials;

extern NSNotificationName const HSSessionStateDidChange;
// Posted on the main queue when the server rejects the access token (M_UNKNOWN_TOKEN): the user must sign in again
extern NSNotificationName const HSSessionDidLoseAuthorization;

// Owns the Matrix client. All network calls run on serial queues, because TClient is not
// thread-safe; results come back on the main queue.
@interface HSSession : NSObject

- (instancetype)initWithHomeserver:(NSString*)homeserver;
// Resumes an account saved by an earlier sign-in, without a network call
- (instancetype)initWithCredentials:(HSCredentials*)credentials;

- (void)loginUser:(NSString*)user password:(NSString*)password completion:(void (^)(NSString* error))completion;
// Stops syncing and invalidates the token on the server; completion runs even if the server is unreachable
- (void)logoutWithCompletion:(void (^)(void))completion;

// What to store in the Keychain after a successful sign-in
- (HSCredentials*)credentials;

// Starts the /sync loop. After every round the main-queue copy of the state is replaced and
// HSSessionStateDidChange is posted on the main queue.
- (void)startSyncing;
- (void)stopSyncing;

// The latest state; read it on the main queue only
- (const NHoshi::TSyncState&)state;

- (void)sendText:(NSString*)text toRoom:(const std::string&)roomId completion:(void (^)(NSString* error))completion;

@property (nonatomic, readonly) NSString* userId;

@end
