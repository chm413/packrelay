// mcpack-sync 单元测试（零依赖，返回非零表示失败）
#include "mcpsync/cdn.hpp"
#include "mcpsync/config.hpp"
#include "mcpsync/diff.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/gitauth.hpp"
#include "mcpsync/http.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/mirror.hpp"
#include "mcpsync/model.hpp"
#include "mcpsync/scanner.hpp"
#include "mcpsync/sha256.hpp"
#include "mcpsync/state.hpp"

#include <cstdio>
#include <string>
#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            ++g_pass;                                                                                                  \
        } else {                                                                                                       \
            ++g_fail;                                                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        auto _va = (a);                                                                                                \
        auto _vb = (b);                                                                                                \
        if (_va == _vb) {                                                                                              \
            ++g_pass;                                                                                                  \
        } else {                                                                                                       \
            ++g_fail;                                                                                                  \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);                                         \
        }                                                                                                              \
    } while (0)

using namespace mcpsync;

void testSha256() {
    CHECK_EQ(Sha256::hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Sha256::hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(Sha256::hex("The quick brown fox jumps over the lazy dog"),
             std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
    // 分块喂入结果必须一致
    Sha256 h;
    std::string big(1000, 'x');
    for (int i = 0; i < 10; ++i) h.update(big.data(), big.size());
    std::string chunked = h.hexDigest();
    Sha256 h2;
    std::string whole(10000, 'x');
    h2.update(whole.data(), whole.size());
    CHECK_EQ(chunked, h2.hexDigest());
}

void testJson() {
    std::string text = R"({"a":1,"b":[1,2,3],"c":{"d":"e"},"f":true,"g":null,"h":"中文\u4e2d"})";
    json::Value v;
    std::string err;
    CHECK(json::parse(text, v, err));
    CHECK_EQ(v.at("a").asInt(), int64_t(1));
    CHECK_EQ(v.at("b").size(), size_t(3));
    CHECK_EQ(v.at("c").at("d").asString(), std::string("e"));
    CHECK(v.at("f").asBool());
    CHECK(v.at("g").isNull());
    CHECK_EQ(v.at("h").asString(), std::string("中文中"));

    auto dumped = v.dump(0);
    json::Value v2;
    CHECK(json::parse(dumped, v2, err));
    CHECK_EQ(v2.at("b").at(2).asInt(), int64_t(3));

    // 非法 JSON 必须报错而不是静默
    json::Value bad;
    CHECK(!json::parse("{\"a\":}", bad, err));
    CHECK(!err.empty());

    // 键顺序保持
    json::Value o = json::Value::object();
    o.set("z", json::Value(1));
    o.set("a", json::Value(2));
    auto s = o.dump(0);
    CHECK(s.find("\"z\"") < s.find("\"a\""));
}

void testGlob() {
    CHECK(fsu::matchGlob("**/*.tmp", "a/b/c.tmp"));
    CHECK(fsu::matchGlob("**/*.tmp", "c.tmp"));
    CHECK(!fsu::matchGlob("**/*.tmp", "c.txt"));
    CHECK(fsu::matchGlob("*.json", "manifest.json"));
    CHECK(!fsu::matchGlob("*.json", "a/manifest.json"));
    CHECK(fsu::matchGlob("**/.DS_Store", "x/y/.DS_Store"));
    CHECK(fsu::matchGlob("**/Thumbs.db", "Thumbs.db"));
}

void testVersionAndSlug() {
    Version v{1, 0, 3};
    CHECK_EQ(versionToString(v), std::string("1.0.3"));
    Version p{0, 0, 0};
    CHECK(parseVersion("2.10.1", p));
    CHECK_EQ(p[0], 2);
    CHECK_EQ(p[1], 10);
    CHECK_EQ(p[2], 1);
    CHECK(compareVersion(Version{1, 0, 9}, Version{1, 1, 0}) < 0);
    CHECK(compareVersion(Version{1, 2, 0}, Version{1, 2, 0}) == 0);

    PackSnapshot s;
    s.id.uuid = "541650da-507a-473a-b2f1-162103ffc351";
    s.id.name = "Kaleidoscope Tavern v1.0.3 [RP]";
    CHECK_EQ(s.slug(), std::string("kaleidoscope-tavern-v1.0.3-rp"));

    PackSnapshot s2;
    s2.id.uuid = "541650da-507a-473a-b2f1-162103ffc351";
    s2.id.name = "pack.name";       // i18n key -> 退回 uuid
    CHECK_EQ(s2.slug(), std::string("541650da-507a-473a-b2f1-162103ffc351"));
}

void testMirror() {
    // 基线：GitHub 托管模式下资源包的完整下载地址
    std::string assetUrl = buildReleaseAssetUrl("chm413", "mcpack", "pack-v1.0.0-ab12cd34", "pack-v1.0.0-ab12cd34.zip");
    CHECK_EQ(
        assetUrl,
        std::string("https://github.com/chm413/mcpack/releases/download/pack-v1.0.0-ab12cd34/"
                    "pack-v1.0.0-ab12cd34.zip")
    );

    // 前缀式加速：ghfast 直接贴在完整地址前，不编码
    CHECK_EQ(concatUrlPrefix("https://ghfast.top/", assetUrl), std::string("https://ghfast.top/") + assetUrl);
    // 前缀缺尾斜杠时自动补齐，且不重复补
    CHECK_EQ(concatUrlPrefix("https://ghfast.top", assetUrl), std::string("https://ghfast.top/") + assetUrl);
    CHECK_EQ(concatUrlPrefix("https://ghfast.top//", assetUrl), std::string("https://ghfast.top/") + assetUrl);
    // 空前缀 = 直连
    CHECK_EQ(concatUrlPrefix("", assetUrl), assetUrl);

    std::vector<MirrorDef> defs = {
        {"direct", true, 0, "{url}", ""},
        {"ghfast", true, 10, "", "https://ghfast.top/"},
        {"jsdelivr", true, 20, "https://cdn.jsdelivr.net/gh/{owner}/{repo}@{tag}/{path}", ""},
        {"disabled", false, 5, "https://nope/{url}", ""},
    };

    MirrorResolver autoR(defs, "auto");
    auto           c = autoR.resolve(assetUrl, "o", "r", "tag", "a.zip", "packs/x");
    CHECK_EQ(c.size(), size_t(3));
    CHECK_EQ(c[0].name, std::string("direct"));
    CHECK_EQ(c[1].name, std::string("ghfast"));
    CHECK_EQ(c[1].url, std::string("https://ghfast.top/") + assetUrl);
    CHECK_EQ(c[2].url, std::string("https://cdn.jsdelivr.net/gh/o/r@tag/packs/x"));

    auto sel = autoR.select(c);
    CHECK_EQ(sel.mirrorName, std::string("direct"));

    MirrorResolver pinned(defs, "ghfast");
    auto           sel2 = pinned.select(pinned.resolve(assetUrl, "o", "r", "tag", "a.zip", "p"));
    CHECK_EQ(sel2.mirrorName, std::string("ghfast"));
    CHECK_EQ(sel2.url, std::string("https://ghfast.top/") + assetUrl);

    // 指定镜像不存在时退回 auto
    MirrorResolver missing(defs, "not-exist");
    auto           sel3 = missing.select(missing.resolve(assetUrl, "o", "r", "tag", "a.zip", "p"));
    CHECK_EQ(sel3.mirrorName, std::string("direct"));
}

void testConfig() {
    auto cfg = parseConfig(std::string(R"({
        "resourceDir": "rp",
        "git": { "remote": "https://example.com/x.git" },
        "github": { "owner": "o", "repo": "r" },
        "cdn": { "mirrors": [ { "name": "m", "template": "https://m/{url}" } ] }
    })"), fsu::fromUtf8("D:/tmp/cfg"));
    CHECK(cfg.ok());
    auto& c = cfg.value();
    CHECK_EQ(c.resourceDir, std::string("rp"));
    CHECK_EQ(c.git.branch, std::string("main"));
    CHECK_EQ(c.cdn.mirrors.size(), size_t(1));
    CHECK_EQ(c.server.scanIntervalSeconds, 60);
    CHECK(validateConfig(c).ok());

    auto bad = parseConfig("{ not json", fsu::fromUtf8("."));
    CHECK(!bad.ok());

    // 默认配置必须自身可解析
    auto def = parseConfig(defaultConfigJson(), fsu::fromUtf8("."));
    CHECK(def.ok());
    CHECK(validateConfig(def.value()).ok());
}

void testScannerAndDiff() {
    namespace fs = std::filesystem;
    auto root    = fs::temp_directory_path() / ("mcpsync-test-" + std::to_string(fsu::nowUnix()));
    fsu::removeAll(root);
    fsu::ensureDir(root);

    auto packDir = root / "packA";
    fsu::ensureDir(packDir / "textures");
    fsu::writeFile(packDir / "manifest.json", R"({
        "format_version": 2,
        "header": { "name": "Test Pack", "uuid": "aaaaaaaa-1111-2222-3333-444444444444", "version": [1,0,0] },
        "modules": [ { "type": "resources", "uuid": "bbbbbbbb-1111-2222-3333-444444444444", "version": [1,0,0] } ]
    })");
    fsu::writeFile(packDir / "textures" / "a.png", "AAAA");
    fsu::writeFile(packDir / "textures" / "b.png", "BBBB");

    ScanOptions so;
    so.ignorePatterns = {"**/.DS_Store"};

    auto report = scanResourceDir(root, so);
    CHECK(report.ok());
    CHECK_EQ(report.value().packs.size(), size_t(1));
    auto const& snap = report.value().packs[0];
    CHECK_EQ(snap.id.uuid, std::string("aaaaaaaa-1111-2222-3333-444444444444"));
    CHECK_EQ(snap.files.size(), size_t(3));
    CHECK_EQ(snap.totalBytes, uint64_t(4 + 4 + snap.files[0].size)); // manifest 长度不定，只校验前两项
    CHECK_EQ(snap.fileSetHash.size(), size_t(64));

    // 1) 首次：Added
    StateStore st(root / ".mcpack-sync" / "state.json");
    auto       d1 = diffSnapshots(report.value().packs, st);
    CHECK_EQ(d1.added, size_t(1));
    CHECK_EQ(d1.pending.size(), size_t(1));

    // 2) 写入状态后重复扫描：Unchanged（不应重复推送）
    PackState ps;
    ps.uuid        = snap.id.uuid;
    ps.name        = snap.id.name;
    ps.slug        = snap.slug();
    ps.version     = snap.id.version;
    ps.fileSetHash = snap.fileSetHash;
    ps.fingerprint = snap.fingerprint;
    st.put(ps);
    auto d2 = diffSnapshots(report.value().packs, st);
    CHECK_EQ(d2.unchanged, size_t(1));
    CHECK_EQ(d2.pending.size(), size_t(0));

    // 3) 内容变化但版本不变：Updated（防止"版本没改就不推"的漏更）
    fsu::writeFile(packDir / "textures" / "a.png", "AAAA-CHANGED");
    auto r3 = scanResourceDir(root, so);
    CHECK(r3.ok());
    auto d3 = diffSnapshots(r3.value().packs, st);
    CHECK_EQ(d3.updated, size_t(1));
    CHECK_EQ(d3.pending.size(), size_t(1));

    // 4) 只改版本：Updated
    fsu::writeFile(packDir / "manifest.json", R"({
        "format_version": 2,
        "header": { "name": "Test Pack", "uuid": "aaaaaaaa-1111-2222-3333-444444444444", "version": [1,0,1] },
        "modules": [ { "type": "resources", "uuid": "bbbbbbbb-1111-2222-3333-444444444444", "version": [1,0,1] } ]
    })");
    auto r4 = scanResourceDir(root, so);
    auto d4 = diffSnapshots(r4.value().packs, st);
    CHECK_EQ(d4.updated, size_t(1));

    // 5) 删除目录：Removed
    fsu::removeAll(packDir);
    auto r5 = scanResourceDir(root, so);
    CHECK(r5.ok());
    auto d5 = diffSnapshots(r5.value().packs, st);
    CHECK_EQ(d5.removed, size_t(1));

    // 6) 状态持久化往返
    st.save();
    StateStore st2(root / ".mcpack-sync" / "state.json");
    auto       loaded = st2.load();
    CHECK(loaded.ok());
    CHECK(loaded.value());
    CHECK_EQ(st2.size(), size_t(1));

    // 7) 非法 manifest 不应崩溃，只跳过
    auto badDir = root / "broken";
    fsu::ensureDir(badDir);
    fsu::writeFile(badDir / "manifest.json", "{ broken");
    auto r6 = scanResourceDir(root, so);
    CHECK(r6.ok());
    CHECK(!r6.value().warnings.empty());

    fsu::removeAll(root);
}

void testStateAtomic() {
    namespace fs = std::filesystem;
    auto dir     = fs::temp_directory_path() / ("mcpsync-state-" + std::to_string(fsu::nowUnix()));
    fsu::removeAll(dir);
    StateStore st(dir / "state.json");
    PackState  ps;
    ps.uuid  = "u1";
    ps.name  = "n";
    ps.slug  = "s";
    ps.version = {1, 2, 3};
    st.put(ps);
    CHECK(st.save().ok());
    CHECK(fsu::exists(dir / "state.json"));
    // 原子写不应留下 .tmp
    CHECK(!fsu::exists(dir / "state.json.tmp"));
    fsu::removeAll(dir);
}

void testPackCdnEntries() {
    CdnTarget t;
    t.uuid      = "541650da-507a-473a-b2f1-162103ffc351";
    t.version   = "1.0.3";
    t.assetUrl  = "https://github.com/chm413/mcpack/releases/download/x/x.zip";
    t.publicUrl = "https://ghfast.top/" + t.assetUrl;

    auto e = buildPackCdnEntries({t});
    CHECK_EQ(e.size(), size_t(1));
    CHECK_EQ(e[0].packId, std::string("541650da-507a-473a-b2f1-162103ffc351_1.0.3"));
    CHECK_EQ(e[0].url, t.publicUrl);
    CHECK_EQ(e[0].directUrl, t.assetUrl);

    // 版本缺失时退回裸 uuid 作主键
    CdnTarget noVer = t;
    noVer.version.clear();
    auto e2 = buildPackCdnEntries({noVer});
    CHECK_EQ(e2.size(), size_t(1));
    CHECK_EQ(e2[0].packId, std::string("541650da-507a-473a-b2f1-162103ffc351"));

    // publicUrl 为空时退回直连地址
    CdnTarget direct = t;
    direct.publicUrl.clear();
    auto e3 = buildPackCdnEntries({direct});
    CHECK_EQ(e3.at(0).url, t.assetUrl);

    // 没有地址的条目应被丢弃
    CdnTarget broken;
    broken.uuid = "u-broken";
    CHECK_EQ(buildPackCdnEntries({broken}).size(), size_t(0));

    // 从 cdn.json 清单解析出同样的映射
    std::string manifest = R"({
      "packs": [
        { "uuid": "u1", "version": "2.0.0",
          "assetUrl": "https://github.com/o/r/releases/download/t/a.zip",
          "publicUrl": "https://ghfast.top/https://github.com/o/r/releases/download/t/a.zip" }
      ]
    })";
    std::vector<PackCdnEntry> parsed;
    CHECK(parsePackCdnEntries(manifest, parsed).ok());
    CHECK_EQ(parsed.size(), size_t(1));
    CHECK_EQ(parsed[0].packId, std::string("u1_2.0.0"));
    CHECK_EQ(parsed[0].url, std::string("https://ghfast.top/https://github.com/o/r/releases/download/t/a.zip"));
    CHECK_EQ(parsed[0].directUrl, std::string("https://github.com/o/r/releases/download/t/a.zip"));

    // 非法 JSON / 空清单应报错而不是返回空映射
    std::vector<PackCdnEntry> tmp;
    CHECK(!parsePackCdnEntries("{ not json", tmp).ok());
    CHECK(!parsePackCdnEntries(R"({"packs":[]})", tmp).ok());
}

void testUpsertPackCdnUrls() {
    using UrlMap = std::vector<std::pair<std::string, std::string>>;

    PackCdnEntry e;
    e.uuid      = "uuid-a";
    e.version   = "1.0.0";
    e.packId    = "uuid-a_1.0.0";
    e.directUrl = "https://github.com/o/r/releases/download/t1/a.zip";
    e.url       = "https://ghfast.top/" + e.directUrl;

    // 第一次写入：packId + uuid 两条
    UrlMap urls{{"someone-else", "https://keep.me/x.zip"}};
    upsertPackCdnUrls(urls, {e});
    CHECK_EQ(urls.size(), size_t(3));
    CHECK_EQ(urls[0].first, std::string("someone-else"));
    CHECK_EQ(urls[1].first, std::string("uuid-a_1.0.0"));
    CHECK_EQ(urls[1].second, e.url);
    CHECK_EQ(urls[2].first, std::string("uuid-a"));
    CHECK_EQ(urls[2].second, e.url);

    // 幂等：重复写入不增长
    upsertPackCdnUrls(urls, {e});
    CHECK_EQ(urls.size(), size_t(3));

    // 新版本：主键变化后旧键被清掉，只留一条新记录 + uuid 别名
    PackCdnEntry e2 = e;
    e2.version      = "1.1.0";
    e2.packId       = "uuid-a_1.1.0";
    e2.url          = "https://ghfast.top/https://github.com/o/r/releases/download/t2/a.zip";
    upsertPackCdnUrls(urls, {e2});
    CHECK_EQ(urls.size(), size_t(3));
    CHECK_EQ(urls[0].first, std::string("someone-else"));
    CHECK_EQ(urls[1].first, std::string("uuid-a_1.1.0"));
    CHECK_EQ(urls[1].second, e2.url);
    CHECK_EQ(urls[2].first, std::string("uuid-a"));
    CHECK_EQ(urls[2].second, e2.url);

    // 空输入不动容器
    UrlMap untouched{{"k", "v"}};
    upsertPackCdnUrls(untouched, {});
    CHECK_EQ(untouched.size(), size_t(1));
}

// BDS 的 cdn_config.json 必须是**扁平**的 key -> string 对象。
//
// 这条测试锁死一个已经踩过的坑：CDNConfig::_readConfigFile() 会把顶层 JSON 对象的每个
// 成员直接塞进 vector<pair<string,string>> mPackCDNUrls，非字符串值退化为空串。
// 早期版本写成 {"CDN URL": ..., "packs": [{...}]}，结果 BDS 内存里凭空多出
// ("packs", "") 这条垃圾记录（运行时日志实测：mPackCDNUrls now 14 entries，
// 其中 12 条是我们的包、2 条是 "CDN URL" 与 "packs"）。
void testBdsCdnConfigFile() {
    namespace fs = std::filesystem;
    auto dir     = fs::temp_directory_path() / ("mcpsync-bdscdn-" + std::to_string(fsu::nowUnix()));
    fsu::removeAll(dir);
    fsu::ensureDir(dir / ".mcpack-sync");

    // 预置上次发布的清单（含 version，用于生成 <uuid>_<version> 主键）
    std::string manifest = R"({
      "owner": "o",
      "repo": "r",
      "packs": [
        { "uuid": "aaaa1111-2222", "version": "1.0.3",
          "assetUrl": "https://github.com/o/r/releases/download/t/a.zip",
          "publicUrl": "https://ghfast.top/https://github.com/o/r/releases/download/t/a.zip" }
      ]
    })";
    CHECK(fsu::writeFileAtomic(dir / ".mcpack-sync/cdn.json", manifest).ok());

    CdnConfig cfg;
    cfg.enabled                 = true;
    cfg.manifestFile            = ".mcpack-sync/cdn.json";
    cfg.activeMirror            = "auto";
    cfg.bdsCdnConfig.enabled    = true;
    cfg.bdsCdnConfig.file       = "cdn_config.json";
    cfg.bdsCdnConfig.key        = "CDN URL";
    cfg.serverProperties.enabled = false;
    cfg.redirect.enabled         = false;

    CdnManager cdn(cfg, dir);
    cdn.init();
    CHECK_EQ(cdn.targets().size(), size_t(1));
    CHECK(cdn.writeManifest().ok());
    CHECK(cdn.writeBdsCdnConfig().ok());

    std::string text;
    CHECK(fsu::readFile(dir / "cdn_config.json", text).ok());
    json::Value root;
    std::string err;
    CHECK(json::parse(text, root, err));
    CHECK(root.isObject());

    // 1) 顶层不允许出现 packs 数组：BDS 会把它退化成 ("packs","") 垃圾条目
    CHECK(!root.has("packs"));
    CHECK(root.at("packs").isNull());

    // 2) 不允许任何数组类型值，全部必须是字符串
    bool allStrings = true;
    for (auto const& kv : root.asObject()) {
        if (!kv.second.isString()) allStrings = false;
    }
    CHECK(allStrings);

    // 3) 遗留前缀键保留
    CHECK(root.has("CDN URL"));

    // 4) 两种主键都写，值都是含加速前缀的完整地址
    std::string const fullUrl = "https://ghfast.top/https://github.com/o/r/releases/download/t/a.zip";
    CHECK_EQ(root.at("aaaa1111-2222").asString(), fullUrl);
    CHECK_EQ(root.at("aaaa1111-2222_1.0.3").asString(), fullUrl);

    // 5) 清单里必须带 packCdnUrls（mod 层写 BDS 内存时优先消费这一段）
    std::string manifestOut;
    CHECK(fsu::readFile(dir / ".mcpack-sync/cdn.json", manifestOut).ok());
    json::Value manifestRoot;
    CHECK(json::parse(manifestOut, manifestRoot, err));
    CHECK(manifestRoot.at("packCdnUrls").isArray());
    CHECK_EQ(manifestRoot.at("packCdnUrls").size(), size_t(1));

    // 6) 同样的文件能被我们自己重新解析回映射（自洽）
    std::vector<PackCdnEntry> parsed;
    CHECK(parsePackCdnEntries(manifestOut, parsed).ok());
    CHECK_EQ(parsed.size(), size_t(1));
    CHECK_EQ(parsed[0].packId, std::string("aaaa1111-2222_1.0.3"));
    CHECK_EQ(parsed[0].url, fullUrl);

    fsu::removeAll(dir);
}

void testGitAuth() {
    // 脱敏
    CHECK_EQ(maskSecret(""), std::string("(unset)"));
    CHECK_EQ(maskSecret("short"), std::string("********"));
    CHECK_EQ(maskSecret("ghp_1234567890abcdef"), std::string("ghp_****cdef"));

    // token 解析：inline 模式用配置值，token_env 模式读环境变量
    GitConfig c;
    c.authMode = "token_inline";
    c.token    = "inline-token";
    CHECK_EQ(resolveGitToken(c), std::string("inline-token"));

    c.authMode = "credential_helper";
    CHECK_EQ(resolveGitToken(c), std::string(""));

    c.authMode = "ssh";
    CHECK_EQ(resolveGitToken(c), std::string(""));

    // HTTPS 认证参数：basic 头，用户名默认 x-access-token
    GitConfig t;
    t.authMode = "token_inline";
    t.token    = "tk";
    auto args  = buildAuthArgs(t);
    CHECK_EQ(args.size(), size_t(2));
    CHECK_EQ(args[0], std::string("-c"));
    CHECK_EQ(args[1], std::string("http.extraheader=AUTHORIZATION: basic ") + base64Encode("x-access-token:tk"));

    // 同时给了用户名密码时优先用密码
    t.username = "bot";
    t.password = "pw";
    auto args2 = buildAuthArgs(t);
    CHECK_EQ(args2.at(1), std::string("http.extraheader=AUTHORIZATION: basic ") + base64Encode("bot:pw"));

    // SSH 走 core.sshCommand，避免依赖环境变量
    GitConfig s;
    s.authMode = "ssh";
    s.sshKey   = "C:/keys/id_ed25519";
    auto sargs = buildAuthArgs(s);
    CHECK_EQ(sargs.size(), size_t(2));
    CHECK_EQ(sargs[0], std::string("-c"));
    CHECK(sargs[1].rfind("core.sshCommand=ssh -i \"C:/keys/id_ed25519\"", 0) == 0);

    // 无凭据时返回空参数
    GitConfig none;
    none.authMode = "credential_helper";
    CHECK_EQ(buildAuthArgs(none).size(), size_t(0));
    CHECK(hasGitCredential(none));  // credential_helper 视为已配置

    GitConfig noTok;
    noTok.authMode = "token_inline";
    noTok.tokenEnv = "";  // 不读任何环境变量
    CHECK(!hasGitCredential(noTok));

    // 控制台修改
    GitConfig g;
    CHECK(!applyGitField(g, GitField::Remote, "").ok());
    CHECK(!applyGitField(g, GitField::Remote, "not-a-url").ok());
    CHECK(applyGitField(g, GitField::Remote, "https://github.com/o/r.git").ok());
    CHECK_EQ(g.remote, std::string("https://github.com/o/r.git"));
    CHECK(applyGitField(g, GitField::Remote, "git@github.com:o/r.git").ok());

    CHECK(!applyGitField(g, GitField::Author, "name", "no-at-sign").ok());
    CHECK(applyGitField(g, GitField::Author, "bot", "bot@local").ok());
    CHECK_EQ(g.authorEmail, std::string("bot@local"));

    // 写 token 必须自动切到 token_inline，否则会被 token_env 分支忽略
    CHECK(applyGitField(g, GitField::Token, "ghp_abcdefghijklmn").ok());
    CHECK_EQ(g.authMode, std::string("token_inline"));
    CHECK_EQ(resolveGitToken(g), std::string("ghp_abcdefghijklmn"));

    // 写 sshKey 自动切到 ssh
    CHECK(applyGitField(g, GitField::SshKey, "C:/k/id_rsa").ok());
    CHECK_EQ(g.authMode, std::string("ssh"));
    CHECK_EQ(resolveGitSshKey(g), std::string("C:/k/id_rsa"));

    // 非法 authMode 拒绝
    CHECK(!applyGitField(g, GitField::AuthMode, "nonsense").ok());
    CHECK(applyGitField(g, GitField::AuthMode, "token_env").ok());

    // clear 清空敏感项，并把 authMode 收回默认，避免留下「声明了却配不全」的矛盾状态
    CHECK(applyGitField(g, GitField::Clear, "").ok());
    CHECK(g.token.empty());
    CHECK(g.password.empty());
    CHECK(g.sshKey.empty());
    CHECK_EQ(g.authMode, std::string("token_env"));
    CHECK(validateGitForConsole(g).ok());

    // 控制台校验：ssh 模式缺私钥要报错
    GitConfig v;
    v.authMode = "ssh";
    CHECK(!validateGitForConsole(v).ok());
    v.sshKey = "k";
    CHECK(validateGitForConsole(v).ok());

    // 配置落盘：只改 git 段，保留其他字段
    namespace fs = std::filesystem;
    auto dir     = fs::temp_directory_path() / ("mcpsync-gitauth-" + std::to_string(fsu::nowUnix()));
    fsu::removeAll(dir);
    fsu::ensureDir(dir);
    auto cfgPath = dir / "config.json";
    CHECK(fsu::writeFileAtomic(cfgPath, std::string(R"({"version":1,"resourceDir":"rp","git":{"remote":"old"}})")).ok());

    GitConfig saved;
    saved.remote    = "https://github.com/chm413/mcpack.git";
    saved.authMode  = "token_inline";
    saved.token     = "ghp_secret";
    saved.sshKey    = "C:/k/id_rsa";
    saved.username  = "chm413";
    saved.extraArgs = {"--verbose"};
    CHECK(patchConfigFileGit(cfgPath, saved).ok());

    std::string text;
    CHECK(fsu::readFile(cfgPath, text).ok());
    json::Value root;
    std::string jerr;
    CHECK(json::parse(text, root, jerr));
    CHECK_EQ(root.at("resourceDir").asString(), std::string("rp"));  // 其他段被保留
    CHECK_EQ(root.at("git").at("remote").asString(), std::string("https://github.com/chm413/mcpack.git"));
    CHECK_EQ(root.at("git").at("token").asString(), std::string("ghp_secret"));
    CHECK_EQ(root.at("git").at("extraArgs").asArray().size(), size_t(1));

    // 落盘后再解析回来，凭据能还原
    auto reloaded = parseConfig(text, dir);
    CHECK(reloaded.ok());
    CHECK_EQ(resolveGitToken(reloaded.value().git), std::string("ghp_secret"));

    // 连通性测试：不存在的本地远端应快速失败并给出建议
    GitConfig probe;
    probe.remote   = "file:///" + fsu::toUtf8(dir) + "/no-such-repo.git";
    probe.authMode = "credential_helper";
    auto pr        = probeGitRemote(probe, dir, 20000);
    CHECK(!pr.ok);
    CHECK(!pr.hint.empty());

    // remote 为空时不执行 git，直接给提示
    GitConfig emptyRemote;
    auto      pr2 = probeGitRemote(emptyRemote, dir, 5000);
    CHECK(!pr2.ok);
    CHECK(pr2.hint.find("/relaygit remote") != std::string::npos);

    fsu::removeAll(dir);
}

} // namespace

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    testSha256();
    testJson();
    testGlob();
    testVersionAndSlug();
    testMirror();
    testPackCdnEntries();
    testUpsertPackCdnUrls();
    testBdsCdnConfigFile();
    testGitAuth();
    testConfig();
    testScannerAndDiff();
    testStateAtomic();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
