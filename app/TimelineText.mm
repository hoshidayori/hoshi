#import "TimelineText.h"

#include <cmath>

NSAttributedStringKey const HSMessageMetaAttribute = @"HSMessageMeta";
NSAttributedStringKey const HSSenderMetaAttribute = @"HSSenderMeta";
NSAttributedStringKey const HSCodeBlockAttribute = @"HSCodeBlock";

const CGFloat HSBubbleSide = 12;
const CGFloat HSAvatarSize = 30;
const CGFloat HSAvatarColumn = 54;  // fits "12:48 PM" in the caption font
const CGFloat HSBubblePadX = 10;
const CGFloat HSBubblePadY = 7;

@implementation HSMessageMeta

// Equal metas let an unchanged message keep its place in the text, and with it the user's selection
- (BOOL)isEqual:(id)other {
    if (![other isKindOfClass:[HSMessageMeta class]]) {
        return NO;
    }
    HSMessageMeta* meta = other;
    return [meta.time isEqualToString:_time] && meta.status == _status && meta.waitingForKey == _waitingForKey &&
        [meta.txnId isEqualToString:_txnId] && meta.part == _part;
}

- (NSUInteger)hash {
    return _time.hash ^ static_cast<NSUInteger>(_status) ^ _txnId.hash ^ (static_cast<NSUInteger>(_part) << 4);
}

@end

@implementation HSCodeBlock

- (instancetype)initWithIdentifier:(NSString*)identifier {
    if ((self = [super init])) {
        _identifier = [identifier copy];
    }
    return self;
}

- (BOOL)isEqual:(id)other {
    return [other isKindOfClass:[HSCodeBlock class]] && [((HSCodeBlock*)other).identifier isEqualToString:_identifier];
}

- (NSUInteger)hash {
    return _identifier.hash;
}

@end

@implementation HSSenderMeta

- (BOOL)isEqual:(id)other {
    if (![other isKindOfClass:[HSSenderMeta class]]) {
        return NO;
    }
    HSSenderMeta* meta = other;
    return [meta.initial isEqualToString:_initial] && [meta.color isEqual:_color] && meta.image == _image &&
        meta.loading == _loading;
}

- (NSUInteger)hash {
    return _initial.hash;
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

- (nullable HSCodeBlock*)codeBlock {
    NSTextParagraph* paragraph = [self.textElement isKindOfClass:[NSTextParagraph class]] ? (NSTextParagraph*)self.textElement : nil;
    if (paragraph.attributedString.length == 0) {
        return nil;
    }
    return [paragraph.attributedString attribute:HSCodeBlockAttribute atIndex:0 effectiveRange:nullptr];
}

// The lines' text, without the empty line break at the end
- (CGRect)textRect {
    CGRect text = CGRectNull;
    for (NSTextLineFragment* line in self.textLineFragments) {
        CGRect bounds = line.typographicBounds;
        if (bounds.size.width > 0) {
            text = CGRectUnion(text, bounds);
        }
    }
    return text;
}

// A message of one paragraph hugs its text: as wide as its longest line. A message of several paragraphs takes
// the whole width, so its parts line up; each part reaches the edge of its fragment where the next one goes on.
- (CGRect)bubbleRect {
    HSMessageMeta* meta = [self meta];
    const CGRect text = [self textRect];
    if (!meta || CGRectIsNull(text)) {
        return CGRectNull;
    }
    if (meta.part == HSBubblePartOnly) {
        return CGRectInset(text, -HSBubblePadX, -HSBubblePadY);
    }
    const CGFloat left = CGRectGetMinX(text) - HSBubblePadX;
    const CGFloat right = self.textLayoutManager.textContainer.size.width - HSBubbleSide - self.layoutFragmentFrame.origin.x;
    const CGFloat top = meta.part == HSBubblePartFirst ? CGRectGetMinY(text) - HSBubblePadY : 0;
    const CGFloat bottom = meta.part == HSBubblePartLast ? CGRectGetMaxY(text) + HSBubblePadY : self.layoutFragmentFrame.size.height;
    return CGRectMake(left, top, right - left, bottom - top);
}

// The width of the longest line as set, unwrapped. The line fragments of a clipped paragraph report their
// bounds cut at the container's edge, so the width comes from the line's text itself.
- (CGFloat)widestLine {
    CGFloat widest = 0;
    for (NSTextLineFragment* line in self.textLineFragments) {
        const NSRange range = line.characterRange;
        if (range.length == 0 || NSMaxRange(range) > line.attributedString.length) {
            continue;
        }
        widest = MAX(widest, [[line.attributedString attributedSubstringFromRange:range] size].width);
    }
    return widest;
}

// The box of a code block, in the bubble's text width
- (CGRect)codeBoxInBubble:(CGRect)bubble {
    const CGRect text = [self textRect];
    return CGRectMake(CGRectGetMinX(bubble) + HSBubblePadX - 6, CGRectGetMinY(text) - 4,
        CGRectGetWidth(bubble) - 2 * HSBubblePadX + 12, CGRectGetHeight(text) + 8);
}

- (nullable HSSenderMeta*)senderMeta {
    NSTextParagraph* paragraph = [self.textElement isKindOfClass:[NSTextParagraph class]] ? (NSTextParagraph*)self.textElement : nil;
    if (paragraph.attributedString.length == 0) {
        return nil;
    }
    return [paragraph.attributedString attribute:HSSenderMetaAttribute atIndex:0 effectiveRange:nullptr];
}

// Left of the name, centered on its line
- (CGRect)avatarRect {
    NSTextLineFragment* line = self.textLineFragments.firstObject;
    const CGFloat middle = line ? CGRectGetMidY(line.typographicBounds) : HSAvatarSize / 2;
    return CGRectMake(HSBubbleSide - self.layoutFragmentFrame.origin.x, middle - HSAvatarSize / 2, HSAvatarSize, HSAvatarSize);
}

- (CGRect)renderingSurfaceBounds {
    CGRect bounds = [super renderingSurfaceBounds];
    if ([self senderMeta]) {
        return CGRectUnion(bounds, [self avatarRect]);
    }
    if (![self meta]) {
        return bounds;
    }
    CGRect bubble = [self bubbleRect];
    if (CGRectIsNull(bubble)) {
        return bounds;
    }
    // The time column left of the bubble
    bubble.origin.x -= HSAvatarColumn;
    bubble.size.width += HSAvatarColumn;
    return CGRectUnion(bounds, bubble);
}

- (void)drawAvatar:(HSSenderMeta*)sender atPoint:(CGPoint)point inContext:(CGContextRef)context {
    const CGRect circle = CGRectOffset([self avatarRect], point.x, point.y);
    UIGraphicsPushContext(context);
    if (sender.image) {
        CGContextSaveGState(context);
        [[UIBezierPath bezierPathWithOvalInRect:circle] addClip];
        [sender.image drawInRect:circle];
        CGContextRestoreGState(context);
        UIGraphicsPopContext();
        return;
    }
    UITraitCollection* traits = UITraitCollection.currentTraitCollection;
    if (sender.loading) {
        // A quiet placeholder rather than the letter, which would flash before the picture replaces it. Still,
        // not spinning: an animation here would redraw the text every frame.
        [[UIColor.tertiarySystemFillColor resolvedColorWithTraitCollection:traits] setFill];
        [[UIBezierPath bezierPathWithOvalInRect:circle] fill];
        UIBezierPath* ring = [UIBezierPath bezierPathWithOvalInRect:CGRectInset(circle, 1, 1)];
        const CGFloat dash[] = {2, 3};
        [ring setLineDash:dash count:2 phase:0];
        ring.lineWidth = 1;
        [[UIColor.tertiaryLabelColor resolvedColorWithTraitCollection:traits] setStroke];
        [ring stroke];
        UIGraphicsPopContext();
        return;
    }
    [[sender.color resolvedColorWithTraitCollection:traits] setFill];
    [[UIBezierPath bezierPathWithOvalInRect:circle] fill];
    UIFont* font = [UIFont systemFontOfSize:HSAvatarSize * 0.45 weight:UIFontWeightSemibold];
    NSDictionary* attributes = @{NSFontAttributeName: font, NSForegroundColorAttributeName: UIColor.whiteColor};
    const CGSize size = [sender.initial sizeWithAttributes:attributes];
    [sender.initial drawAtPoint:CGPointMake(CGRectGetMidX(circle) - size.width / 2, CGRectGetMidY(circle) - size.height / 2)
                 withAttributes:attributes];
    UIGraphicsPopContext();
}

- (void)drawAtPoint:(CGPoint)point inContext:(CGContextRef)context {
    if (HSSenderMeta* sender = [self senderMeta]) {
        [self drawAvatar:sender atPoint:point inContext:context];
    }
    HSMessageMeta* meta = [self meta];
    CGRect bubble = meta ? [self bubbleRect] : CGRectNull;
    if (!CGRectIsNull(bubble)) {
        bubble = CGRectOffset(bubble, point.x, point.y);
        UIGraphicsPushContext(context);
        UITraitCollection* traits = UITraitCollection.currentTraitCollection;
        // Translucent: the selection highlight lies under the text layer, and must show through the bubble
        [[[UIColor.labelColor resolvedColorWithTraitCollection:traits] colorWithAlphaComponent:0.06] setFill];
        const UIRectCorner corners = meta.part == HSBubblePartOnly ? UIRectCornerAllCorners
            : meta.part == HSBubblePartFirst ? (UIRectCornerTopLeft | UIRectCornerTopRight)
            : meta.part == HSBubblePartLast ? (UIRectCornerBottomLeft | UIRectCornerBottomRight)
            : 0;
        [[UIBezierPath bezierPathWithRoundedRect:bubble byRoundingCorners:corners cornerRadii:CGSizeMake(12, 12)] fill];

        HSCodeBlock* block = [self codeBlock];
        CGRect box = CGRectNull;
        if (block) {
            box = [self codeBoxInBubble:bubble];
            [[[UIColor.labelColor resolvedColorWithTraitCollection:traits] colorWithAlphaComponent:0.07] setFill];
            [[UIBezierPath bezierPathWithRoundedRect:box cornerRadius:6] fill];
            // The lines do not wrap: what is wider than the box is reached by scrolling it sideways
            block.maxOffset = MAX(0, [self widestLine] - (CGRectGetWidth(box) - 12));
            block.offset = MIN(MAX(block.offset, 0), block.maxOffset);
        }
        if (meta.part == HSBubblePartOnly || meta.part == HSBubblePartLast) {
            [self drawTimeOf:meta rightOf:bubble traits:traits];
        }
        UIGraphicsPopContext();
        if (block) {
            CGContextSaveGState(context);
            CGContextClipToRect(context, CGRectInset(box, 2, 0));
            [super drawAtPoint:CGPointMake(point.x - block.offset, point.y) inContext:context];
            CGContextRestoreGState(context);
            return;
        }
    }
    [super drawAtPoint:point inContext:context];
}

// In the column left of the bubble, against it, on its last line: the time, or instead the state of a message
// sent from this device or still waiting for its key
- (void)drawTimeOf:(HSMessageMeta*)meta rightOf:(CGRect)bubble traits:(UITraitCollection*)traits {
    {
        UIFont* font = TimeFont();
        const CGFloat right = CGRectGetMinX(bubble) - 5;
        const CGFloat baselineY = CGRectGetMaxY(bubble) - HSBubblePadY;
        NSString* icon = meta.status == HSMessageStatusSending ? @"clock"
            : meta.status == HSMessageStatusFailed ? @"exclamationmark.circle.fill"
            : meta.waitingForKey ? @"lock" : nil;
        if (icon) {
            UIColor* color = meta.status == HSMessageStatusFailed ? UIColor.systemRedColor : UIColor.secondaryLabelColor;
            UIImage* image = [[UIImage systemImageNamed:icon withConfiguration:[UIImageSymbolConfiguration configurationWithFont:font]]
                imageWithTintColor:[color resolvedColorWithTraitCollection:traits] renderingMode:UIImageRenderingModeAlwaysOriginal];
            [image drawAtPoint:CGPointMake(right - image.size.width, baselineY - image.size.height + 2)];
        } else {
            NSDictionary* attributes = @{
                NSFontAttributeName: font,
                NSForegroundColorAttributeName: [UIColor.secondaryLabelColor resolvedColorWithTraitCollection:traits],
            };
            const CGFloat width = [meta.time sizeWithAttributes:attributes].width;
            [meta.time drawAtPoint:CGPointMake(right - width, baselineY - font.ascender) withAttributes:attributes];
        }
    }
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

@interface HSTimelineTextView () <NSTextLayoutManagerDelegate, UIGestureRecognizerDelegate>
@end

@implementation HSTimelineTextView {
    UIPanGestureRecognizer* _codePan;
    HSCodeBlock* _scrolledBlock;    // the block a sideways drag moves
    CGFloat _scrolledBlockStart;    // its offset when the drag began
    NSTextRange* _scrolledRange;    // its paragraph, to lay out again
}

- (instancetype)init {
    // TextKit 2: the custom layout fragments draw the bubbles, and layout stays near the visible part
    if ((self = [super initWithFrame:CGRectZero textContainer:nil])) {
        self.editable = NO;
        self.selectable = YES;
        self.dataDetectorTypes = UIDataDetectorTypeLink;
        self.textContainerInset = UIEdgeInsetsMake(12, 0, 12, 0);
        self.textContainer.lineFragmentPadding = 0;
        self.textLayoutManager.delegate = self;
        // A sideways drag on a code block that does not fit scrolls the block; the timeline scrolls only once
        // that gesture gave up
        _codePan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(scrollCodeBlock:)];
        _codePan.delegate = self;
        [self addGestureRecognizer:_codePan];
        [self.panGestureRecognizer requireGestureRecognizerToFail:_codePan];
    }
    return self;
}

// The fragment of the code block at a point of the view, if the block is wider than its box
- (nullable HSMessageLayoutFragment*)scrollableCodeFragmentAtPoint:(CGPoint)point {
    const CGPoint inContainer = CGPointMake(point.x - self.textContainerInset.left, point.y - self.textContainerInset.top);
    NSTextLayoutFragment* fragment = [self.textLayoutManager textLayoutFragmentForPosition:inContainer];
    if (![fragment isKindOfClass:[HSMessageLayoutFragment class]]) {
        return nil;
    }
    HSMessageLayoutFragment* message = (HSMessageLayoutFragment*)fragment;
    HSCodeBlock* block = [message codeBlock];
    return block && block.maxOffset > 0 ? message : nil;
}

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer*)recognizer {
    if (recognizer != _codePan) {
        return [super gestureRecognizerShouldBegin:recognizer];
    }
    const CGPoint velocity = [_codePan velocityInView:self];
    if (std::abs(velocity.x) <= std::abs(velocity.y)) {
        return NO;
    }
    HSMessageLayoutFragment* fragment = [self scrollableCodeFragmentAtPoint:[_codePan locationInView:self]];
    if (!fragment) {
        return NO;
    }
    _scrolledBlock = [fragment codeBlock];
    _scrolledBlockStart = _scrolledBlock.offset;
    _scrolledRange = fragment.rangeInElement;
    return YES;
}

- (void)scrollCodeBlock:(UIPanGestureRecognizer*)pan {
    if (!_scrolledBlock || (pan.state != UIGestureRecognizerStateChanged && pan.state != UIGestureRecognizerStateEnded)) {
        return;
    }
    const CGFloat offset = MIN(MAX(_scrolledBlockStart - [pan translationInView:self].x, 0), _scrolledBlock.maxOffset);
    if (offset != _scrolledBlock.offset) {
        _scrolledBlock.offset = offset;
        // The fragment draws with the new offset once it is laid out again; only this paragraph is
        [self.textLayoutManager invalidateLayoutForRange:_scrolledRange];
        [self.textLayoutManager.textViewportLayoutController layoutViewport];
    }
    if (pan.state == UIGestureRecognizerStateEnded) {
        _scrolledBlock = nil;
        _scrolledRange = nil;
    }
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
        usingBlock:^BOOL(NSTextRange* segment, CGRect frame, CGFloat, NSTextContainer*) {
            if (frame.size.width < 1) {
                return YES;  // the line break at a paragraph's end: nothing to show
            }
            // In a scrolled code block the text is drawn shifted, and so is its selection, cut at the box
            CGRect line = CGRectOffset(frame, inset.left, inset.top);
            if (segment) {
                NSTextLayoutFragment* fragment = [self.textLayoutManager textLayoutFragmentForLocation:segment.location];
                if ([fragment isKindOfClass:[HSMessageLayoutFragment class]]) {
                    HSMessageLayoutFragment* message = (HSMessageLayoutFragment*)fragment;
                    if (HSCodeBlock* block = [message codeBlock]) {
                        line = CGRectOffset(line, -block.offset, 0);
                        CGRect box = [message codeBoxInBubble:[message bubbleRect]];
                        box = CGRectOffset(box, fragment.layoutFragmentFrame.origin.x + inset.left,
                            fragment.layoutFragmentFrame.origin.y + inset.top);
                        line.origin.x = MAX(line.origin.x, CGRectGetMinX(box));
                        line.size.width = MIN(CGRectGetMaxX(line), CGRectGetMaxX(box)) - line.origin.x;
                        if (line.size.width < 1) {
                            return YES;  // scrolled out of the box
                        }
                    }
                }
            }
            HSSelectionRect* rect = [[HSSelectionRect alloc] init];
            rect.lineRect = line;
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

// How far the code block at a point of the view is scrolled; 0 elsewhere
- (CGFloat)codeOffsetAtPoint:(CGPoint)point {
    const CGPoint inContainer = CGPointMake(point.x - self.textContainerInset.left, point.y - self.textContainerInset.top);
    NSTextLayoutFragment* fragment = [self.textLayoutManager textLayoutFragmentForPosition:inContainer];
    if (![fragment isKindOfClass:[HSMessageLayoutFragment class]]) {
        return 0;
    }
    HSCodeBlock* block = [(HSMessageLayoutFragment*)fragment codeBlock];
    return block ? block.offset : 0;
}

// A touch in a scrolled code block means the character drawn under it, which the layout has further right
- (UITextPosition*)closestPositionToPoint:(CGPoint)point {
    return [super closestPositionToPoint:CGPointMake(point.x + [self codeOffsetAtPoint:point], point.y)];
}

- (UITextPosition*)closestPositionToPoint:(CGPoint)point withinRange:(UITextRange*)range {
    return [super closestPositionToPoint:CGPointMake(point.x + [self codeOffsetAtPoint:point], point.y) withinRange:range];
}

- (CGRect)caretRectForPosition:(UITextPosition*)position {
    CGRect caret = [super caretRectForPosition:position];
    const CGPoint middle = CGPointMake(CGRectGetMidX(caret), CGRectGetMidY(caret));
    return CGRectOffset(caret, -[self codeOffsetAtPoint:middle], 0);
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
    // The tap counts on the icon, left of the bubble, so a tap in the text still selects
    const CGFloat x = inContainer.x - fragment.layoutFragmentFrame.origin.x;
    return x < CGRectGetMinX(bubble) ? meta.txnId : nil;
}

@end
