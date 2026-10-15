#import <UIKit/UIKit.h>

// The timeline as one text, so that a selection can start in one message and end in another.
//
// Sender names and message bodies are the text; bubbles, times and status icons are drawn around it by
// HSMessageLayoutFragment and are not part of any selection or copy. Each message is one paragraph: line
// breaks inside a message are stored as U+2028 so its bubble stays in one piece, and copying turns them
// back into newlines.

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, HSMessageStatus) {
    HSMessageStatusSent,
    HSMessageStatusSending,
    HSMessageStatusFailed,
};

// What the fragment draws around a message body; set as the HSMessageMetaAttribute of the body paragraph
@interface HSMessageMeta : NSObject
@property (nonatomic, copy) NSString* time;
@property (nonatomic) HSMessageStatus status;
@property (nonatomic) BOOL waitingForKey;
@property (nonatomic, copy) NSString* txnId;  // for a retry of a failed message
@end

extern NSAttributedStringKey const HSMessageMetaAttribute;

// Geometry shared by the paragraph styles and the drawing
extern const CGFloat HSBubbleSide;     // from the screen edge to the bubble
extern const CGFloat HSBubblePadX;     // from the bubble edge to the text
extern const CGFloat HSBubblePadY;
extern const CGFloat HSTimeColumn;     // room right of the widest bubble for the time and the status icon

@interface HSTimelineTextView : UITextView
- (instancetype)init;
// The message whose status icon or time is at the point, if it is a failed one: the transaction id to retry
- (nullable NSString*)failedTxnIdAtPoint:(CGPoint)point;
@end

NS_ASSUME_NONNULL_END
