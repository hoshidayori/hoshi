#import "VerificationViewController.h"
#import "Session.h"

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

UIButton* MakeButton(NSString* title, BOOL prominent, BOOL destructive) {
    UIButtonConfiguration* configuration = prominent ? UIButtonConfiguration.filledButtonConfiguration
                                                     : UIButtonConfiguration.grayButtonConfiguration;
    configuration.title = title;
    configuration.buttonSize = UIButtonConfigurationSizeLarge;
    if (destructive) {
        configuration.baseForegroundColor = prominent ? UIColor.whiteColor : UIColor.systemRedColor;
        if (prominent) {
            configuration.baseBackgroundColor = UIColor.systemRedColor;
        }
    }
    return [UIButton buttonWithConfiguration:configuration primaryAction:nil];
}

} // namespace

@implementation VerificationViewController {
    HSSession* _session;
    UILabel* _title;
    UILabel* _message;
    UIStackView* _emoji;
    UIStackView* _buttons;
}

- (instancetype)initWithSession:(HSSession*)session {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _session = session;
        self.modalInPresentation = YES;  // closing goes through the buttons, so the other side hears about it
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    _title = [[UILabel alloc] init];
    _title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    _title.adjustsFontForContentSizeCategory = YES;
    _title.textAlignment = NSTextAlignmentCenter;
    _title.numberOfLines = 0;

    _message = [[UILabel alloc] init];
    _message.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _message.adjustsFontForContentSizeCategory = YES;
    _message.textColor = UIColor.secondaryLabelColor;
    _message.textAlignment = NSTextAlignmentCenter;
    _message.numberOfLines = 0;

    _emoji = [[UIStackView alloc] init];
    _emoji.axis = UILayoutConstraintAxisVertical;
    _emoji.spacing = 16;

    _buttons = [[UIStackView alloc] init];
    _buttons.axis = UILayoutConstraintAxisVertical;
    _buttons.spacing = 12;

    UIStackView* column = [[UIStackView alloc] initWithArrangedSubviews:@[_title, _message, _emoji, _buttons]];
    column.axis = UILayoutConstraintAxisVertical;
    column.spacing = 24;
    column.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:column];
    UILayoutGuide* margins = self.view.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
        [column.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
        [column.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
        [column.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(update)
                                               name:HSSessionVerificationDidChange object:_session];
    [self update];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)addButton:(NSString*)title prominent:(BOOL)prominent destructive:(BOOL)destructive action:(void (^)(void))action {
    UIButton* button = MakeButton(title, prominent, destructive);
    [button addAction:[UIAction actionWithHandler:^(UIAction*) { action(); }] forControlEvents:UIControlEventTouchUpInside];
    [_buttons addArrangedSubview:button];
}

// Two rows, 4 and 3, as Element shows them, each emoji with its name under it
- (void)showEmoji:(const std::array<NHoshi::NSas::TEmoji, 7>&)emoji {
    for (size_t row = 0; row < 2; ++row) {
        UIStackView* line = [[UIStackView alloc] init];
        line.axis = UILayoutConstraintAxisHorizontal;
        line.distribution = UIStackViewDistributionFillEqually;
        line.spacing = 8;
        for (size_t i = row * 4; i < std::min<size_t>(row * 4 + 4, emoji.size()); ++i) {
            UILabel* symbol = [[UILabel alloc] init];
            symbol.text = @(emoji[i].Symbol);
            symbol.font = [UIFont systemFontOfSize:44];
            symbol.textAlignment = NSTextAlignmentCenter;
            UILabel* name = [[UILabel alloc] init];
            name.text = @(emoji[i].Description);
            name.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
            name.adjustsFontForContentSizeCategory = YES;
            name.textAlignment = NSTextAlignmentCenter;
            name.numberOfLines = 2;
            UIStackView* cell = [[UIStackView alloc] initWithArrangedSubviews:@[symbol, name]];
            cell.axis = UILayoutConstraintAxisVertical;
            cell.spacing = 4;
            cell.isAccessibilityElement = YES;
            cell.accessibilityLabel = name.text;
            [line addArrangedSubview:cell];
        }
        [_emoji addArrangedSubview:line];
    }
}

- (void)update {
    using NHoshi::NVerification::EState;
    for (UIView* view in [_emoji.arrangedSubviews arrayByAddingObjectsFromArray:_buttons.arrangedSubviews]) {
        [view removeFromSuperview];
    }
    const auto& verification = _session.verification;
    if (!verification) {
        [self dismissViewControllerAnimated:YES completion:nil];
        return;
    }
    HSSession* session = _session;
    __weak VerificationViewController* weakSelf = self;
    void (^close)(void) = ^{
        [weakSelf dismissViewControllerAnimated:YES completion:nil];
    };
    void (^cancel)(void) = ^{
        [session cancelVerificationBecauseOfMismatch:NO];
        close();
    };
    const auto& them = verification->Them();
    NSString* device = [NSString stringWithFormat:@"%@ (%@)", ToNs(them.DeviceId), ToNs(them.UserId)];
    _title.text = @"Verify device";
    switch (verification->State()) {
        case EState::Requested: {
            _message.text = [NSString stringWithFormat:@"%@ asks to verify this device. You will compare emoji on both screens.", device];
            [self addButton:@"Accept" prominent:YES destructive:NO action:^{ [session acceptVerification]; }];
            [self addButton:@"Decline" prominent:NO destructive:YES action:cancel];
            break;
        }
        case EState::WaitingForReady:
            _message.text = [NSString stringWithFormat:@"Accept the request on %@.", device];
            [self addButton:@"Cancel" prominent:NO destructive:NO action:cancel];
            break;
        case EState::Ready: {
            _message.text = [NSString stringWithFormat:@"Ready. Start here, or choose emoji verification on %@.", device];
            [self addButton:@"Compare emoji" prominent:YES destructive:NO action:^{ [session startEmojiVerification]; }];
            [self addButton:@"Cancel" prominent:NO destructive:NO action:cancel];
            break;
        }
        case EState::WaitingForAccept:
        case EState::WaitingForKey:
            _message.text = @"Exchanging keys…";
            [self addButton:@"Cancel" prominent:NO destructive:NO action:cancel];
            break;
        case EState::ShowingSas: {
            _message.text = [NSString stringWithFormat:@"Check that %@ shows the same emoji in the same order.", device];
            [self showEmoji:verification->Emoji()];
            [self addButton:@"They match" prominent:YES destructive:NO action:^{ [session confirmEmoji]; }];
            [self addButton:@"They don't match" prominent:NO destructive:YES action:^{
                [session cancelVerificationBecauseOfMismatch:YES];
            }];
            break;
        }
        case EState::WaitingForMac:
        case EState::WaitingForDone:
            _message.text = [NSString stringWithFormat:@"Waiting for %@ to confirm…", device];
            [self showEmoji:verification->Emoji()];
            [self addButton:@"Cancel" prominent:NO destructive:NO action:cancel];
            break;
        case EState::Done:
            _title.text = @"Verified";
            _message.text = [NSString stringWithFormat:@"%@ is verified. Both screens showed the same emoji, so no one in between replaced its keys.", device];
            [self addButton:@"Done" prominent:YES destructive:NO action:close];
            break;
        case EState::Cancelled: {
            _title.text = @"Not verified";
            const std::string& code = verification->CancelCode();
            NSString* why = code == "m.mismatched_sas" ? @"The emoji did not match. Someone may be intercepting the connection."
                : code == "m.key_mismatch" ? @"The keys did not match. Someone may be intercepting the connection."
                : code == "m.user" ? @"The verification was cancelled."
                : [NSString stringWithFormat:@"The verification stopped: %@ (%@).", ToNs(verification->CancelReason()), ToNs(code)];
            _message.text = why;
            [self addButton:@"Close" prominent:YES destructive:NO action:close];
            break;
        }
    }
}

@end
