#include "mcpsync/http.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winhttp.h>
#endif

namespace mcpsync {

namespace {

#ifdef _WIN32
std::wstring w(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string a(std::wstring const& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

struct UrlParts {
    std::wstring scheme;
    std::wstring host;
    uint16_t     port{0};
    std::wstring path;
};

bool splitUrl(std::string const& url, UrlParts& out) {
    std::wstring u = w(url);
    auto         schemeEnd = u.find(L"://");
    if (schemeEnd == std::wstring::npos) return false;
    out.scheme = u.substr(0, schemeEnd);
    std::wstring rest = u.substr(schemeEnd + 3);

    auto pathStart = rest.find(L'/');
    std::wstring authority = pathStart == std::wstring::npos ? rest : rest.substr(0, pathStart);
    out.path = pathStart == std::wstring::npos ? L"/" : rest.substr(pathStart);

    auto colon = authority.find(L':');
    if (colon == std::wstring::npos) {
        out.host = authority;
        out.port = (out.scheme == L"https") ? 443 : 80;
    } else {
        out.host = authority.substr(0, colon);
        out.port = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1)));
    }
    return !out.host.empty();
}

struct HandleGuard {
    HINTERNET h{nullptr};
    ~HandleGuard() {
        if (h) WinHttpCloseHandle(h);
    }
};
#endif

} // namespace

std::string HttpResponse::header(std::string const& name) const {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto it = headers.find(lower);
    return it == headers.end() ? std::string() : it->second;
}

Result<HttpResponse> httpRequest(HttpRequest const& req) {
#ifdef _WIN32
    UrlParts parts;
    if (!splitUrl(req.url, parts)) {
        return Error{ErrCode::Network, "invalid url: " + req.url};
    }

    HandleGuard session;
    session.h = WinHttpOpen(
        L"mcpack-sync/1.0",
        req.proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        req.proxy.empty() ? WINHTTP_NO_PROXY_NAME : w(req.proxy).c_str(),
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!session.h) {
        // 旧系统不支持 AUTOMATIC_PROXY，回退到默认代理
        session.h = WinHttpOpen(L"mcpack-sync/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) {
        return Error{ErrCode::Network, "WinHttpOpen failed"};
    }

    int timeout = req.timeoutMs > 0 ? req.timeoutMs : 30000;
    WinHttpSetTimeouts(session.h, timeout, timeout, timeout, timeout);

    HandleGuard conn;
    conn.h = WinHttpConnect(session.h, parts.host.c_str(), parts.port, 0);
    if (!conn.h) {
        return Error{ErrCode::Network, "WinHttpConnect failed for " + req.url + " (err " + std::to_string(GetLastError()) + ")"};
    }

    std::wstring verb = w(req.method.empty() ? "GET" : req.method);
    HandleGuard  request;
    request.h = WinHttpOpenRequest(conn.h, verb.c_str(), parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   parts.scheme == L"https" ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) {
        return Error{ErrCode::Network, "WinHttpOpenRequest failed"};
    }

    if (req.insecure) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        WinHttpSetOption(request.h, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
    }

    std::wstring headerBlock;
    bool         hasUa = false, hasAccept = false;
    for (auto const& kv : req.headers) {
        headerBlock += w(kv.first);
        headerBlock += L": ";
        headerBlock += w(kv.second);
        headerBlock += L"\r\n";
        std::string lk;
        for (char c : kv.first) lk.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lk == "user-agent") hasUa = true;
        if (lk == "accept") hasAccept = true;
    }
    if (!hasUa) headerBlock += L"User-Agent: mcpack-sync/1.0\r\n";
    if (!hasAccept) headerBlock += L"Accept: application/vnd.github+json\r\n";
    headerBlock += L"X-GitHub-Api-Version: 2022-11-28\r\n";

    BOOL sent = WinHttpSendRequest(
        request.h,
        headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
        headerBlock.empty() ? 0 : static_cast<DWORD>(-1L),
        req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
        static_cast<DWORD>(req.body.size()),
        static_cast<DWORD>(req.body.size()),
        0
    );
    if (!sent) {
        return Error{ErrCode::Network, "WinHttpSendRequest failed: " + req.url,
                     "err " + std::to_string(GetLastError()), true};
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        return Error{ErrCode::Network, "WinHttpReceiveResponse failed: " + req.url,
                     "err " + std::to_string(GetLastError()), true};
    }

    HttpResponse res;
    {
        DWORD code    = 0;
        DWORD codeLen = sizeof code;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeLen, WINHTTP_NO_HEADER_INDEX);
        res.status = static_cast<int>(code);
    }
    {
        DWORD len = 0;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (len > 0) {
            std::wstring raw(len / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    raw.data(), &len, WINHTTP_NO_HEADER_INDEX)) {
                std::string text = a(raw);
                size_t      pos  = 0;
                while (pos < text.size()) {
                    auto eol = text.find("\r\n", pos);
                    if (eol == std::string::npos) break;
                    std::string line = text.substr(pos, eol - pos);
                    pos              = eol + 2;
                    auto colon       = line.find(':');
                    if (colon == std::string::npos) continue;
                    std::string k = line.substr(0, colon);
                    std::string v = line.substr(colon + 1);
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    res.headers[k] = v;
                }
            }
        }
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) break;
        if (avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD       read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), avail, &read)) break;
        chunk.resize(read);
        res.body += chunk;
        if (res.body.size() > (64u << 20)) break; // 防御超大响应
    }
    return res;
#else
    (void)req;
    return Error{ErrCode::Network, "httpRequest not implemented on this platform"};
#endif
}

Result<HttpResponse> httpGetJson(
    std::string const&                        url,
    std::map<std::string, std::string> const& headers,
    int                                       timeoutMs,
    std::string const&                        proxy,
    int                                       maxRetries
) {
    std::string lastErr;
    for (int attempt = 0; attempt <= maxRetries; ++attempt) {
        HttpRequest req;
        req.method    = "GET";
        req.url       = url;
        req.headers   = headers;
        req.timeoutMs = timeoutMs;
        req.proxy     = proxy;

        auto r = httpRequest(req);
        if (r) {
            auto& resp = r.value();
            if (resp.status >= 200 && resp.status < 300) return resp;
            if (resp.status == 404) {
                return Error{ErrCode::Http, "404 Not Found: " + url, resp.body.substr(0, 400)};
            }
            lastErr = "HTTP " + std::to_string(resp.status) + ": " + url + " " + resp.body.substr(0, 300);
            if (resp.status < 500) {
                return Error{ErrCode::Http, lastErr};
            }
        } else {
            lastErr = r.error().str();
        }
        if (attempt < maxRetries) {
            int delay = 500 * (1 << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    return Error{ErrCode::Network, "GET failed after retries: " + url, lastErr, true};
}

std::string urlEncode(std::string_view s) {
    static char const* hex = "0123456789ABCDEF";
    std::string        out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string base64Encode(std::string_view s) {
    static char const* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    out.reserve(((s.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < s.size()) {
        uint32_t v = (static_cast<uint8_t>(s[i]) << 16) | (static_cast<uint8_t>(s[i + 1]) << 8) |
                     static_cast<uint8_t>(s[i + 2]);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
        out.push_back(tbl[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == s.size()) {
        uint32_t v = static_cast<uint8_t>(s[i]) << 16;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out += "==";
    } else if (i + 2 == s.size()) {
        uint32_t v = (static_cast<uint8_t>(s[i]) << 16) | (static_cast<uint8_t>(s[i + 1]) << 8);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

} // namespace mcpsync
