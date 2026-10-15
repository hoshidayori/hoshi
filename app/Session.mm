#import "Session.h"
#import "Credentials.h"

#include "UrlSessionHttp.h"
#include "client.h"

#include <atomic>
#include <functional>
#include <memory>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

NSString* Describe(const NHoshi::TMatrixError& e) {
    if (e.Status == 0) {
        return @"The server could not be reached.";
    }
    return e.ErrCode.empty() ? ToNs(e.what()) : [NSString stringWithFormat:@"%@: %@", ToNs(e.ErrCode), ToNs(e.what())];
}

} // namespace

NSNotificationName const HSSessionStateDidChange = @"HSSessionStateDidChange";
NSNotificationName const HSSessionDidLoseAuthorization = @"HSSessionDidLoseAuthorization";

@implementation HSSession {
    NSString* _homeserver;
    // Sync long-polls for up to 30 s, so sending has its own client and queue and never waits behind it
    dispatch_queue_t _queue;
    std::unique_ptr<NHoshi::TClient> _client;
    dispatch_queue_t _sendQueue;
    std::unique_ptr<NHoshi::TClient> _sendClient;
    std::shared_ptr<std::atomic<bool>> _syncing;
    NHoshi::TSyncState _state;  // main queue only
    std::string _deviceId;     // main queue only
    std::string _accessToken;  // main queue only
}

- (instancetype)initWithHomeserver:(NSString*)homeserver {
    if ((self = [super init])) {
        _homeserver = [homeserver copy];
        auto http = std::make_shared<NHoshi::TUrlSessionHttp>();
        _queue = dispatch_queue_create("hoshi.sync", DISPATCH_QUEUE_SERIAL);
        _client = std::make_unique<NHoshi::TClient>(http, homeserver.UTF8String);
        _sendQueue = dispatch_queue_create("hoshi.send", DISPATCH_QUEUE_SERIAL);
        _sendClient = std::make_unique<NHoshi::TClient>(http, homeserver.UTF8String);
        _syncing = std::make_shared<std::atomic<bool>>(false);
    }
    return self;
}

- (instancetype)initWithCredentials:(HSCredentials*)credentials {
    if ((self = [self initWithHomeserver:credentials.homeserver])) {
        _userId = [credentials.userId copy];
        _deviceId = credentials.deviceId.UTF8String;
        _accessToken = credentials.accessToken.UTF8String;
        // Nothing else uses the clients yet, so they can be set up here directly
        _client->Restore(_userId.UTF8String, _deviceId, _accessToken);
        _sendClient->Restore(_userId.UTF8String, _deviceId, _accessToken);
    }
    return self;
}

- (HSCredentials*)credentials {
    HSCredentials* credentials = [[HSCredentials alloc] init];
    credentials.homeserver = _homeserver;
    credentials.userId = _userId;
    credentials.deviceId = ToNs(_deviceId);
    credentials.accessToken = ToNs(_accessToken);
    return credentials;
}

- (void)loginUser:(NSString*)user password:(NSString*)password completion:(void (^)(NSString*))completion {
    const std::string u = user.UTF8String;
    const std::string p = password.UTF8String;
    dispatch_async(_queue, ^{
        NSString* error = nil;
        try {
            self->_client->Login(u, p, "Hoshi on iOS");
        } catch (const NHoshi::TMatrixError& e) {
            error = Describe(e);
        }
        const std::string id = self->_client->UserId();
        const std::string device = self->_client->DeviceId();
        const std::string token = self->_client->AccessToken();
        dispatch_sync(self->_sendQueue, ^{
            self->_sendClient->Restore(id, device, token);
        });
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_userId = ToNs(id);
            self->_deviceId = device;
            self->_accessToken = token;
            completion(error);
        });
    });
}

- (void)logoutWithCompletion:(void (^)(void))completion {
    [self stopSyncing];
    // The send queue is idle between messages, unlike the sync queue that may wait in a long poll
    dispatch_async(_sendQueue, ^{
        try {
            self->_sendClient->Logout();
        } catch (const NHoshi::TMatrixError&) {
            // Offline: the token stays valid on the server, but it is forgotten on this device
        }
        dispatch_async(dispatch_get_main_queue(), completion);
    });
}

- (const NHoshi::TSyncState&)state {
    return _state;
}

- (void)startSyncing {
    if (_syncing->exchange(true)) {
        return;
    }
    __weak HSSession* weakSelf = self;
    [self syncOnce:[weakSelf](const NHoshi::TSyncState& state) {
        HSSession* strongSelf = weakSelf;
        if (strongSelf) {
            strongSelf->_state = state;
            [NSNotificationCenter.defaultCenter postNotificationName:HSSessionStateDidChange object:strongSelf];
        }
    } syncing:_syncing];
}

// One round, then schedules the next one
- (void)syncOnce:(std::function<void(const NHoshi::TSyncState&)>)onState syncing:(std::shared_ptr<std::atomic<bool>>)syncing {
    __weak HSSession* weakSelf = self;
    dispatch_async(_queue, ^{
        HSSession* strongSelf = weakSelf;
        if (!strongSelf || !*syncing) {
            return;
        }
        bool ok = true;
        bool unauthorized = false;
        try {
            strongSelf->_client->Sync(30000);
        } catch (const NHoshi::TMatrixError& e) {
            ok = false;
            unauthorized = e.ErrCode == "M_UNKNOWN_TOKEN";
        }
        if (unauthorized) {
            *syncing = false;
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSNotificationCenter.defaultCenter postNotificationName:HSSessionDidLoseAuthorization object:weakSelf];
            });
            return;
        }
        auto snapshot = std::make_shared<NHoshi::TSyncState>(strongSelf->_client->State());
        dispatch_async(dispatch_get_main_queue(), ^{
            onState(*snapshot);
        });
        // After an error, wait a little instead of hammering the server
        const int64_t delay = ok ? 0 : 3 * NSEC_PER_SEC;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay), dispatch_get_main_queue(), ^{
            [weakSelf syncOnce:onState syncing:syncing];
        });
    });
}

- (void)stopSyncing {
    *_syncing = false;
}

- (void)sendText:(NSString*)text toRoom:(const std::string&)roomId completion:(void (^)(NSString*))completion {
    const std::string body = text.UTF8String;
    const std::string room = roomId;
    const std::string txnId = NSUUID.UUID.UUIDString.UTF8String;
    dispatch_async(_sendQueue, ^{
        NSString* error = nil;
        try {
            self->_sendClient->SendText(room, body, txnId);
        } catch (const NHoshi::TMatrixError& e) {
            error = Describe(e);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(error);
        });
    });
}

@end
