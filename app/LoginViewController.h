#import <UIKit/UIKit.h>

// Homeserver, user name and password; on success hands the session to HSAppRouter
@interface LoginViewController : UIViewController

// Shown under the form, for example after the server signed the user out
@property (nonatomic, copy) NSString* message;

@end
