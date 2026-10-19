#import "DevicesViewController.h"
#import "Session.h"
#import "VerificationViewController.h"

#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

// Ed25519 key in groups of four, the way Element shows a session key, so the two can be compared by eye
NSString* Fingerprint(const std::string& key) {
    NSMutableArray<NSString*>* groups = [NSMutableArray array];
    for (size_t i = 0; i < key.size(); i += 4) {
        [groups addObject:ToNs(key.substr(i, 4))];
    }
    return [groups componentsJoinedByString:@" "];
}

} // namespace

@implementation DevicesViewController {
    HSSession* _session;
    std::vector<HSDevice> _devices;
    NSString* _error;
}

- (instancetype)initWithSession:(HSSession*)session {
    if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
        _session = session;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Devices";
    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:@"device"];
    self.refreshControl = [[UIRefreshControl alloc] init];
    [self.refreshControl addTarget:self action:@selector(reload) forControlEvents:UIControlEventValueChanged];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(verificationChanged)
                                               name:HSSessionVerificationDidChange object:_session];
    [self reload];
}

// A verification that succeeds changes what is verified
- (void)verificationChanged {
    const auto& verification = _session.verification;
    if (verification && verification->State() == NHoshi::NVerification::EState::Done) {
        [self reload];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)reload {
    __weak DevicesViewController* weakSelf = self;
    [_session fetchDevicesOfUser:_session.userId completion:^(std::vector<HSDevice> devices, NSString* error) {
        DevicesViewController* strongSelf = weakSelf;
        if (!strongSelf) {
            return;
        }
        strongSelf->_devices = std::move(devices);
        strongSelf->_error = error;
        [strongSelf.refreshControl endRefreshing];
        [strongSelf.tableView reloadData];
    }];
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
    if (_error) {
        return _error;
    }
    return _devices.empty() ? @"No other devices are signed in to this account."
                            : @"Verify a device by comparing emoji with it. A device's key is shown to compare with the key the device itself shows.";
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return static_cast<NSInteger>(_devices.size());
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:@"device" forIndexPath:indexPath];
    const HSDevice& device = _devices[static_cast<size_t>(indexPath.row)];
    UIListContentConfiguration* content = UIListContentConfiguration.subtitleCellConfiguration;
    content.text = ToNs(device.Device.DeviceId);
    content.secondaryText = device.CrossSigned
        ? [Fingerprint(device.Device.Ed25519) stringByAppendingString:@"\nVerified through cross-signing"]
        : Fingerprint(device.Device.Ed25519);
    content.secondaryTextProperties.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    content.image = [UIImage systemImageNamed:device.Verified ? @"checkmark.shield.fill" : @"exclamationmark.shield"];
    content.imageProperties.tintColor = device.Verified ? UIColor.systemGreenColor : UIColor.systemOrangeColor;
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    [_session requestVerificationOfDevice:_devices[static_cast<size_t>(indexPath.row)].Device];
    [self presentViewController:[[VerificationViewController alloc] initWithSession:_session] animated:YES completion:nil];
}

@end
