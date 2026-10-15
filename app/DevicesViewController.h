#import <UIKit/UIKit.h>

@class HSSession;

// The user's other devices and whether this device verified them; tapping one starts a verification
@interface DevicesViewController : UITableViewController

- (instancetype)initWithSession:(HSSession*)session;

@end
