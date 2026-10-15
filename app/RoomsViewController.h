#import <UIKit/UIKit.h>

@class HSSession;

// Joined rooms, updated after every /sync round; a tap opens the timeline
@interface RoomsViewController : UITableViewController

- (instancetype)initWithSession:(HSSession*)session;

@end
