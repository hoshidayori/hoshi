#import <UIKit/UIKit.h>

@class HSSession;

// Switches between the sign-in screen and the signed-in app, and keeps the Keychain in step
@interface HSAppRouter : NSObject

- (instancetype)initWithWindow:(UIWindow*)window;

// Resumes a saved account or shows the sign-in screen
- (void)start;
// Called by the sign-in screen after a successful login
- (void)didSignIn:(HSSession*)session;
- (void)signOut;

+ (HSAppRouter*)shared;

@end
