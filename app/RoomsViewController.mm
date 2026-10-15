#import "RoomsViewController.h"
#import "Session.h"
#import "TimelineViewController.h"

#include <string>
#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

} // namespace

@implementation RoomsViewController {
    HSSession* _session;
    std::vector<std::string> _roomIds;
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
    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:@"room"];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(stateChanged)
                                               name:HSSessionStateDidChange object:_session];
    [self stateChanged];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)stateChanged {
    _roomIds.clear();
    for (const auto& [id, room] : _session.state.Rooms) {
        _roomIds.push_back(id);
    }
    [self.tableView reloadData];
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return static_cast<NSInteger>(_roomIds.size());
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:@"room" forIndexPath:indexPath];
    const auto& room = _session.state.Rooms.at(_roomIds[static_cast<size_t>(indexPath.row)]);
    UIListContentConfiguration* content = UIListContentConfiguration.subtitleCellConfiguration;
    content.text = room.Name.empty() ? ToNs(room.Id) : ToNs(room.Name);
    if (room.Encrypted) {
        content.secondaryText = @"Encrypted — not supported yet";
    } else if (!room.Timeline.empty()) {
        content.secondaryText = ToNs(room.Timeline.back().Body);
    }
    content.secondaryTextProperties.numberOfLines = 1;
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    TimelineViewController* timeline = [[TimelineViewController alloc] initWithSession:_session
                                                                                roomId:_roomIds[static_cast<size_t>(indexPath.row)]];
    [self.navigationController pushViewController:timeline animated:YES];
}

@end
