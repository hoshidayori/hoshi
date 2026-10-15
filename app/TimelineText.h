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

// What the fragment draws left of a sender's name: the avatar; set as the HSSenderMetaAttribute of the name
@interface HSSenderMeta : NSObject
@property (nonatomic, copy) NSString* initial;  // shown until there is a picture
@property (nonatomic, strong) UIColor* color;
@end

extern NSAttributedStringKey const HSSenderMetaAttribute;

// Geometry shared by the paragraph styles and the drawing
extern const CGFloat HSBubbleSide;     // from the screen edges to the avatar and to the widest bubble
extern const CGFloat HSAvatarSize;
// From the avatar's left edge to the bubble. Below a series' avatar the column holds each message's time, or
// its status icon, so the bubbles can use the whole width to the right.
extern const CGFloat HSAvatarColumn;
extern const CGFloat HSBubblePadX;     // from the bubble edge to the text
extern const CGFloat HSBubblePadY;

@interface HSTimelineTextView : UITextView
- (instancetype)init;
// The message whose status icon or time is at the point, if it is a failed one: the transaction id to retry
- (nullable NSString*)failedTxnIdAtPoint:(CGPoint)point;
@end

NS_ASSUME_NONNULL_END
