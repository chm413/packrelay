#include "mcpsync/model.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/result.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mcpsync {

const char* toString(ErrCode c) noexcept {
    switch (c) {
    case ErrCode::Ok: return "ok";
    case ErrCode::Config: return "config";
    case ErrCode::NotFound: return "not_found";
    case ErrCode::Io: return "io";
    case ErrCode::Parse: return "parse";
    case ErrCode::Scan: return "scan";
    case ErrCode::Process: return "process";
    case ErrCode::Git: return "git";
    case ErrCode::Network: return "network";
    case ErrCode::Http: return "http";
    case ErrCode::Release: return "release";
    case ErrCode::Conflict: return "conflict";
    case ErrCode::Internal: return "internal";
    }
    return "unknown";
}

std::string Error::str() const {
    std::string s = "[";
    s += toString(code);
    s += "] ";
    s += message;
    if (!detail.empty()) {
        s += " | ";
        if (detail.size() > 800) {
            s += detail.substr(0, 800);
            s += "...(truncated)";
        } else {
            s += detail;
        }
    }
    if (retryable) s += " (retryable)";
    return s;
}

const char* toString(ChangeKind k) noexcept {
    switch (k) {
    case ChangeKind::Added: return "added";
    case ChangeKind::Updated: return "updated";
    case ChangeKind::Removed: return "removed";
    case ChangeKind::Unchanged: return "unchanged";
    }
    return "unknown";
}

std::string versionToString(Version const& v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d.%d.%d", v[0], v[1], v[2]);
    return buf;
}

bool parseVersion(std::string_view s, Version& out) {
    int  parts[3] = {0, 0, 0};
    int  idx      = 0;
    int  acc      = 0;
    bool hasDigit = false;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            acc      = acc * 10 + (c - '0');
            hasDigit = true;
        } else if (c == '.') {
            if (!hasDigit) return false;
            if (idx > 2) return false;
            parts[idx++] = acc;
            acc          = 0;
            hasDigit     = false;
        } else if (c == '-' || c == '+') {
            break; // 忽略预发布后缀
        } else {
            return false;
        }
    }
    if (idx == 0 && !hasDigit) return false;
    out = {parts[0], parts[1], parts[2]};
    return true;
}

int compareVersion(Version const& a, Version const& b) {
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

std::string PackSnapshot::slug() const {
    std::string base = id.name.empty() ? id.uuid : id.name;
    // name 若是 i18n key（含 '.'），退回 uuid
    bool isKey = base.find('.') != std::string::npos && base.find(' ') == std::string::npos;
    if (isKey) base = id.uuid;
    auto s = fsu::slugify(base);
    if (s.size() > 48) s = s.substr(0, 48);
    return s;
}

} // namespace mcpsync
