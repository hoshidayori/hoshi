#import "AppRouter.h"
#import "Credentials.h"
#import "LoginViewController.h"
#import "RoomsViewController.h"
#import "Session.h"

static HSAppRouter* SharedRouter;

@implementation HSAppRouter {
    UIWindow* _window;
    HSSession* _session;
}

+ (HSAppRouter*)shared {
    return SharedRouter;
}

- (instancetype)initWithWindow:(UIWindow*)window {
    if ((self = [super init])) {
        _window = window;
        SharedRouter = self;
    }
    return self;
}

- (void)start {
    HSCredentials* saved = [HSCredentials load];
    if (saved) {
        [self showSession:[[HSSession alloc] initWithCredentials:saved]];
    } else {
        [self showLoginWithMessage:nil];
    }
}

- (void)didSignIn:(HSSession*)session {
    if (![[session credentials] save]) {
        NSLog(@"Hoshi: could not save the session to the Keychain; the next launch will ask to sign in");
    }
    [self showSession:session];
}

- (void)signOut {
    HSSession* session = _session;
    [self endSession];
    [HSCredentials remove];
    [self showLoginWithMessage:nil];
    [session logoutWithCompletion:^{}];
}

- (void)endSession {
    if (_session) {
        [NSNotificationCenter.defaultCenter removeObserver:self name:HSSessionDidLoseAuthorization object:_session];
        [_session stopSyncing];
        _session = nil;
    }
}

- (void)showSession:(HSSession*)session {
    [self endSession];
    _session = session;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(lostAuthorization)
                                               name:HSSessionDidLoseAuthorization object:session];
    [session startSyncing];
    RoomsViewController* rooms = [[RoomsViewController alloc] initWithSession:session];
    _window.rootViewController = [[UINavigationController alloc] initWithRootViewController:rooms];
}

- (void)lostAuthorization {
    // The token was revoked, for example by signing this device out from another client
    [self endSession];
    [HSCredentials remove];
    [self showLoginWithMessage:@"You were signed out. Sign in again."];
}

- (void)showLoginWithMessage:(NSString*)message {
    LoginViewController* login = [[LoginViewController alloc] init];
    login.message = message;
    _window.rootViewController = [[UINavigationController alloc] initWithRootViewController:login];
}

@end
