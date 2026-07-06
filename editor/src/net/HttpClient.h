#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace forge {

// One HTTPS GET outcome. `error` empty = the transport delivered a response
// (status may still be non-200); non-empty = nothing came back at all
// (offline, DNS failure, TLS failure) and status/body are meaningless.
struct HttpResult {
    int status = 0;
    std::vector<uint8_t> body;
    std::string error;

    bool Ok() const { return error.empty() && status == 200; }
};

// Blocking HTTPS GET via WinHTTP — schannel TLS, system cert store and proxy,
// zero third-party dependencies on MinGW (#84). Safe to call from a worker
// thread; it touches no GL or editor state. `maxBytes` bounds the response
// body so a misbehaving server can't exhaust memory.
HttpResult HttpsGet(const std::string& host, const std::string& path,
                    const std::string& userAgent, uint64_t maxBytes = 512ull * 1024 * 1024);

} // namespace forge
