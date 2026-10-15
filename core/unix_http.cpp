#include "unix_http.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace NHoshi {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Returns the path and query of an absolute URL, or the URL itself if it is already a path
std::string RequestTarget(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const auto slash = url.find('/', scheme + 3);
    return slash == std::string::npos ? "/" : url.substr(slash);
}

std::string DecodeChunked(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        const auto lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string::npos) {
            break;
        }
        const size_t size = std::strtoul(body.substr(pos, lineEnd - pos).c_str(), nullptr, 16);
        pos = lineEnd + 2;
        if (size == 0 || pos + size > body.size()) {
            break;
        }
        out.append(body, pos, size);
        pos += size + 2;
    }
    return out;
}

// Closes the descriptor on every exit path
struct TFd {
    int Fd;
    ~TFd() {
        if (Fd >= 0) {
            ::close(Fd);
        }
    }
};

} // namespace

THttpResponse ParseHttpResponse(const std::string& raw) {
    const auto headerEnd = raw.find("\r\n\r\n");
    if (raw.compare(0, 5, "HTTP/") != 0 || headerEnd == std::string::npos) {
        return {0, {}};
    }
    const auto space = raw.find(' ');
    THttpResponse response;
    response.Status = std::atoi(raw.c_str() + space + 1);

    bool chunked = false;
    long contentLength = -1;
    size_t lineStart = raw.find("\r\n") + 2;
    while (lineStart < headerEnd) {
        const auto lineEnd = raw.find("\r\n", lineStart);
        const std::string line = raw.substr(lineStart, lineEnd - lineStart);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string name = Lower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            if (name == "transfer-encoding" && Lower(value).find("chunked") != std::string::npos) {
                chunked = true;
            } else if (name == "content-length") {
                contentLength = std::atol(value.c_str());
            }
        }
        lineStart = lineEnd + 2;
    }

    std::string body = raw.substr(headerEnd + 4);
    if (chunked) {
        body = DecodeChunked(body);
    } else if (contentLength >= 0 && static_cast<size_t>(contentLength) < body.size()) {
        body.resize(contentLength);
    }
    response.Body = std::move(body);
    return response;
}

TUnixHttp::TUnixHttp(std::string socketPath)
    : SocketPath(std::move(socketPath))
{}

THttpResponse TUnixHttp::Send(const THttpRequest& request) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (SocketPath.size() >= sizeof(address.sun_path)) {
        return {0, {}};
    }
    std::memcpy(address.sun_path, SocketPath.c_str(), SocketPath.size() + 1);

    TFd fd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (fd.Fd < 0 || ::connect(fd.Fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return {0, {}};
    }

    std::string out = request.Method + " " + RequestTarget(request.Url) + " HTTP/1.1\r\n"
        "Host: localhost\r\nConnection: close\r\nAccept: application/json\r\n"
        // A socket peer has no IP; Synapse behind a Unix socket expects a proxy to name the client
        "X-Forwarded-For: 127.0.0.1\r\n";
    if (!request.AccessToken.empty()) {
        out += "Authorization: Bearer " + request.AccessToken + "\r\n";
    }
    if (!request.Body.empty() || request.Method != "GET") {
        out += "Content-Type: application/json\r\nContent-Length: " + std::to_string(request.Body.size()) + "\r\n";
    }
    out += "\r\n" + request.Body;

    for (size_t sent = 0; sent < out.size();) {
        const ssize_t n = ::write(fd.Fd, out.data() + sent, out.size() - sent);
        if (n <= 0) {
            return {0, {}};
        }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[16384];
    while (true) {
        const ssize_t n = ::read(fd.Fd, buf, sizeof(buf));
        if (n < 0) {
            return {0, {}};
        }
        if (n == 0) {
            break;
        }
        raw.append(buf, static_cast<size_t>(n));
    }
    return ParseHttpResponse(raw);
}

} // namespace NHoshi
