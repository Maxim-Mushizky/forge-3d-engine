#include "HttpClient.h"

// windows.h must precede winhttp.h — winhttp.h does not self-include it and
// fails on bare LPVOID otherwise.
#include <windows.h>
#include <winhttp.h>

namespace forge {

namespace {

// Owns one WinHTTP handle for the duration of a scope (RAII per CLAUDE.md —
// the raw HINTERNET never escapes this file).
class HInternet {
public:
    explicit HInternet(HINTERNET handle) : m_Handle(handle) {}
    ~HInternet()
    {
        if (m_Handle)
            WinHttpCloseHandle(m_Handle);
    }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;

    HINTERNET Get() const { return m_Handle; }
    explicit operator bool() const { return m_Handle != nullptr; }

private:
    HINTERNET m_Handle = nullptr;
};

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string StageError(const char* stage)
{
    return std::string(stage) + " failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

} // namespace

HttpResult HttpsGet(const std::string& host, const std::string& path,
                    const std::string& userAgent, uint64_t maxBytes)
{
    HttpResult r;

    // DEFAULT_PROXY, not AUTOMATIC_PROXY_CONFIG: the latter is missing from
    // MinGW-w64's winhttp.h, and the machine-wide default settings are enough
    // for a public CDN.
    HInternet session(WinHttpOpen(Widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        r.error = StageError("WinHttpOpen");
        return r;
    }
    // Defaults are tight for CDN-sized bodies; a multi-MB texture over a slow
    // link trips the 30 s receive timeout (error 12002). resolve/connect/send/
    // receive, milliseconds.
    WinHttpSetTimeouts(session.Get(), 15000, 30000, 30000, 120000);
    HInternet connection(WinHttpConnect(session.Get(), Widen(host).c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        r.error = StageError("WinHttpConnect");
        return r;
    }
    HInternet request(WinHttpOpenRequest(connection.Get(), L"GET", Widen(path).c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        r.error = StageError("WinHttpOpenRequest");
        return r;
    }
    if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        r.error = StageError("WinHttpSendRequest");
        return r;
    }
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
        r.error = StageError("WinHttpReceiveResponse");
        return r;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        r.error = StageError("WinHttpQueryHeaders");
        return r;
    }
    r.status = (int)status;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &available)) {
            r.error = StageError("WinHttpQueryDataAvailable");
            return r;
        }
        if (available == 0)
            break;
        if (r.body.size() + available > maxBytes) {
            r.error = "response body exceeds the " + std::to_string(maxBytes) + "-byte limit";
            return r;
        }
        const size_t offset = r.body.size();
        r.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.Get(), r.body.data() + offset, available, &read)) {
            r.error = StageError("WinHttpReadData");
            return r;
        }
        r.body.resize(offset + read);
    }
    return r;
}

} // namespace forge
