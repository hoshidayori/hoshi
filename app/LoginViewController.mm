#import "LoginViewController.h"
#import "AppRouter.h"
#import "Session.h"

@implementation LoginViewController {
    UITextField* _homeserver;
    UITextField* _user;
    UITextField* _password;
    UIButton* _login;
    UILabel* _error;
    HSSession* _session;
}

- (UITextField*)fieldWithPlaceholder:(NSString*)placeholder {
    UITextField* field = [[UITextField alloc] init];
    field.placeholder = placeholder;
    field.borderStyle = UITextBorderStyleRoundedRect;
    field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    return field;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Hoshi";
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    _homeserver = [self fieldWithPlaceholder:@"Homeserver, e.g. https://matrix.org"];
    _homeserver.keyboardType = UIKeyboardTypeURL;
#if TARGET_OS_SIMULATOR
    // The local test Synapse in the repository listens here when started for the simulator
    _homeserver.text = @"http://127.0.0.1:28008";
#endif
    _user = [self fieldWithPlaceholder:@"User name"];
    _user.textContentType = UITextContentTypeUsername;
    _password = [self fieldWithPlaceholder:@"Password"];
    _password.secureTextEntry = YES;
    _password.textContentType = UITextContentTypePassword;

    _login = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration* config = UIButtonConfiguration.filledButtonConfiguration;
    config.title = @"Sign in";
    _login.configuration = config;
    [_login addTarget:self action:@selector(signIn) forControlEvents:UIControlEventTouchUpInside];

    _error = [[UILabel alloc] init];
    _error.textColor = UIColor.systemRedColor;
    _error.numberOfLines = 0;
    _error.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _error.text = self.message;

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[_homeserver, _user, _password, _login, _error]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:32],
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
    ]];
}

- (void)signIn {
    NSString* homeserver = [_homeserver.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (homeserver.length == 0 || _user.text.length == 0 || _password.text.length == 0) {
        _error.text = @"Fill in all three fields.";
        return;
    }
    _error.text = nil;
    _login.enabled = NO;
    _session = [[HSSession alloc] initWithHomeserver:homeserver];
    [_session loginUser:_user.text password:_password.text completion:^(NSString* error) {
        self->_login.enabled = YES;
        if (error) {
            self->_error.text = error;
            return;
        }
        [HSAppRouter.shared didSignIn:self->_session];
    }];
}

@end
