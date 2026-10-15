#pragma once

#include "client.h"

#include <string>

namespace NHoshi {

// HTTP/1.1 over a Unix socket, one connection per request. For the local test Synapse: the Claude sandbox
// allows this socket but not TCP. The URL host is ignored; only the path and query are sent.
class TUnixHttp : public IHttp {
public:
    // A relative path is resolved against the current directory: macOS limits socket paths to 104 bytes
    explicit TUnixHttp(std::string socketPath);

    THttpResponse Send(const THttpRequest& request) override;

private:
    std::string SocketPath;
};

// Parses a complete HTTP/1.1 response read until the server closed the connection.
// Handles Content-Length and chunked bodies. Exposed for tests.
THttpResponse ParseHttpResponse(const std::string& raw);

} // namespace NHoshi
