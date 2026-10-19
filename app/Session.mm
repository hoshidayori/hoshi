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
NSNotificationName const HSSessionVerificationDidChange = @"HSSessionVerificationDidChange";
NSNotificationName const HSSessionAvatarsDidChange = @"HSSessionAvatarsDidChange";

namespace {

bool IsVerificationEvent(const std::string& type) {
    return type.rfind("m.key.verification.", 0) == 0;
}

std::int64_t NowMs() {
    return static_cast<std::int64_t>(NSDate.date.timeIntervalSince1970 * 1000);
}

} // namespace

@implementation HSSession {
    NSString* _homeserver;
    // Sync long-polls for up to 30 s; sending and loading history have their own clients and queues
    std::unique_ptr<TWorker> _sync;
    std::unique_ptr<TWorker> _send;
    std::unique_ptr<TWorker> _history;
    std::unique_ptr<TWorker> _media;
    std::shared_ptr<std::atomic<bool>> _syncing;
    // Avatars by mxc URI, main queue only: pictures in memory, and the ones being fetched or not available
    NSCache<NSString*, UIImage*>* _avatars;
    NSMutableSet<NSString*>* _avatarsAsked;
    NSMutableSet<NSString*>* _avatarsMissing;  // asked, and neither the cache nor the server had a picture

    // The rooms, written by the sync and history queues. Network calls happen outside the lock.
    std::mutex _coreMutex;
    NHoshi::TSyncState _core;
    // Encryption state. When both locks are needed, _cryptoMutex is taken first.
    std::mutex _cryptoMutex;
    std::optional<NHoshi::NE2ee::TMachine> _machine;
    std::atomic<bool> _uploadingKeys;
    std::optional<NHoshi::NVerification::TVerification> _verificationCore;  // under _cryptoMutex
    std::uint64_t _verificationVersion;                                    // under _cryptoMutex
    // Users' cross-signing keys from their latest /keys/query, for the master keys a verification covers
    std::map<std::string, NHoshi::NE2ee::TCrossSigning> _crossSigning;    // under _cryptoMutex
    std::optional<NHoshi::NVerification::TVerification> _verification;      // main-queue copy
    std::uint64_t _verificationShown;                                      // main queue: the version of that copy

    // Main queue: the latest snapshot for the screens, shared rather than copied again (a long room is thousands
    // of messages)
    std::shared_ptr<const NHoshi::TSyncState> _state;
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
        _media = std::make_unique<TWorker>("hoshi.media", http, hs);
        _syncing = std::make_shared<std::atomic<bool>>(false);
        _avatars = [[NSCache alloc] init];
        _avatarsAsked = [NSMutableSet set];
        _avatarsMissing = [NSMutableSet set];
    }
    return self;
}

- (void)restoreWorkersUser:(const std::string&)userId device:(const std::string&)device token:(const std::string&)token {
    for (TWorker* worker : {_send.get(), _history.get(), _media.get()}) {
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
    static const NHoshi::TSyncState empty;
    return _state ? *_state : empty;
}

// Hands a snapshot of the shared state to the main queue and tells the screens. The snapshot is a copy made
// for this, and nothing changes it afterwards.
- (void)publish:(std::shared_ptr<NHoshi::TSyncState>)snapshot {
    __weak HSSession* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        HSSession* strongSelf = weakSelf;
        if (strongSelf) {
            strongSelf->_state = snapshot;
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
            std::vector<NHoshi::TJson> verificationEvents;
            {
                std::lock_guard cryptoLock(strongSelf->_cryptoMutex);
                bool cryptoChanged = false;
                // Room keys come as to-device messages; take them before the timeline, so its messages decrypt at once
                if (strongSelf->_machine && response["to_device"]["events"].IsArray()) {
                    for (const auto& event : response["to_device"]["events"].AsArray()) {
                        if (IsVerificationEvent(event.StringOr("type"))) {
                            verificationEvents.push_back(event);
                            continue;
                        }
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
            for (const auto& event : verificationEvents) {
                [strongSelf handleVerificationEvent:event];
            }
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

- (void)changeMembership:(const std::string&)roomId join:(BOOL)join completion:(void (^)(NSString*))completion {
    const std::string room = roomId;
    dispatch_async(_send->Queue, ^{
        NSString* error = nil;
        try {
            if (join) {
                self->_send->Client->JoinRoom(room);
            } else {
                self->_send->Client->LeaveRoom(room);
            }
        } catch (const NHoshi::TMatrixError& e) {
            error = Describe(e);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(error);
        });
    });
}

- (void)joinRoom:(const std::string&)roomId completion:(void (^)(NSString*))completion {
    [self changeMembership:roomId join:YES completion:completion];
}

- (void)declineInvite:(const std::string&)roomId completion:(void (^)(NSString*))completion {
    [self changeMembership:roomId join:NO completion:completion];
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

// Runs on the send queue. The room's Megolm session is replaced when it is due (see PrepareRoomKey). Before the
// first message of a session, and whenever a member has a device that has not got its key yet, the key goes to
// those devices over Olm; then the message is encrypted.
// Throws TMatrixError on network failures and std::runtime_error when there is no crypto state.
- (std::string)sendEncryptedText:(const std::string&)text room:(const std::string&)room
                         members:(const std::vector<std::string>&)members rotation:(const NHoshi::NE2ee::TRotation&)rotation
                           txnId:(const std::string&)txnId {
    const auto devices = NHoshi::NE2ee::DevicesFromQuery(_send->Client->QueryKeys(members));
    const std::int64_t nowMs = static_cast<std::int64_t>(NSDate.date.timeIntervalSince1970 * 1000);
    NHoshi::TJson claimRequest;
    {
        std::lock_guard lock(_cryptoMutex);
        if (!_machine) {
            throw std::runtime_error("no crypto state");
        }
        _machine->PrepareRoomKey(room, devices, rotation, nowMs);
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
            NHoshi::NE2ee::TRotation rotation;
            {
                std::lock_guard lock(self->_coreMutex);
                const auto it = self->_core.Rooms.find(room);
                if (it != self->_core.Rooms.end()) {
                    encrypted = it->second.Encrypted;
                    members.assign(it->second.Members.begin(), it->second.Members.end());
                    rotation = {it->second.RotationPeriodMs, it->second.RotationPeriodMsgs};
                }
            }
            if (encrypted) {
                eventId = [self sendEncryptedText:text room:room members:members rotation:rotation txnId:txn];
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

#pragma mark - Avatars

// Library/Caches/avatars/<server>_<media id>: the system may clear it, then they are fetched again
static NSURL* AvatarFile(const NHoshi::TMxc& mxc) {
    NSURL* caches = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL* folder = [caches URLByAppendingPathComponent:@"avatars" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
    // ParseMxc allows only letters, digits and ".-_:[]"; the last three become "_" in a file name
    std::string name = mxc.Server + "_" + mxc.MediaId;
    for (char& c : name) {
        if (c == ':' || c == '[' || c == ']') {
            c = '_';
        }
    }
    return [folder URLByAppendingPathComponent:@(name.c_str())];
}

- (UIImage*)avatarForMxc:(const std::string&)uri {
    const auto mxc = NHoshi::ParseMxc(uri);
    if (!mxc) {
        return nil;
    }
    NSString* key = @(uri.c_str());
    if (UIImage* image = [_avatars objectForKey:key]) {
        return image;
    }
    if ([_avatarsAsked containsObject:key]) {
        return nil;  // on its way, or the server has none
    }
    // Nothing here waits: the cache on disk is read on the media queue too, and the picture is decoded there,
    // so that neither building the timeline nor drawing it stops for it
    [_avatarsAsked addObject:key];
    const std::string mxcUri = uri;
    const NHoshi::TMxc parsed = *mxc;
    __weak HSSession* weakSelf = self;
    dispatch_async(_media->Queue, ^{
        HSSession* strongSelf = weakSelf;
        if (!strongSelf) {
            return;
        }
        NSURL* file = AvatarFile(parsed);
        NSData* data = [NSData dataWithContentsOfURL:file];
        const bool cached = data != nil;
        if (!data) {
            try {
                // Twice the drawn size, for sharp circles on Retina screens
                const std::string bytes = strongSelf->_media->Client->FetchThumbnail(mxcUri, 96);
                data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
            } catch (const NHoshi::TMatrixError& e) {
                NSLog(@"Hoshi: no avatar %s: %s", mxcUri.c_str(), e.what());
            }
        }
        UIImage* image = [[UIImage imageWithData:data] imageByPreparingForDisplay];
        if (image && !cached) {
            [data writeToURL:file atomically:YES];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            HSSession* session = weakSelf;
            if (!session) {
                return;
            }
            if (image) {
                [session->_avatars setObject:image forKey:key];
            } else {
                [session->_avatarsMissing addObject:key];
            }
            [NSNotificationCenter.defaultCenter postNotificationName:HSSessionAvatarsDidChange object:session];
        });
    });
    return nil;
}

- (BOOL)avatarIsLoading:(const std::string&)uri {
    if (!NHoshi::ParseMxc(uri)) {
        return NO;
    }
    NSString* key = @(uri.c_str());
    return ![_avatars objectForKey:key] && ![_avatarsMissing containsObject:key];
}

#pragma mark - Device verification

- (const std::optional<NHoshi::NVerification::TVerification>&)verification {
    return _verification;
}

// Runs with _cryptoMutex held
- (NHoshi::NVerification::TIdentity)identityLocked {
    return {_machine->UserId(), _machine->DeviceId(), _machine->Account().FingerprintKey()};
}

// Runs with _cryptoMutex held
- (void)rememberCrossSigningLocked:(const NHoshi::TJson&)queryResponse {
    for (auto& [user, keys] : NHoshi::NE2ee::CrossSigningFromQuery(queryResponse)) {
        _crossSigning[user] = std::move(keys);
    }
}

// Runs with _cryptoMutex held: the master keys of both users, as far as /keys/query told us
- (void)setMasterKeysLocked:(NHoshi::NVerification::TVerification&)verification {
    const auto masterOf = [&](const std::string& user) {
        const auto it = _crossSigning.find(user);
        return it == _crossSigning.end() ? std::string() : it->second.Master;
    };
    verification.SetMasterKeys(masterOf(_machine->UserId()), masterOf(verification.Them().UserId));
}

static bool IsActive(const std::optional<NHoshi::NVerification::TVerification>& verification) {
    using NHoshi::NVerification::EState;
    return verification && verification->State() != EState::Done && verification->State() != EState::Cancelled;
}

// Applies a change to the verification in progress, sends what it wants to send, records a verified device
// and tells the screens. Takes _cryptoMutex.
- (void)changeVerification:(void (^)(NHoshi::NVerification::TVerification& verification))change {
    std::vector<NHoshi::NVerification::TOutgoing> outgoing;
    std::optional<NHoshi::NVerification::TVerification> snapshot;
    std::uint64_t version = 0;
    {
        std::lock_guard lock(_cryptoMutex);
        if (!_verificationCore || !_machine) {
            return;
        }
        change(*_verificationCore);
        outgoing = _verificationCore->TakeOutgoing();
        if (_verificationCore->State() == NHoshi::NVerification::EState::Done) {
            const auto& them = _verificationCore->Them();
            const std::string& master = _verificationCore->VerifiedMasterKey();
            const bool newMaster = !master.empty() && !_machine->IsMasterKeyTrusted(them.UserId, master);
            if (!_machine->IsVerified(them) || newMaster) {
                _machine->MarkVerified(them);
                if (newMaster) {
                    // The other side's MAC covered its master key: the devices it cross-signs are trusted too
                    _machine->TrustMasterKey(them.UserId, master);
                }
                SaveMachine(*_machine);
            }
        }
        snapshot = _verificationCore;
        version = ++_verificationVersion;
    }
    [self sendVerificationMessages:std::move(outgoing) to:snapshot->Them()];
    // From a button on the main queue, at once, so a screen opened right after it shows the new state. A copy
    // from another queue that arrives later but is older is dropped.
    void (^show)(HSSession*) = ^(HSSession* session) {
        if (version > session->_verificationShown) {
            session->_verificationShown = version;
            session->_verification = snapshot;
            [NSNotificationCenter.defaultCenter postNotificationName:HSSessionVerificationDidChange object:session];
        }
    };
    if (NSThread.isMainThread) {
        show(self);
        return;
    }
    __weak HSSession* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (HSSession* strongSelf = weakSelf) {
            show(strongSelf);
        }
    });
}

// In order, on the send queue. Verification messages go unencrypted, as the spec allows for to-device ones:
// the MACs, not secrecy, protect the outcome.
- (void)sendVerificationMessages:(std::vector<NHoshi::NVerification::TOutgoing>)messages to:(const NHoshi::NE2ee::TDevice&)device {
    if (messages.empty()) {
        return;
    }
    const auto them = device;
    auto queued = std::make_shared<std::vector<NHoshi::NVerification::TOutgoing>>(std::move(messages));
    dispatch_async(_send->Queue, ^{
        for (const auto& message : *queued) {
            const NHoshi::TJson body(NHoshi::TJson::TObject{
                {them.UserId, NHoshi::TJson::TObject{{them.DeviceId, message.Content}}},
            });
            try {
                self->_send->Client->SendToDevice(message.Type, body, NSUUID.UUID.UUIDString.UTF8String);
            } catch (const NHoshi::TMatrixError& e) {
                NSLog(@"Hoshi: could not send %s: %s", message.Type.c_str(), e.what());
            }
        }
    });
}

// Runs on the sync queue, outside the locks
- (void)handleVerificationEvent:(const NHoshi::TJson&)event {
    const std::string type = event.StringOr("type");
    const std::string sender = event.StringOr("sender");
    const NHoshi::TJson& content = event["content"];
    if (type != "m.key.verification.request") {
        [self changeVerification:^(NHoshi::NVerification::TVerification& verification) {
            verification.Receive(type, content, sender);
        }];
        return;
    }
    // The keys the server has for the asking device: the verification checks them. Our own user's keys too, for
    // our master key.
    std::optional<NHoshi::NE2ee::TDevice> them;
    NHoshi::TJson keys;
    try {
        std::vector<std::string> users{sender};
        if (sender != _sync->Client->UserId()) {
            users.push_back(_sync->Client->UserId());
        }
        keys = _sync->Client->QueryKeys(users);
        for (const auto& device : NHoshi::NE2ee::DevicesFromQuery(keys)) {
            if (device.UserId == sender && device.DeviceId == content.StringOr("from_device")) {
                them = device;
            }
        }
    } catch (const NHoshi::TMatrixError& e) {
        NSLog(@"Hoshi: no keys for the verification request: %s", e.what());
    }
    if (!them) {
        return;
    }
    {
        std::lock_guard lock(_cryptoMutex);
        if (!_machine || IsActive(_verificationCore)) {
            return;  // one at a time; the other side times out
        }
        [self rememberCrossSigningLocked:keys];
        auto request = NHoshi::NVerification::TVerification::FromRequest([self identityLocked], *them, content, NowMs());
        if (!request) {
            return;
        }
        [self setMasterKeysLocked:*request];
        _verificationCore = std::move(request);
    }
    [self changeVerification:^(NHoshi::NVerification::TVerification&) {}];
}

- (void)requestVerificationOfDevice:(const NHoshi::NE2ee::TDevice&)device {
    std::vector<NHoshi::NVerification::TOutgoing> cancel;
    NHoshi::NE2ee::TDevice previous;
    {
        std::lock_guard lock(_cryptoMutex);
        if (!_machine) {
            return;
        }
        if (IsActive(_verificationCore)) {
            _verificationCore->Cancel("m.user", "Replaced by another verification");
            cancel = _verificationCore->TakeOutgoing();
            previous = _verificationCore->Them();
        }
        _verificationCore = NHoshi::NVerification::TVerification::Request([self identityLocked], device,
            NSUUID.UUID.UUIDString.UTF8String, NowMs());
        // The device came from the devices screen, whose /keys/query filled the cache
        [self setMasterKeysLocked:*_verificationCore];
    }
    [self sendVerificationMessages:std::move(cancel) to:previous];
    [self changeVerification:^(NHoshi::NVerification::TVerification&) {}];
}

- (void)acceptVerification {
    [self changeVerification:^(NHoshi::NVerification::TVerification& verification) {
        verification.AcceptRequest();
    }];
}

- (void)startEmojiVerification {
    [self changeVerification:^(NHoshi::NVerification::TVerification& verification) {
        verification.StartSas();
    }];
}

- (void)confirmEmoji {
    [self changeVerification:^(NHoshi::NVerification::TVerification& verification) {
        verification.ConfirmSas();
    }];
}

- (void)cancelVerificationBecauseOfMismatch:(BOOL)mismatch {
    [self changeVerification:^(NHoshi::NVerification::TVerification& verification) {
        if (mismatch) {
            verification.Cancel("m.mismatched_sas", "The emoji did not match");
        } else {
            verification.Cancel("m.user", "Cancelled by the user");
        }
    }];
}

- (void)fetchDevicesOfUser:(NSString*)userId completion:(void (^)(std::vector<HSDevice>, NSString*))completion {
    const std::string user = userId.UTF8String;
    dispatch_async(_send->Queue, ^{
        std::vector<HSDevice> devices;
        NSString* error = nil;
        try {
            std::vector<std::string> users{user};
            if (user != self->_send->Client->UserId()) {
                users.push_back(self->_send->Client->UserId());
            }
            const auto keys = self->_send->Client->QueryKeys(users);
            const auto found = NHoshi::NE2ee::DevicesFromQuery(keys);
            std::lock_guard lock(self->_cryptoMutex);
            [self rememberCrossSigningLocked:keys];
            for (const auto& device : found) {
                if (device.UserId != user ||
                    (self->_machine && device.UserId == self->_machine->UserId() && device.DeviceId == self->_machine->DeviceId()))
                {
                    continue;
                }
                const bool direct = self->_machine && self->_machine->IsVerified(device);
                const bool trusted = self->_machine && self->_machine->IsTrusted(device);
                devices.push_back({device, trusted, trusted && !direct});
            }
        } catch (const NHoshi::TMatrixError& e) {
            error = Describe(e);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(devices, error);
        });
    });
}

@end
