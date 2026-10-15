#import "Session.h"
#import "Credentials.h"

#include "UrlSessionHttp.h"
#include "client.h"
#include "e2ee.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {

// The crypto state holds the device's private keys and every room key it received. It lives in a file in
// the app's own Library, encrypted by iOS until the first unlock after a restart, so syncing can resume
// in the background. The access token stays in the Keychain.
NSURL* CryptoFile() {
    NSURL* library = [NSFileManager.defaultManager URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask].firstObject;
    return [library URLByAppendingPathComponent:@"hoshi-crypto.json"];
}

std::optional<NHoshi::NE2ee::TMachine> LoadMachine(const std::string& userId, const std::string& deviceId) {
    NSData* data = [NSData dataWithContentsOfURL:CryptoFile()];
    if (!data) {
        return std::nullopt;
    }
    try {
        auto machine = NHoshi::NE2ee::TMachine::Load(NHoshi::TJson::Parse(
            std::string(static_cast<const char*>(data.bytes), data.length)));
        // A state for another account or device is useless here; a fresh one replaces it
        if (machine && machine->UserId() == userId && machine->DeviceId() == deviceId) {
            return machine;
        }
    } catch (const std::runtime_error&) {
    }
    return std::nullopt;
}

void SaveMachine(const NHoshi::NE2ee::TMachine& machine) {
    const std::string json = machine.Save().Dump();
    NSData* data = [NSData dataWithBytes:json.data() length:json.size()];
    if (![data writeToURL:CryptoFile() options:NSDataWritingAtomic | NSDataWritingFileProtectionCompleteUntilFirstUserAuthentication error:nil]) {
        NSLog(@"Hoshi: could not save the crypto state");
    }
}

constexpr int OneTimeKeyTarget = 50;

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

NSString* Describe(const NHoshi::TMatrixError& e) {
    if (e.Status == 0) {
        return @"The server could not be reached.";
    }
    return e.ErrCode.empty() ? ToNs(e.what()) : [NSString stringWithFormat:@"%@: %@", ToNs(e.ErrCode), ToNs(e.what())];
}

// One client per queue: TClient is not thread-safe, and each queue must not wait behind another's request
struct TWorker {
    dispatch_queue_t Queue;
    std::unique_ptr<NHoshi::TClient> Client;

    TWorker(const char* name, std::shared_ptr<NHoshi::IHttp> http, const std::string& homeserver)
        : Queue(dispatch_queue_create(name, DISPATCH_QUEUE_SERIAL))
        , Client(std::make_unique<NHoshi::TClient>(std::move(http), homeserver))
    {}
};

} // namespace

NSNotificationName const HSSessionStateDidChange = @"HSSessionStateDidChange";
NSNotificationName const HSSessionDidLoseAuthorization = @"HSSessionDidLoseAuthorization";

@implementation HSSession {
    NSString* _homeserver;
    // Sync long-polls for up to 30 s; sending and loading history have their own clients and queues
    std::unique_ptr<TWorker> _sync;
    std::unique_ptr<TWorker> _send;
    std::unique_ptr<TWorker> _history;
    std::shared_ptr<std::atomic<bool>> _syncing;

    // The rooms, written by the sync and history queues. Network calls happen outside the lock.
    std::mutex _coreMutex;
    NHoshi::TSyncState _core;
    // Encryption state. When both locks are needed, _cryptoMutex is taken first.
    std::mutex _cryptoMutex;
    std::optional<NHoshi::NE2ee::TMachine> _machine;
    std::atomic<bool> _uploadingKeys;

    NHoshi::TSyncState _state;  // main-queue copy for the screens
    std::string _deviceId;      // main queue only
    std::string _accessToken;   // main queue only
}

- (instancetype)initWithHomeserver:(NSString*)homeserver {
    if ((self = [super init])) {
        _homeserver = [homeserver copy];
        auto http = std::make_shared<NHoshi::TUrlSessionHttp>();
        const std::string hs = homeserver.UTF8String;
        _sync = std::make_unique<TWorker>("hoshi.sync", http, hs);
        _send = std::make_unique<TWorker>("hoshi.send", http, hs);
        _history = std::make_unique<TWorker>("hoshi.history", http, hs);
        _syncing = std::make_shared<std::atomic<bool>>(false);
    }
    return self;
}

- (void)restoreWorkersUser:(const std::string&)userId device:(const std::string&)device token:(const std::string&)token {
    for (TWorker* worker : {_send.get(), _history.get()}) {
        dispatch_sync(worker->Queue, ^{
            worker->Client->Restore(userId, device, token);
        });
    }
}

- (instancetype)initWithCredentials:(HSCredentials*)credentials {
    if ((self = [self initWithHomeserver:credentials.homeserver])) {
        _userId = [credentials.userId copy];
        _deviceId = credentials.deviceId.UTF8String;
        _accessToken = credentials.accessToken.UTF8String;
        _sync->Client->Restore(_userId.UTF8String, _deviceId, _accessToken);
        [self restoreWorkersUser:_userId.UTF8String device:_deviceId token:_accessToken];
        [self setUpCryptoForUser:_userId.UTF8String device:_deviceId];
    }
    return self;
}

// Loads the device's crypto state, or creates it and publishes the device keys; runs the upload on the send queue
- (void)setUpCryptoForUser:(const std::string&)userId device:(const std::string&)deviceId {
    const std::string user = userId;
    const std::string device = deviceId;
    dispatch_async(_send->Queue, ^{
        bool fresh = false;
        {
            std::lock_guard lock(self->_cryptoMutex);
            self->_machine = LoadMachine(user, device);
            if (!self->_machine) {
                self->_machine.emplace(user, device, NHoshi::NOlm::TAccount::Create());
                fresh = true;
                SaveMachine(*self->_machine);
            }
        }
        if (fresh) {
            [self uploadKeysWithDeviceKeys:YES count:OneTimeKeyTarget];
        }
    });
}

// Runs on the send queue. The keys are published only after the server accepted them.
- (void)uploadKeysWithDeviceKeys:(BOOL)withDeviceKeys count:(int)count {
    if (_uploadingKeys.exchange(true)) {
        return;
    }
    NHoshi::TJson body;
    {
        std::lock_guard lock(_cryptoMutex);
        if (!_machine) {
            _uploadingKeys = false;
            return;
        }
        body = _machine->KeysUploadBody(static_cast<size_t>(count), withDeviceKeys);
        SaveMachine(*_machine);  // the new one-time keys must survive a crash before the upload
    }
    try {
        _send->Client->UploadKeys(body);
        std::lock_guard lock(_cryptoMutex);
        _machine->MarkKeysAsPublished();
        SaveMachine(*_machine);
    } catch (const NHoshi::TMatrixError& e) {
        NSLog(@"Hoshi: key upload failed: %s", e.what());
    }
    _uploadingKeys = false;
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
    dispatch_async(_sync->Queue, ^{
        NSString* error = nil;
        try {
            self->_sync->Client->Login(u, p, "Hoshi on iOS");
        } catch (const NHoshi::TMatrixError& e) {
            error = Describe(e);
        }
        const std::string id = self->_sync->Client->UserId();
        const std::string device = self->_sync->Client->DeviceId();
        const std::string token = self->_sync->Client->AccessToken();
        [self restoreWorkersUser:id device:device token:token];
        if (!error) {
            [self setUpCryptoForUser:id device:device];
        }
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
    dispatch_async(_send->Queue, ^{
        try {
            self->_send->Client->Logout();
        } catch (const NHoshi::TMatrixError&) {
            // Offline: the token stays valid on the server, but it is forgotten on this device
        }
        {
            // The device is gone, and so are its keys
            std::lock_guard lock(self->_cryptoMutex);
            self->_machine.reset();
            [NSFileManager.defaultManager removeItemAtURL:CryptoFile() error:nil];
        }
        dispatch_async(dispatch_get_main_queue(), completion);
    });
}

- (const NHoshi::TSyncState&)state {
    return _state;
}

// Copies the shared state to the main queue and tells the screens
- (void)publish:(std::shared_ptr<NHoshi::TSyncState>)snapshot {
    __weak HSSession* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        HSSession* strongSelf = weakSelf;
        if (strongSelf) {
            strongSelf->_state = *snapshot;
            [NSNotificationCenter.defaultCenter postNotificationName:HSSessionStateDidChange object:strongSelf];
        }
    });
}

- (void)startSyncing {
    if (_syncing->exchange(true)) {
        return;
    }
    [self syncOnce:_syncing];
}

// One round, then schedules the next one
- (void)syncOnce:(std::shared_ptr<std::atomic<bool>>)syncing {
    __weak HSSession* weakSelf = self;
    dispatch_async(_sync->Queue, ^{
        HSSession* strongSelf = weakSelf;
        if (!strongSelf || !*syncing) {
            return;
        }
        std::string since;
        {
            std::lock_guard lock(strongSelf->_coreMutex);
            since = strongSelf->_core.NextBatch;
        }
        bool ok = true;
        bool unauthorized = false;
        NHoshi::TJson response;
        try {
            response = strongSelf->_sync->Client->FetchSync(since, 30000);
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
        if (ok) {
            std::shared_ptr<NHoshi::TSyncState> snapshot;
            int oneTimeKeysLeft = OneTimeKeyTarget;
            {
                std::lock_guard cryptoLock(strongSelf->_cryptoMutex);
                bool cryptoChanged = false;
                // Room keys come as to-device messages; take them before the timeline, so its messages decrypt at once
                if (strongSelf->_machine && response["to_device"]["events"].IsArray()) {
                    for (const auto& event : response["to_device"]["events"].AsArray()) {
                        cryptoChanged = strongSelf->_machine->ReceiveToDevice(event) || cryptoChanged;
                    }
                    // A pre-key message uses up one of our one-time keys even when it carries something else
                    cryptoChanged = cryptoChanged || !response["to_device"]["events"].AsArray().empty();
                }
                if (cryptoChanged) {
                    SaveMachine(*strongSelf->_machine);
                }
                const NHoshi::TJson& counts = response["device_one_time_keys_count"];
                if (counts.IsObject()) {
                    oneTimeKeysLeft = counts["signed_curve25519"].IsInt() ? static_cast<int>(counts["signed_curve25519"].AsInt()) : 0;
                }
                std::lock_guard lock(strongSelf->_coreMutex);
                strongSelf->_core.Apply(response);
                if (strongSelf->_machine) {
                    NHoshi::NE2ee::DecryptWaiting(strongSelf->_core, *strongSelf->_machine);
                }
                snapshot = std::make_shared<NHoshi::TSyncState>(strongSelf->_core);
            }
            [strongSelf publish:snapshot];
            // Keep enough one-time keys on the server for other devices to start sessions with us
            if (oneTimeKeysLeft < OneTimeKeyTarget / 2) {
                const int missing = OneTimeKeyTarget - oneTimeKeysLeft;
                dispatch_async(strongSelf->_send->Queue, ^{
                    [weakSelf uploadKeysWithDeviceKeys:NO count:missing];
                });
            }
        }
        // After an error, wait a little instead of hammering the server
        const int64_t delay = ok ? 0 : 3 * NSEC_PER_SEC;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay), dispatch_get_main_queue(), ^{
            [weakSelf syncOnce:syncing];
        });
    });
}

- (void)stopSyncing {
    *_syncing = false;
}

- (void)loadOlderInRoom:(const std::string&)roomId completion:(void (^)(BOOL more))completion {
    const std::string room = roomId;
    dispatch_async(_history->Queue, ^{
        std::string from;
        bool complete = true;
        {
            std::lock_guard lock(self->_coreMutex);
            const auto it = self->_core.Rooms.find(room);
            if (it != self->_core.Rooms.end()) {
                from = it->second.PrevBatch;
                complete = it->second.HistoryComplete || from.empty();
            }
        }
        BOOL more = NO;
        if (!complete) {
            try {
                const NHoshi::TJson response = self->_history->Client->FetchMessages(room, from, 50);
                std::shared_ptr<NHoshi::TSyncState> snapshot;
                {
                    std::lock_guard cryptoLock(self->_cryptoMutex);
                    std::lock_guard lock(self->_coreMutex);
                    // A limited sync may have reset the room meanwhile; then this page no longer fits
                    const auto it = self->_core.Rooms.find(room);
                    if (it != self->_core.Rooms.end() && it->second.PrevBatch == from) {
                        self->_core.PrependHistory(room, response);
                        more = !it->second.HistoryComplete;
                        if (self->_machine) {
                            NHoshi::NE2ee::DecryptWaiting(self->_core, *self->_machine);
                        }
                    }
                    snapshot = std::make_shared<NHoshi::TSyncState>(self->_core);
                }
                [self publish:snapshot];
            } catch (const NHoshi::TMatrixError&) {
                more = YES;  // try again on the next scroll
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(more);
        });
    });
}

- (void)sendText:(NSString*)text toRoom:(const std::string&)roomId {
    const std::string body = text.UTF8String;
    const std::string txnId = NSUUID.UUID.UUIDString.UTF8String;
    const std::int64_t nowMs = static_cast<std::int64_t>(NSDate.date.timeIntervalSince1970 * 1000);
    std::shared_ptr<NHoshi::TSyncState> snapshot;
    {
        std::lock_guard lock(_coreMutex);
        _core.AddPending(roomId, txnId, _userId.UTF8String, body, nowMs);
        snapshot = std::make_shared<NHoshi::TSyncState>(_core);
    }
    [self publish:snapshot];
    [self sendPendingText:body room:roomId txnId:txnId];
}

- (void)retryMessage:(const std::string&)txnId inRoom:(const std::string&)roomId {
    std::string body;
    std::shared_ptr<NHoshi::TSyncState> snapshot;
    {
        std::lock_guard lock(_coreMutex);
        const NHoshi::TMessage* pending = _core.RetryPending(roomId, txnId);
        if (!pending) {
            return;
        }
        body = pending->Body;
        snapshot = std::make_shared<NHoshi::TSyncState>(_core);
    }
    [self publish:snapshot];
    [self sendPendingText:body room:roomId txnId:txnId];
}

// Runs on the send queue. Before the first message of the room's Megolm session, and whenever a member has a
// device that has not got its key yet, the key goes to those devices over Olm; then the message is encrypted.
// Throws TMatrixError on network failures and std::runtime_error when there is no crypto state.
- (std::string)sendEncryptedText:(const std::string&)text room:(const std::string&)room
                         members:(const std::vector<std::string>&)members txnId:(const std::string&)txnId {
    const auto devices = NHoshi::NE2ee::DevicesFromQuery(_send->Client->QueryKeys(members));
    NHoshi::TJson claimRequest;
    {
        std::lock_guard lock(_cryptoMutex);
        if (!_machine) {
            throw std::runtime_error("no crypto state");
        }
        claimRequest = _machine->ClaimRequestFor(room, devices);
    }
    const NHoshi::TJson claimed = claimRequest.AsObject().empty() ? NHoshi::TJson(NHoshi::TJson::TObject{})
                                                                  : _send->Client->ClaimKeys(claimRequest);
    NHoshi::NE2ee::TToDevice toDevice;
    NHoshi::TJson content;
    {
        std::lock_guard lock(_cryptoMutex);
        toDevice = _machine->ShareRoomKey(room, devices, claimed);
        content = _machine->EncryptRoomEvent(room, "m.room.message",
            NHoshi::TJson(NHoshi::TJson::TObject{{"msgtype", "m.text"}, {"body", text}}));
        SaveMachine(*_machine);
    }
    if (!toDevice.empty()) {
        NHoshi::TJson::TObject messages;
        for (const auto& [user, perDevice] : toDevice) {
            messages[user] = NHoshi::TJson(NHoshi::TJson::TObject(perDevice.begin(), perDevice.end()));
        }
        // The key has to be on its way before the message, or the recipients wait for it
        _send->Client->SendToDevice("m.room.encrypted", NHoshi::TJson(std::move(messages)), txnId + "-keys");
    }
    return _send->Client->SendEvent(room, "m.room.encrypted", content, txnId);
}

// The same transaction id on a retry makes the server treat it as the same message, so a send that did
// reach the server before the connection broke is not duplicated
- (void)sendPendingText:(const std::string&)body room:(const std::string&)roomId txnId:(const std::string&)txnId {
    const std::string room = roomId;
    const std::string txn = txnId;
    const std::string text = body;
    dispatch_async(_send->Queue, ^{
        std::string eventId;
        bool ok = true;
        try {
            bool encrypted = false;
            std::vector<std::string> members;
            {
                std::lock_guard lock(self->_coreMutex);
                const auto it = self->_core.Rooms.find(room);
                if (it != self->_core.Rooms.end()) {
                    encrypted = it->second.Encrypted;
                    members.assign(it->second.Members.begin(), it->second.Members.end());
                }
            }
            if (encrypted) {
                eventId = [self sendEncryptedText:text room:room members:members txnId:txn];
            } else {
                eventId = self->_send->Client->SendText(room, text, txn);
            }
        } catch (const NHoshi::TMatrixError&) {
            ok = false;
        } catch (const std::runtime_error& e) {
            NSLog(@"Hoshi: could not encrypt: %s", e.what());
            ok = false;
        }
        std::shared_ptr<NHoshi::TSyncState> snapshot;
        {
            std::lock_guard lock(self->_coreMutex);
            if (ok) {
                self->_core.MarkPendingSent(room, txn, eventId);
            } else {
                self->_core.MarkPendingFailed(room, txn);
            }
            snapshot = std::make_shared<NHoshi::TSyncState>(self->_core);
        }
        [self publish:snapshot];
    });
}

@end
