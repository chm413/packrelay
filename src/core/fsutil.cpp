#include "mcpsync/fsutil.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <ctime>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace mcpsync::fsu {

namespace {

std::string winErr() {
#ifdef _WIN32
    return "win32 error " + std::to_string(static_cast<unsigned long>(GetLastError()));
#else
    return std::string("os error");
#endif
}

} // namespace

fs::path fromUtf8(std::string_view s) {
#ifdef _WIN32
    if (s.empty()) return fs::path();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return fs::path();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return fs::path(w);
#else
    return fs::path(std::string(s));
#endif
}

std::string toUtf8(fs::path const& p) {
#ifdef _WIN32
    std::wstring w = p.wstring();
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

bool exists(fs::path const& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool isDirectory(fs::path const& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

Status ensureDir(fs::path const& p) {
    if (p.empty()) return {};
    std::error_code ec;
    if (fs::exists(p, ec)) {
        if (fs::is_directory(p, ec)) return {};
        return Error{ErrCode::Io, "path exists but is not a directory: " + toUtf8(p)};
    }
    fs::create_directories(p, ec);
    if (ec) return Error{ErrCode::Io, "create_directories failed: " + toUtf8(p), ec.message()};
    return {};
}

Status readFile(fs::path const& p, std::string& out) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return Error{ErrCode::NotFound, "file not found: " + toUtf8(p)};
    std::ifstream f(p, std::ios::binary);
    if (!f) return Error{ErrCode::Io, "open failed: " + toUtf8(p)};
    f.seekg(0, std::ios::end);
    auto len = f.tellg();
    if (len < 0) return Error{ErrCode::Io, "tellg failed: " + toUtf8(p)};
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(len));
    if (len > 0) f.read(out.data(), len);
    if (!f && !f.eof()) return Error{ErrCode::Io, "read failed: " + toUtf8(p)};
    return {};
}

Status writeFile(fs::path const& p, std::string_view data) {
    auto dir = p.parent_path();
    if (!dir.empty()) {
        auto st = ensureDir(dir);
        if (!st) return st;
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return Error{ErrCode::Io, "open for write failed: " + toUtf8(p)};
    if (!data.empty()) f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) return Error{ErrCode::Io, "write failed: " + toUtf8(p)};
    return {};
}

Status writeFileAtomic(fs::path const& p, std::string_view data) {
    auto dir = p.parent_path();
    if (!dir.empty()) {
        auto st = ensureDir(dir);
        if (!st) return st;
    }
    auto tmp = p;
    tmp += L".tmp";
    auto st = writeFile(tmp, data);
    if (!st) return st;
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return Error{ErrCode::Io, "MoveFileEx failed: " + toUtf8(p), winErr()};
    }
#else
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) return Error{ErrCode::Io, "rename failed: " + toUtf8(p), ec.message()};
#endif
    return {};
}

uint64_t fileSize(fs::path const& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}

uint64_t fileWriteTime(fs::path const& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<uint64_t>(t.time_since_epoch().count());
}

std::string relativeUtf8(fs::path const& base, fs::path const& target) {
    std::error_code ec;
    auto rel = fs::relative(target, base, ec);
    if (ec) rel = target.lexically_relative(base);
    std::string s = toUtf8(rel);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string nowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

uint64_t nowUnix() {
    return static_cast<uint64_t>(std::time(nullptr));
}

namespace {

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// 递归匹配：'*' 不跨 '/'，'**' 跨目录，'?' 匹配单字符（大小写不敏感）
bool globMatch(std::string_view pat, size_t pi, std::string_view text, size_t ti) {
    while (ti < text.size()) {
        if (pi >= pat.size()) return false;
        char pc = lowerAscii(pat[pi]);

        if (pc == '*') {
            bool isDstar = (pi + 1 < pat.size() && pat[pi + 1] == '*');
            size_t next  = pi + (isDstar ? 2 : 1);
            if (isDstar && next < pat.size() && pat[next] == '/') {
                // "**/" 可匹配零级目录
                if (globMatch(pat, next + 1, text, ti)) return true;
                for (size_t k = ti; k < text.size(); ++k) {
                    if (text[k] == '/') {
                        if (globMatch(pat, next + 1, text, k + 1)) return true;
                    }
                }
                return false;
            }
            for (size_t k = ti;; ++k) {
                if (globMatch(pat, next, text, k)) return true;
                if (k >= text.size() || text[k] == '/') break;
            }
            return false;
        }

        if (pc == '?' || pc == lowerAscii(text[ti])) {
            ++pi;
            ++ti;
            continue;
        }
        return false;
    }

    // 文本耗尽：允许剩余模式为纯 '*' / '**'
    while (pi < pat.size()) {
        if (pat[pi] != '*') return false;
        ++pi;
        if (pi < pat.size() && pat[pi] == '*' && pi >= 1 && pat[pi - 1] == '*') ++pi;
        if (pi < pat.size() && pat[pi] == '/' && pi + 1 == pat.size()) ++pi;
    }
    return pi == pat.size();
}

} // namespace

bool matchGlob(std::string_view pattern, std::string_view text) {
    return globMatch(pattern, 0, text, 0);
}

Status walkFiles(fs::path const& root, std::vector<WalkEntry>& out, std::function<bool(std::string const&)> const& filter) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return Error{ErrCode::NotFound, "not a directory: " + toUtf8(root)};
    }
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return Error{ErrCode::Io, "iterator failed: " + toUtf8(root), ec.message()};

    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        auto const&     e = *it;
        if (e.is_symlink(fec)) {
            if (e.is_directory(fec)) it.disable_recursion_pending();
            continue;
        }
        auto rel = relativeUtf8(root, e.path());
        if (e.is_directory(fec)) {
            // 目录级剪枝：.git 与状态目录永不进入
            if (rel == ".git" || rel.rfind(".git/", 0) == 0 || rel == ".mcpack-sync" ||
                rel.rfind(".mcpack-sync/", 0) == 0) {
                it.disable_recursion_pending();
                continue;
            }
            if (filter && !filter(rel + "/")) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!e.is_regular_file(fec)) continue;
        if (filter && !filter(rel)) continue;
        WalkEntry we;
        we.abs   = e.path();
        we.rel   = rel;
        we.size  = static_cast<uint64_t>(e.file_size(fec));
        we.mtime = fileWriteTime(e.path());
        out.push_back(std::move(we));
    }
    std::sort(out.begin(), out.end(), [](WalkEntry const& a, WalkEntry const& b) { return a.rel < b.rel; });
    return {};
}

Status copyTree(fs::path const& src, fs::path const& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (auto const& e : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec)) {
        auto rel     = e.path().lexically_relative(src);
        auto target  = dst / rel;
        std::error_code fec;
        if (e.is_directory(fec)) {
            fs::create_directories(target, fec);
        } else if (e.is_regular_file(fec)) {
            fs::create_directories(target.parent_path(), fec);
            fs::copy_file(e.path(), target, fs::copy_options::overwrite_existing, fec);
            if (fec) return Error{ErrCode::Io, "copy failed: " + toUtf8(e.path()), fec.message()};
        }
    }
    return {};
}

Status mirrorTree(fs::path const& src, fs::path const& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return Error{ErrCode::Io, "create dir failed: " + toUtf8(dst), ec.message()};

    // 1) 删除 dst 中不在 src 的条目
    std::vector<fs::path> stale;
    for (auto const& e : fs::recursive_directory_iterator(dst, fs::directory_options::skip_permission_denied, ec)) {
        if (e.is_directory()) continue;
        auto rel = e.path().lexically_relative(dst);
        std::error_code fec;
        if (!fs::exists(src / rel, fec)) stale.push_back(e.path());
    }
    for (auto const& p : stale) {
        std::error_code fec;
        fs::remove(p, fec);
    }

    // 2) 复制新增/变更
    for (auto const& e : fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        if (e.is_directory(fec)) continue;
        if (!e.is_regular_file(fec)) continue;
        auto rel    = e.path().lexically_relative(src);
        auto target = dst / rel;
        bool need   = true;
        if (fs::exists(target, fec) && fs::is_regular_file(target, fec)) {
            need = fs::file_size(target, fec) != fs::file_size(e.path(), fec) ||
                   fs::last_write_time(target, fec) != fs::last_write_time(e.path(), fec);
        }
        if (need) {
            fs::create_directories(target.parent_path(), fec);
            fs::copy_file(e.path(), target, fs::copy_options::overwrite_existing, fec);
            if (fec) return Error{ErrCode::Io, "copy failed: " + toUtf8(e.path()), fec.message()};
        }
    }
    return {};
}

Status removeAll(fs::path const& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return {};
    fs::remove_all(p, ec);
    if (ec) return Error{ErrCode::Io, "remove_all failed: " + toUtf8(p), ec.message()};
    return {};
}

std::string sanitizeName(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string slugify(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool lastDash = false;
    for (char c : s) {
        char lc = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        bool alnum = (lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9');
        if (alnum) {
            out.push_back(lc);
            lastDash = false;
        } else if (lc == '.' || lc == '_' || lc == '-') {
            out.push_back(lc);
            lastDash = false;
        } else if (!lastDash && !out.empty()) {
            out.push_back('-');
            lastDash = true;
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "pack";
    return out;
}

} // namespace mcpsync::fsu
