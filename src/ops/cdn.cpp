#include "mcpsync/cdn.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"

#include <algorithm>
#include <cstring>

#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#endif

namespace mcpsync {

namespace {

#ifdef _WIN32
std::atomic<int> g_wsaRefs{0};

Status ensureWsa() {
    if (g_wsaRefs.fetch_add(1) == 0) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            g_wsaRefs.fetch_sub(1);
            return Error{ErrCode::Network, "WSAStartup failed"};
        }
    }
    return {};
}

void releaseWsa() {
    if (g_wsaRefs.fetch_sub(1) == 1) WSACleanup();
}
#endif

json::Value targetToJson(CdnTarget const& t) {
    json::Value o = json::Value::object();
    o.set("uuid", json::Value(t.uuid));
    o.set("name", json::Value(t.name));
    o.set("slug", json::Value(t.slug));
    o.set("version", json::Value(t.version));
    o.set("tag", json::Value(t.tag));
    o.set("assetName", json::Value(t.assetName));
    o.set("assetUrl", json::Value(t.assetUrl));
    o.set("publicUrl", json::Value(t.publicUrl));
    o.set("mirror", json::Value(t.mirror));
    o.set("sizeBytes", json::Value(static_cast<uint64_t>(t.sizeBytes)));
    o.set("updatedAt", json::Value(t.updatedAt));
    json::Array cands;
    for (auto const& c : t.candidates) {
        json::Value co = json::Value::object();
        co.set("name", json::Value(c.name));
        co.set("priority", json::Value(c.priority));
        co.set("url", json::Value(c.url));
        cands.push_back(std::move(co));
    }
    o.set("candidates", json::Value(std::move(cands)));
    return o;
}

} // namespace

std::vector<PackCdnEntry> buildPackCdnEntries(std::vector<CdnTarget> const& targets) {
    std::vector<PackCdnEntry> out;
    out.reserve(targets.size());
    for (auto const& t : targets) {
        PackCdnEntry e;
        e.uuid      = t.uuid;
        e.version   = t.version;
        e.directUrl = t.assetUrl;
        e.url       = t.publicUrl.empty() ? t.assetUrl : t.publicUrl;
        if (e.uuid.empty() || e.url.empty()) continue;
        // BDS 的资源包标识是 <uuid>_<version>；拿不到版本时退回裸 uuid
        e.packId = e.version.empty() ? e.uuid : (e.uuid + "_" + e.version);
        out.push_back(std::move(e));
    }
    return out;
}

Status parsePackCdnEntries(std::string const& manifestJson, std::vector<PackCdnEntry>& out) {
    json::Value root;
    std::string err;
    if (!json::parse(manifestJson, root, err)) {
        return Error{ErrCode::Parse, "cdn manifest is not valid json", err};
    }
    out.clear();

    // 首选 packCdnUrls：这是 writeManifest() 专门为「写 BDS 内存」准备的扁平段，
    // 字段已归一（packId/uuid/version/url/directUrl），不存在格式歧义。
    auto const& flat = root.at("packCdnUrls");
    if (flat.isArray()) {
        for (auto const& e : flat.asArray()) {
            if (!e.isObject()) continue;
            PackCdnEntry pe;
            pe.packId    = e.at("packId").asString();
            pe.uuid      = e.at("uuid").asString();
            pe.version   = e.at("version").asString();
            pe.url       = e.at("url").asString();
            pe.directUrl = e.at("directUrl").asString();
            if (pe.url.empty()) pe.url = pe.directUrl;
            if (pe.packId.empty() && !pe.uuid.empty()) {
                pe.packId = pe.version.empty() ? pe.uuid : (pe.uuid + "_" + pe.version);
            }
            if (pe.packId.empty() || pe.url.empty()) continue;
            if (pe.uuid.empty()) pe.uuid = pe.packId;
            out.push_back(std::move(pe));
        }
        if (!out.empty()) return {};
    }

    // 兼容旧清单：从 packs[] 里取 uuid/version/assetUrl/publicUrl
    for (auto const& e : root.at("packs").asArray()) {
        if (!e.isObject()) continue;
        PackCdnEntry pe;
        pe.uuid      = e.at("uuid").asString();
        pe.version   = e.at("version").asString();
        pe.directUrl = e.at("assetUrl").asString();
        pe.url       = e.at("publicUrl").asString();
        if (pe.url.empty()) pe.url = pe.directUrl;
        if (pe.url.empty()) pe.url = e.at("url").asString();
        if (pe.uuid.empty() || pe.url.empty()) continue;
        pe.packId = pe.version.empty() ? pe.uuid : (pe.uuid + "_" + pe.version);
        out.push_back(std::move(pe));
    }
    if (out.empty()) {
        return Error{ErrCode::Parse, "cdn manifest contains no usable pack entry"};
    }
    return {};
}

void upsertPackCdnUrls(
    std::vector<std::pair<std::string, std::string>>& urls,
    std::vector<PackCdnEntry> const&                  entries
) {
    if (entries.empty()) return;

    // 归属判定要覆盖「同一 uuid 的历史版本主键」，否则版本升级后旧键会残留，
    // BDS 仍可能按旧键取到过期外链。
    auto owned = [&entries](std::string const& key) {
        for (auto const& e : entries) {
            if (key == e.packId) return true;
            if (e.uuid.empty()) continue;
            if (key == e.uuid) return true;
            if (key.size() > e.uuid.size() && key.compare(0, e.uuid.size(), e.uuid) == 0
                && key[e.uuid.size()] == '_') {
                return true; // <uuid>_<任意版本>
            }
        }
        return false;
    };

    urls.erase(
        std::remove_if(urls.begin(), urls.end(), [&owned](std::pair<std::string, std::string> const& kv) {
            return owned(kv.first);
        }),
        urls.end()
    );

    for (auto const& e : entries) {
        urls.emplace_back(e.packId, e.url);
        if (!e.uuid.empty() && e.uuid != e.packId) urls.emplace_back(e.uuid, e.url);
    }
}

CdnManager::CdnManager(CdnConfig cfg, std::filesystem::path baseDir)
: mCfg(std::move(cfg)), mBaseDir(std::move(baseDir)) {}
CdnManager::~CdnManager() {
    stopRedirectServer();
}

Status CdnManager::init() {
    if (!mCfg.enabled) return {};
    auto st = fsu::ensureDir(mBaseDir / ".mcpack-sync");
    if (!st) return st;
    loadPersisted();
    if (mCfg.redirect.enabled) {
        auto r = startRedirectServer();
        if (!r) {
            logger().warn("cdn: redirect server not started: " + r.error().str());
        }
    }
    return {};
}

std::string CdnManager::stableUrl() const {
    if (!mCfg.redirect.enabled) return mRedirectTarget;
    std::string base = mCfg.publicBaseUrl;
    if (base.empty()) {
        base = "http://127.0.0.1:" + std::to_string(mCfg.redirect.port);
    }
    std::string p = mCfg.redirect.path;
    if (p.empty() || p[0] != '/') p = "/" + p;
    return base + p;
}

void CdnManager::setRedirectTarget(std::string const& url) {
    std::lock_guard<std::mutex> lk(mMtx);
    if (mRedirectTarget != url) {
        logger().info("cdn: redirect target -> " + url);
    }
    mRedirectTarget = url;
}

std::string CdnManager::redirectTarget() const {
    std::lock_guard<std::mutex> lk(mMtx);
    return mRedirectTarget;
}

Status CdnManager::apply(CdnTarget const& target, std::string const& owner, std::string const& repo) {
    {
        std::lock_guard<std::mutex> lk(mMtx);
        mOwner = owner;
        mRepo  = repo;
        bool replaced = false;
        for (auto& t : mTargets) {
            if (t.uuid == target.uuid) {
                t        = target;
                replaced = true;
                break;
            }
        }
        if (!replaced) mTargets.push_back(target);
        mRedirectTarget = target.publicUrl;
    }

    Status firstErr;
    if (mCfg.writeConfigFile) {
        auto st = writeManifest();
        if (!st) firstErr = st;
    }
    if (mCfg.serverProperties.enabled) {
        auto st = patchServerProperties();
        if (!st && firstErr) firstErr = st;
    }
    if (mCfg.bdsCdnConfig.enabled) {
        auto st = writeBdsCdnConfig();
        if (!st) {
            logger().warn("cdn: write " + mCfg.bdsCdnConfig.file + " failed: " + st.error().str());
            if (!firstErr) firstErr = st;
        }
    }
    if (mCfg.redirect.enabled && !redirectRunning()) {
        auto st = startRedirectServer();
        if (!st) logger().warn("cdn: redirect server not running: " + st.error().str());
    }
    logger().info("cdn: published " + target.slug + " -> " + target.publicUrl);
    return firstErr;
}

void CdnManager::loadPersisted() {
    auto path = mBaseDir / fsu::fromUtf8(mCfg.manifestFile);
    std::string text;
    auto        st = fsu::readFile(path, text);
    if (!st) return;

    json::Value root;
    std::string err;
    if (!json::parse(text, root, err)) {
        logger().warn("cdn: manifest unreadable, ignore: " + err);
        return;
    }
    std::lock_guard<std::mutex> lk(mMtx);
    mOwner = root.at("owner").asString();
    mRepo  = root.at("repo").asString();

    std::string newestUrl;
    std::string newestAt;
    for (auto const& e : root.at("packs").asArray()) {
        if (!e.isObject()) continue;
        CdnTarget t;
        t.uuid      = e.at("uuid").asString();
        t.name      = e.at("name").asString();
        t.slug      = e.at("slug").asString();
        t.version   = e.at("version").asString();
        t.tag       = e.at("tag").asString();
        t.assetName = e.at("assetName").asString();
        t.assetUrl  = e.at("assetUrl").asString();
        t.publicUrl = e.at("publicUrl").asString();
        t.mirror    = e.at("mirror").asString();
        t.sizeBytes = static_cast<uint64_t>(e.at("sizeBytes").asInt(0));
        t.updatedAt = e.at("updatedAt").asString();
        for (auto const& c : e.at("candidates").asArray()) {
            MirrorCandidate mc;
            mc.name     = c.at("name").asString();
            mc.priority = static_cast<int>(c.at("priority").asInt(0));
            mc.url      = c.at("url").asString();
            t.candidates.push_back(std::move(mc));
        }
        if (t.updatedAt >= newestAt && !t.publicUrl.empty()) {
            newestAt  = t.updatedAt;
            newestUrl = t.publicUrl;
        }
        mTargets.push_back(std::move(t));
    }
    if (!newestUrl.empty()) {
        mRedirectTarget = newestUrl;
        logger().info("cdn: restored " + std::to_string(mTargets.size()) + " target(s), redirect -> " + newestUrl);
    }
}

Status CdnManager::writeManifest() const {
    json::Value root = json::Value::object();
    root.set("generatedBy", json::Value("mcpack-sync"));
    root.set("generatedAt", json::Value(fsu::nowIso8601()));
    {
        std::lock_guard<std::mutex> lk(mMtx);
        root.set("owner", json::Value(mOwner));
        root.set("repo", json::Value(mRepo));
        root.set("stableUrl", json::Value(mCfg.redirect.enabled ? stableUrl() : mRedirectTarget));
        json::Array arr;
        for (auto const& t : mTargets) arr.push_back(targetToJson(t));
        root.set("packs", json::Value(std::move(arr)));

        // 扁平映射：资源包标识 -> 完整外链，mod 层直接消费这一段写入 BDS 内存
        json::Array flat;
        for (auto const& e : buildPackCdnEntries(mTargets)) {
            json::Value o = json::Value::object();
            o.set("packId", json::Value(e.packId));
            o.set("uuid", json::Value(e.uuid));
            o.set("version", json::Value(e.version));
            o.set("url", json::Value(e.url));
            o.set("directUrl", json::Value(e.directUrl));
            flat.push_back(std::move(o));
        }
        root.set("packCdnUrls", json::Value(std::move(flat)));
    }
    auto path = mBaseDir / fsu::fromUtf8(mCfg.manifestFile);
    return fsu::writeFileAtomic(path, root.dump(2) + "\n");
}

// 写入 BDS 官方 cdn_config.json。
//
// 格式是**扁平对象** map<string, string>：键 = 资源包标识，值 = 完整下载地址。
// 这一点由运行时实测确认，而不是猜测：BDS 的 CDNConfig::_readConfigFile() 会把顶层
// JSON 对象的每个成员直接塞进 vector<pair<string,string>> mPackCDNUrls，且对非字符串
// 值填入空串。早先版本这里写成 {"CDN URL": ..., "packs": [{...}]}，结果 BDS 内存里
// 多出 ("CDN URL", "...") 和 ("packs", "") 两条垃圾条目——数组被整体丢掉了。
//
// 因此：
//   - 不再输出 packs 数组（BDS 不认，只会变成一条空值垃圾键）；
//   - 每个资源包同时写 <uuid> 与 <uuid>_<version> 两个键，覆盖 BDS 两种取值习惯；
//   - "CDN URL" 作为历史遗留的前缀键保留（basePrefix）。
// 完整的结构化信息（tag / asset / 各镜像候选）另存于 .mcpack-sync/cdn.json，不丢。
Status CdnManager::writeBdsCdnConfig() const {
    // "CDN URL" 填写当前生效的加速前缀（直连时为空），供不支持逐包 url 的老版本回退使用。
    std::string basePrefix;
    for (auto const& d : mCfg.mirrors) {
        if (d.name == mCfg.activeMirror && d.enabled) {
            basePrefix = d.prefix;
            break;
        }
    }

    json::Value root = json::Value::object();
    if (!mCfg.bdsCdnConfig.key.empty()) {
        root.set(mCfg.bdsCdnConfig.key, json::Value(basePrefix));
    }

    {
        std::lock_guard<std::mutex> lk(mMtx);
        for (auto const& t : mTargets) {
            if (t.uuid.empty()) continue;
            // 关键：完整下载地址（已含可选 gh 加速前缀）
            std::string url = t.publicUrl.empty() ? t.assetUrl : t.publicUrl;
            if (url.empty()) continue;
            root.set(t.uuid, json::Value(url));
            if (!t.version.empty()) {
                root.set(t.uuid + "_" + t.version, json::Value(url));
            }
        }
    }

    auto path = mBaseDir / fsu::fromUtf8(mCfg.bdsCdnConfig.file);
    auto st   = fsu::writeFileAtomic(path, root.dump(2) + "\n");
    if (st) logger().info("cdn: wrote " + fsu::toUtf8(path) + " (key=" + mCfg.bdsCdnConfig.key + ")");
    return st;
}

Status CdnManager::patchServerProperties() const {
    auto path = mBaseDir / fsu::fromUtf8(mCfg.serverProperties.file);
    std::string text;
    auto        st = fsu::readFile(path, text);
    if (!st && st.error().code != ErrCode::NotFound) return st;

    std::string url = redirectTarget();
    if (url.empty()) url = stableUrl();

    std::string key  = mCfg.serverProperties.key;
    std::string line = key + "=" + url;

    std::vector<std::string> lines;
    bool                     replaced = false;
    size_t                   start    = 0;
    while (start <= text.size()) {
        auto eol = text.find('\n', start);
        std::string cur = (eol == std::string::npos) ? text.substr(start) : text.substr(start, eol - start);
        // 去掉行尾 \r 便于比较
        std::string cmp = cur;
        while (!cmp.empty() && cmp.back() == '\r') cmp.pop_back();
        if (!replaced && cmp.rfind(key + "=", 0) == 0) {
            lines.push_back(line);
            replaced = true;
        } else if (!cmp.empty()) {
            lines.push_back(cmp);
        }
        if (eol == std::string::npos) break;
        start = eol + 1;
    }
    if (!replaced) lines.push_back(line);

    std::string out;
    for (auto const& l : lines) {
        out += l;
        out += "\r\n";
    }
    return fsu::writeFileAtomic(path, out);
}

Status CdnManager::startRedirectServer() {
#ifdef _WIN32
    if (mRunning.load()) return {};
    auto st = ensureWsa();
    if (!st) return st;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        releaseWsa();
        return Error{ErrCode::Network, "socket() failed"};
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(mCfg.redirect.port));
    if (mCfg.redirect.bind.empty() || mCfg.redirect.bind == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, mCfg.redirect.bind.c_str(), &addr.sin_addr);
    }

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(s);
        releaseWsa();
        return Error{ErrCode::Network, "bind failed on port " + std::to_string(mCfg.redirect.port),
                     "wsa error " + std::to_string(err)};
    }
    if (listen(s, 16) == SOCKET_ERROR) {
        closesocket(s);
        releaseWsa();
        return Error{ErrCode::Network, "listen failed"};
    }

    mListenSocket = static_cast<uintptr_t>(s);
    mRunning.store(true);
    mThread = std::thread([this] { serveLoop(); });
    logger().info("cdn: redirect server listening on " + mCfg.redirect.bind + ":" + std::to_string(mCfg.redirect.port) +
                  " path " + mCfg.redirect.path);
    return {};
#else
    return Error{ErrCode::Network, "redirect server not implemented on this platform"};
#endif
}

void CdnManager::stopRedirectServer() {
#ifdef _WIN32
    if (!mRunning.exchange(false)) return;
    SOCKET s = static_cast<SOCKET>(mListenSocket);
    if (s != 0 && s != INVALID_SOCKET) {
        closesocket(s);
        mListenSocket = 0;
    }
    if (mThread.joinable()) mThread.join();
    releaseWsa();
#endif
}

void CdnManager::serveLoop() {
#ifdef _WIN32
    SOCKET ls = static_cast<SOCKET>(mListenSocket);
    while (mRunning.load()) {
        sockaddr_in cli{};
        int         cliLen = sizeof cli;
        SOCKET      c      = accept(ls, reinterpret_cast<sockaddr*>(&cli), &cliLen);
        if (c == INVALID_SOCKET) {
            if (!mRunning.load()) break;
            continue;
        }
        handleClient(static_cast<uintptr_t>(c));
        closesocket(c);
    }
#endif
}

void CdnManager::handleClient(uintptr_t sock) {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(sock);

    char  buf[2048];
    int   n = recv(s, buf, sizeof buf - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    std::string req(buf);
    auto        sp1 = req.find(' ');
    if (sp1 == std::string::npos) return;
    auto sp2 = req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return;
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string body;
    std::string head;

    std::string routePath = mCfg.redirect.path;
    if (routePath.empty() || routePath[0] != '/') routePath = "/" + routePath;

    if (path.rfind("/health", 0) == 0) {
        body = "{\"status\":\"ok\",\"target\":\"" + redirectTarget() + "\"}\n";
        head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n";
    } else if (path.rfind(routePath, 0) == 0) {
        std::string target = redirectTarget();
        if (target.empty()) {
            body = "no release published yet\n";
            head = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
        } else {
            head = "HTTP/1.1 302 Found\r\nLocation: " + target + "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        }
    } else {
        body = "not found\n";
        head = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n";
    }

    std::string resp = head + body;
    send(s, resp.data(), static_cast<int>(resp.size()), 0);
#endif
}

} // namespace mcpsync
