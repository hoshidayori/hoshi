#import <Foundation/Foundation.h>

#include "sync.h"

#include <functional>
#include <string>

extern NSNotificationName const HSSessionStateDidChange;

// Owns the Matrix client. All network calls run on one serial queue, because TClient is not
// thread-safe; results come back on the main queue.
@interface HSSession : NSObject

- (instancetype)initWithHomeserver:(NSString*)homeserver;

- (void)loginUser:(NSString*)user password:(NSString*)password completion:(void (^)(NSString* error))completion;

// Starts the /sync loop. After every round the main-queue copy of the state is replaced and
// HSSessionStateDidChange is posted on the main queue.
- (void)startSyncing;
- (void)stopSyncing;

// The latest state; read it on the main queue only
- (const NHoshi::TSyncState&)state;

- (void)sendText:(NSString*)text toRoom:(const std::string&)roomId completion:(void (^)(NSString* error))completion;

@property (nonatomic, readonly) NSString* userId;

@end
