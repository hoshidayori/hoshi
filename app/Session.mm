#import "Session.h"

#include "UrlSessionHttp.h"
#include "client.h"

#include <atomic>
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

@implementation HSSession {
    // Sync long-polls for up to 30 s, so sending has its own client and queue and never waits behind it
    dispatch_queue_t _queue;
    std::unique_ptr<NHoshi::TClient> _client;
    dispatch_queue_t _sendQueue;
    std::unique_ptr<NHoshi::TClient> _sendClient;
    std::shared_ptr<std::atomic<bool>> _syncing;
    NHoshi::TSyncState _state;  // main queue only
}

- (instancetype)initWithHomeserver:(NSString*)homeserver {
    if ((self = [super init])) {
        auto http = std::make_shared<NHoshi::TUrlSessionHttp>();
        _queue = dispatch_queue_create("hoshi.sync", DISPATCH_QUEUE_SERIAL);
        _client = std::make_unique<NHoshi::TClient>(http, homeserver.UTF8String);
        _sendQueue = dispatch_queue_create("hoshi.send", DISPATCH_QUEUE_SERIAL);
        _sendClient = std::make_unique<NHoshi::TClient>(http, homeserver.UTF8String);
        _syncing = std::make_shared<std::atomic<bool>>(false);
    }
    return self;
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
        NSString* userId = ToNs(self->_client->UserId());
        std::string id = self->_client->UserId();
        std::string device = self->_client->DeviceId();
        std::string token = self->_client->AccessToken();
        dispatch_sync(self->_sendQueue, ^{
            self->_sendClient->Restore(id, device, token);
        });
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_userId = userId;
            completion(error);
        });
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
        try {
            strongSelf->_client->Sync(30000);
        } catch (const NHoshi::TMatrixError&) {
            ok = false;
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
