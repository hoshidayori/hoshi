#import <UIKit/UIKit.h>

#include <string>

@class HSSession;

// A room timeline. Every message body is a non-editable UITextView, so iOS gives it the system text
// selection: handles, the magnifier, Copy, Look Up and Translate on any part of the text.
@interface TimelineViewController : UIViewController

- (instancetype)initWithSession:(HSSession*)session roomId:(const std::string&)roomId;

@end
