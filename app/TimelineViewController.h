#import <UIKit/UIKit.h>

// A room timeline. Every message body is a non-editable UITextView, so iOS gives it the system text
// selection: handles, the magnifier, Copy, Look Up and Translate on any part of the text.
@interface TimelineViewController : UITableViewController
@end
