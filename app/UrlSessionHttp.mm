#import <Foundation/Foundation.h>

#include "UrlSessionHttp.h"

namespace NHoshi {

namespace {

NSURLSession* Session() {
    static NSURLSession* session = [] {
        // Ephemeral and without a URL cache: avatars have their own cache in the app
        NSURLSessionConfiguration* config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        // Long enough for a /sync long poll (the client asks for at most 30 s) plus the network
        config.timeoutIntervalForRequest = 90;
        config.HTTPCookieStorage = nil;
        config.URLCache = nil;
        return [NSURLSession sessionWithConfiguration:config];
    }();
    return session;
}

} // namespace

THttpResponse TUrlSessionHttp::Send(const THttpRequest& request) {
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request.Url.c_str()]];
    if (!url) {
        return {0, {}};
    }
    NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
    urlRequest.HTTPMethod = [NSString stringWithUTF8String:request.Method.c_str()];
    [urlRequest setValue:@"application/json" forHTTPHeaderField:@"Accept"];
    if (!request.AccessToken.empty()) {
        NSString* bearer = [@"Bearer " stringByAppendingString:[NSString stringWithUTF8String:request.AccessToken.c_str()]];
        [urlRequest setValue:bearer forHTTPHeaderField:@"Authorization"];
    }
    if (!request.Body.empty() || request.Method != "GET") {
        [urlRequest setValue:[NSString stringWithUTF8String:request.ContentType.c_str()] forHTTPHeaderField:@"Content-Type"];
        urlRequest.HTTPBody = [NSData dataWithBytes:request.Body.data() length:request.Body.size()];
    }

    __block THttpResponse response;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionDataTask* task = [Session() dataTaskWithRequest:urlRequest
        completionHandler:^(NSData* data, NSURLResponse* urlResponse, NSError* error) {
            if (!error && [urlResponse isKindOfClass:[NSHTTPURLResponse class]]) {
                response.Status = static_cast<int>(((NSHTTPURLResponse*)urlResponse).statusCode);
                if (data) {
                    response.Body.assign(static_cast<const char*>(data.bytes), data.length);
                }
            }
            dispatch_semaphore_signal(done);
        }];
    [task resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return response;
}

} // namespace NHoshi
