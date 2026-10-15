#import "TimelineText.h"

NSAttributedStringKey const HSMessageMetaAttribute = @"HSMessageMeta";

const CGFloat HSBubbleSide = 12;
const CGFloat HSBubblePadX = 10;
const CGFloat HSBubblePadY = 7;
const CGFloat HSTimeColumn = 58;

@implementation HSMessageMeta

// Equal metas let an unchanged message keep its place in the text, and with it the user's selection
- (BOOL)isEqual:(id)other {
    if (![other isKindOfClass:[HSMessageMeta class]]) {
        return NO;
    }
    HSMessageMeta* meta = other;
    return [meta.time isEqualToString:_time] && meta.status == _status && meta.waitingForKey == _waitingForKey &&
        [meta.txnId isEqualToString:_txnId];
}

- (NSUInteger)hash {
    return _time.hash ^ static_cast<NSUInteger>(_status) ^ _txnId.hash;
}

@end

namespace {

UIFont* TimeFont() {
    return [UIFont preferredFontForTextStyle:UIFontTextStyleCaption2];
}

} // namespace

// Draws the bubble behind a message body, and the time and status icon to the right of it
@interface HSMessageLayoutFragment : NSTextLayoutFragment
@end

@implementation HSMessageLayoutFragment

- (nullable HSMessageMeta*)meta {
    NSTextParagraph* paragraph = [self.textElement isKindOfClass:[NSTextParagraph class]] ? (NSTextParagraph*)self.textElement : nil;
    if (paragraph.attributedString.length == 0) {
        return nil;
    }
    return [paragraph.attributedString attribute:HSMessageMetaAttribute atIndex:0 effectiveRange:nullptr];
}

// Hugs the text: as wide as its longest line
- (CGRect)bubbleRect {
    CGRect text = CGRectNull;
    for (NSTextLineFragment* line in self.textLineFragments) {
        CGRect bounds = line.typographicBounds;
        if (bounds.size.width > 0) {
            text = CGRectUnion(text, bounds);
        }
    }
    if (CGRectIsNull(text)) {
        return CGRectNull;
    }
    return CGRectInset(text, -HSBubblePadX, -HSBubblePadY);
}

- (CGRect)renderingSurfaceBounds {
    CGRect bounds = [super renderingSurfaceBounds];
    if (![self meta]) {
        return bounds;
    }
    CGRect bubble = [self bubbleRect];
    if (CGRectIsNull(bubble)) {
        return bounds;
    }
    bubble.size.width += HSTimeColumn;
    return CGRectUnion(bounds, bubble);
}

- (void)drawAtPoint:(CGPoint)point inContext:(CGContextRef)context {
    HSMessageMeta* meta = [self meta];
    CGRect bubble = meta ? [self bubbleRect] : CGRectNull;
    if (!CGRectIsNull(bubble)) {
        bubble = CGRectOffset(bubble, point.x, point.y);
        UIGraphicsPushContext(context);
        UITraitCollection* traits = UITraitCollection.currentTraitCollection;
        // Translucent: the selection highlight lies under the text layer, and must show through the bubble
        [[[UIColor.labelColor resolvedColorWithTraitCollection:traits] colorWithAlphaComponent:0.06] setFill];
        [[UIBezierPath bezierPathWithRoundedRect:bubble cornerRadius:12] fill];

        // Time, and before it the state of a message sent from this device, at the bubble's bottom right
        UIFont* font = TimeFont();
        const CGFloat x = CGRectGetMaxX(bubble) + 6;
        const CGFloat baselineY = CGRectGetMaxY(bubble) - HSBubblePadY;
        CGFloat timeX = x;
        NSString* icon = meta.status == HSMessageStatusSending ? @"clock"
            : meta.status == HSMessageStatusFailed ? @"exclamationmark.circle.fill"
            : meta.waitingForKey ? @"lock" : nil;
        if (icon) {
            UIColor* color = meta.status == HSMessageStatusFailed ? UIColor.systemRedColor : UIColor.secondaryLabelColor;
            UIImage* image = [[UIImage systemImageNamed:icon withConfiguration:[UIImageSymbolConfiguration configurationWithFont:font]]
                imageWithTintColor:[color resolvedColorWithTraitCollection:traits] renderingMode:UIImageRenderingModeAlwaysOriginal];
            [image drawAtPoint:CGPointMake(x, baselineY - image.size.height + 2)];
            timeX += image.size.width + 3;
        }
        [meta.time drawAtPoint:CGPointMake(timeX, baselineY - font.ascender) withAttributes:@{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: [UIColor.secondaryLabelColor resolvedColorWithTraitCollection:traits],
        }];
        UIGraphicsPopContext();
    }
    [super drawAtPoint:point inContext:context];
}

@end

// One line of a selection, as tight as the text on it
@interface HSSelectionRect : UITextSelectionRect
@property (nonatomic) CGRect lineRect;
@property (nonatomic) BOOL start;
@property (nonatomic) BOOL end;
@end

@implementation HSSelectionRect
- (CGRect)rect { return _lineRect; }
- (NSWritingDirection)writingDirection { return NSWritingDirectionNatural; }
- (BOOL)containsStart { return _start; }
- (BOOL)containsEnd { return _end; }
- (BOOL)isVertical { return NO; }
@end

@interface HSTimelineTextView () <NSTextLayoutManagerDelegate>
@end

@implementation HSTimelineTextView

- (instancetype)init {
    // TextKit 2: the custom layout fragments draw the bubbles, and layout stays near the visible part
    if ((self = [super initWithFrame:CGRectZero textContainer:nil])) {
        self.editable = NO;
        self.selectable = YES;
        self.dataDetectorTypes = UIDataDetectorTypeLink;
        self.textContainerInset = UIEdgeInsetsMake(12, 0, 12, 0);
        self.textContainer.lineFragmentPadding = 0;
        self.textLayoutManager.delegate = self;
    }
    return self;
}

- (NSTextLayoutFragment*)textLayoutManager:(NSTextLayoutManager*)textLayoutManager
             textLayoutFragmentForLocation:(id<NSTextLocation>)location
                             inTextElement:(NSTextElement*)textElement {
    return [[HSMessageLayoutFragment alloc] initWithTextElement:textElement range:textElement.elementRange];
}

// The copy has plain newlines where the messages had line breaks; names and bodies, no times or icons
- (void)copy:(id)sender {
    const NSRange range = self.selectedRange;
    if (range.length == 0) {
        return;
    }
    NSString* text = [[self.textStorage.string substringWithRange:range] stringByReplacingOccurrencesOfString:@" "
                                                                                                  withString:@"\n"];
    UIPasteboard.generalPasteboard.string = text;
}

// Over several lines UIKit paints from the selection's start to the right edge, whole lines in between and
// the gaps between paragraphs, which across messages becomes one wide block over bubbles and margins. Here
// each line gets a rectangle around its own selected text only.
- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)range {
    NSArray<UITextSelectionRect*>* system = [super selectionRectsForRange:range];
    NSTextContentManager* content = self.textLayoutManager.textContentManager;
    const NSInteger start = [self offsetFromPosition:self.beginningOfDocument toPosition:range.start];
    const NSInteger end = [self offsetFromPosition:self.beginningOfDocument toPosition:range.end];
    id<NSTextLocation> from = [content locationFromLocation:content.documentRange.location withOffset:start];
    id<NSTextLocation> to = [content locationFromLocation:content.documentRange.location withOffset:end];
    NSTextRange* textRange = from && to ? [[NSTextRange alloc] initWithLocation:from endLocation:to] : nil;
    if (!textRange || textRange.isEmpty) {
        return system;
    }
    NSMutableArray<HSSelectionRect*>* rects = [NSMutableArray array];
    const UIEdgeInsets inset = self.textContainerInset;
    [self.textLayoutManager enumerateTextSegmentsInRange:textRange type:NSTextLayoutManagerSegmentTypeStandard
        options:NSTextLayoutManagerSegmentOptionsNone
        usingBlock:^BOOL(NSTextRange*, CGRect frame, CGFloat, NSTextContainer*) {
            if (frame.size.width < 1) {
                return YES;  // the line break at a paragraph's end: nothing to show
            }
            HSSelectionRect* rect = [[HSSelectionRect alloc] init];
            rect.lineRect = CGRectOffset(frame, inset.left, inset.top);
            [rects addObject:rect];
            return YES;
        }];
    if (rects.count == 0) {
        return system;
    }
    rects.firstObject.start = YES;
    rects.lastObject.end = YES;
    return rects;
}

- (nullable NSString*)failedTxnIdAtPoint:(CGPoint)point {
    const CGPoint inContainer = CGPointMake(point.x - self.textContainerInset.left, point.y - self.textContainerInset.top);
    NSTextLayoutFragment* fragment = [self.textLayoutManager textLayoutFragmentForPosition:inContainer];
    if (![fragment isKindOfClass:[HSMessageLayoutFragment class]]) {
        return nil;
    }
    HSMessageLayoutFragment* message = (HSMessageLayoutFragment*)fragment;
    HSMessageMeta* meta = [message meta];
    CGRect bubble = [message bubbleRect];
    if (!meta || meta.status != HSMessageStatusFailed || CGRectIsNull(bubble)) {
        return nil;
    }
    // The tap counts on the icon and time, right of the bubble, so a tap in the text still selects
    const CGFloat x = inContainer.x - fragment.layoutFragmentFrame.origin.x;
    return x > CGRectGetMaxX(bubble) ? meta.txnId : nil;
}

@end
