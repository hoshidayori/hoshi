#import <UIKit/UIKit.h>

@class HSSession;

// Shows the device verification in progress: the request, the emoji to compare, the outcome. Follows the
// session's HSSessionVerificationDidChange.
@interface VerificationViewController : UIViewController

- (instancetype)initWithSession:(HSSession*)session;

@end
