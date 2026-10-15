#import "RoomsViewController.h"
#import "AppRouter.h"
#import "DevicesViewController.h"
#import "Session.h"
#import "TimelineViewController.h"
#import "VerificationViewController.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

} // namespace

// Invites, when there are any, come in a section above the rooms
@implementation RoomsViewController {
    HSSession* _session;
    std::vector<std::string> _inviteIds;
    std::vector<std::string> _roomIds;
}

- (BOOL)isInviteSection:(NSInteger)section {
    return !_inviteIds.empty() && section == 0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
    return _inviteIds.empty() ? 1 : 2;
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
    if (_inviteIds.empty()) {
        return nil;
    }
    return [self isInviteSection:section] ? @"Invites" : @"Rooms";
}

- (instancetype)initWithSession:(HSSession*)session {
    if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
        _session = session;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Rooms";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Sign out" style:UIBarButtonItemStylePlain
                                                                             target:self action:@selector(signOut)];
    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:@"room"];
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Devices" style:UIBarButtonItemStylePlain
                                                                            target:self action:@selector(showDevices)];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(stateChanged)
                                               name:HSSessionStateDidChange object:_session];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(verificationChanged)
                                               name:HSSessionVerificationDidChange object:_session];
    [NSNotificationCenter.defaultCenter addObserver:self.tableView selector:@selector(reloadData)
                                               name:HSSessionAvatarsDidChange object:_session];
    [self stateChanged];
}

- (void)showDevices {
    [self.navigationController pushViewController:[[DevicesViewController alloc] initWithSession:_session] animated:YES];
}

// Another device asks to verify this one: show the request over whatever screen is open
- (void)verificationChanged {
    const auto& verification = _session.verification;
    UIViewController* top = self.navigationController ?: self;
    if (verification && verification->State() == NHoshi::NVerification::EState::Requested && !top.presentedViewController) {
        [top presentViewController:[[VerificationViewController alloc] initWithSession:_session] animated:YES completion:nil];
    }
}

- (void)signOut {
    UIAlertController* confirm = [UIAlertController alertControllerWithTitle:@"Sign out?"
        message:@"This device will forget the account and stop receiving messages." preferredStyle:UIAlertControllerStyleAlert];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Sign out" style:UIAlertActionStyleDestructive handler:^(UIAlertAction*) {
        [HSAppRouter.shared signOut];
    }]];
    [self presentViewController:confirm animated:YES completion:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)stateChanged {
    _inviteIds.clear();
    for (const auto& [id, invite] : _session.state.Invites) {
        _inviteIds.push_back(id);
    }
    _roomIds.clear();
    for (const auto& [id, room] : _session.state.Rooms) {
        _roomIds.push_back(id);
    }
    // The room with the newest message first, as in other messengers; rooms without messages go last
    const auto& rooms = _session.state.Rooms;
    auto lastActivity = [&rooms](const std::string& id) -> std::int64_t {
        const auto& timeline = rooms.at(id).Timeline;
        return timeline.empty() ? 0 : timeline.back().OriginServerTs;
    };
    std::stable_sort(_roomIds.begin(), _roomIds.end(), [&](const std::string& a, const std::string& b) {
        return lastActivity(a) > lastActivity(b);
    });
    [self.tableView reloadData];
    [self openRoomFromLaunchArguments];
}

// For screenshots from sim-run.sh, which cannot tap: "-HSOpenRoomAt N" opens the N-th room of the list once
- (void)openRoomFromLaunchArguments {
    static BOOL opened = NO;
    NSString* argument = [NSUserDefaults.standardUserDefaults stringForKey:@"HSOpenRoomAt"];
    if (opened || !argument || _roomIds.empty()) {
        return;
    }
    opened = YES;
    const size_t index = std::min<size_t>(static_cast<size_t>(MAX(argument.integerValue, 0)), _roomIds.size() - 1);
    [self.navigationController pushViewController:[[TimelineViewController alloc] initWithSession:_session roomId:_roomIds[index]]
                                         animated:NO];
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return static_cast<NSInteger>([self isInviteSection:section] ? _inviteIds.size() : _roomIds.size());
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:@"room" forIndexPath:indexPath];
    if ([self isInviteSection:indexPath.section]) {
        const auto& invite = _session.state.Invites.at(_inviteIds[static_cast<size_t>(indexPath.row)]);
        UIListContentConfiguration* content = UIListContentConfiguration.subtitleCellConfiguration;
        content.text = invite.Name.empty() ? [NSString stringWithFormat:@"Chat with %@", ToNs(invite.Inviter)] : ToNs(invite.Name);
        content.secondaryText = [NSString stringWithFormat:@"Invited by %@", ToNs(invite.Inviter)];
        content.image = [UIImage systemImageNamed:invite.Encrypted ? @"lock.fill" : @"envelope"];
        cell.contentConfiguration = content;
        cell.accessoryType = UITableViewCellAccessoryNone;
        return cell;
    }
    const auto& room = _session.state.Rooms.at(_roomIds[static_cast<size_t>(indexPath.row)]);
    UIListContentConfiguration* content = UIListContentConfiguration.subtitleCellConfiguration;
    content.text = ToNs(room.DisplayName(_session.userId.UTF8String));
    if (!room.Timeline.empty()) {
        const auto& last = room.Timeline.back();
        content.secondaryText = last.IsWaitingForKey() ? @"🔒 Encrypted message" : ToNs(last.Body);
    }
    // The room's own avatar; for an unnamed room, which is named after its people, the other person's
    std::string avatar = room.AvatarUrl;
    if (avatar.empty() && room.Name.empty()) {
        for (const auto& [user, url] : room.MemberAvatars) {
            if (user != _session.userId.UTF8String) {
                avatar = url;
                break;
            }
        }
    }
    if (UIImage* image = avatar.empty() ? nil : [_session avatarForMxc:avatar]) {
        content.image = image;
        content.imageProperties.maximumSize = CGSizeMake(40, 40);
        content.imageProperties.reservedLayoutSize = CGSizeMake(40, 40);
        content.imageProperties.cornerRadius = 20;
    } else if (room.Encrypted) {
        content.image = [UIImage systemImageNamed:@"lock.fill"];
        content.imageProperties.tintColor = UIColor.secondaryLabelColor;
    }
    content.secondaryTextProperties.numberOfLines = 1;
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if ([self isInviteSection:indexPath.section]) {
        const std::string roomId = _inviteIds[static_cast<size_t>(indexPath.row)];
        const auto& invite = _session.state.Invites.at(roomId);
        UIAlertController* sheet = [UIAlertController alertControllerWithTitle:invite.Name.empty() ? [NSString stringWithFormat:@"Chat with %@", ToNs(invite.Inviter)] : ToNs(invite.Name)
            message:[NSString stringWithFormat:@"Invited by %@", ToNs(invite.Inviter)] preferredStyle:UIAlertControllerStyleActionSheet];
        void (^report)(NSString*) = ^(NSString* error) {
            if (error) {
                UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Failed" message:error
                                                                        preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
                [self presentViewController:alert animated:YES completion:nil];
            }
        };
        [sheet addAction:[UIAlertAction actionWithTitle:@"Join" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
            [self->_session joinRoom:roomId completion:report];
        }]];
        [sheet addAction:[UIAlertAction actionWithTitle:@"Decline" style:UIAlertActionStyleDestructive handler:^(UIAlertAction*) {
            [self->_session declineInvite:roomId completion:report];
        }]];
        [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:sheet animated:YES completion:nil];
        return;
    }
    TimelineViewController* timeline = [[TimelineViewController alloc] initWithSession:_session
                                                                                roomId:_roomIds[static_cast<size_t>(indexPath.row)]];
    [self.navigationController pushViewController:timeline animated:YES];
}

@end
