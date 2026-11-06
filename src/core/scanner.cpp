#include "mcpsync/scanner.hpp"
#include "mcpsync/diff.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"
#include "mcpsync/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>

namespace mcpsync {

namespace {

bool parseVersionArray(json::Value const& v, Version& out) {
    if (!v.isArray()) return false;
    int i = 0;
    for (auto const& e : v.asArray()) {
        if (i >= 3) break;
        out[i++] = static_cast<int>(e.asInt(0));
    }
    return i > 0;
}

bool ignored(std::string const& rel, std::vector<std::string> const& patterns) {
    for (auto const& p : patterns) {
        if (fsu::matchGlob(p, rel)) return true;
    }
    return false;
}

Result<std::string> hashFile(std::filesystem::path const& p, uint64_t& sizeOut) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return Error{ErrCode::Io, "open for hash failed: " + fsu::toUtf8(p)};
    Sha256            h;
    std::vector<char> buf(64 * 1024);
    uint64_t          total = 0;
    for (;;) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = f.gcount();
        if (got <= 0) break;
        h.update(buf.data(), static_cast<size_t>(got));
        total += static_cast<uint64_t>(got);
    }
    sizeOut = total;
    return h.hexDigest();
}

// 广度优先收集所有包含 manifest.json 的目录（不进入 .git / .mcpack-sync）
void collectPackDirs(std::filesystem::path const& root, std::vector<std::filesystem::path>& out) {
    namespace sfs = std::filesystem;
    std::error_code ec;
    if (fsu::exists(root / "manifest.json")) out.push_back(root);

    sfs::recursive_directory_iterator it(root, sfs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    sfs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_directory(fec)) continue;
        auto rel = fsu::relativeUtf8(root, it->path());
        if (rel == ".git" || rel.rfind(".git/", 0) == 0 || rel == ".mcpack-sync" ||
            rel.rfind(".mcpack-sync/", 0) == 0 || rel == ".github") {
            it.disable_recursion_pending();
            continue;
        }
        if (fsu::exists(it->path() / "manifest.json")) {
            out.push_back(it->path());
            it.disable_recursion_pending(); // 资源包内部不再嵌套
        }
    }
}

} // namespace

Result<PackIdentity> parseManifest(std::filesystem::path const& manifestPath) {
    std::string text;
    auto        st = fsu::readFile(manifestPath, text);
    if (!st) return st.error();

    json::Value root;
    std::string err;
    if (!json::parse(text, root, err)) {
        return Error{ErrCode::Parse, "manifest.json invalid: " + fsu::toUtf8(manifestPath), err};
    }
    if (!root.isObject()) {
        return Error{ErrCode::Parse, "manifest.json root is not an object: " + fsu::toUtf8(manifestPath)};
    }

    PackIdentity id;
    id.formatVersion = static_cast<int>(root.at("format_version").asInt(2));

    auto const& header = root.at("header");
    if (!header.isObject()) {
        return Error{ErrCode::Parse, "manifest.json missing 'header': " + fsu::toUtf8(manifestPath)};
    }
    id.uuid        = header.at("uuid").asString();
    id.name        = header.at("name").asString();
    id.description = header.at("description").asString();
    if (id.uuid.empty()) {
        return Error{ErrCode::Parse, "manifest.json header.uuid missing: " + fsu::toUtf8(manifestPath)};
    }
    std::transform(id.uuid.begin(), id.uuid.end(), id.uuid.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (!parseVersionArray(header.at("version"), id.version)) {
        return Error{ErrCode::Parse, "manifest.json header.version invalid: " + fsu::toUtf8(manifestPath)};
    }
    parseVersionArray(header.at("min_engine_version"), id.minEngine);

    auto const& meta = root.at("metadata");
    if (meta.isObject()) id.productType = meta.at("product_type").asString();

    auto const& modules = root.at("modules");
    if (modules.isArray()) {
        for (auto const& m : modules.asArray()) {
            if (!m.isObject()) continue;
            if (m.at("type").asString() == "resources") {
                auto mu = m.at("uuid").asString();
                if (!mu.empty()) {
                    std::transform(mu.begin(), mu.end(), mu.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    id.moduleUuid = mu;
                }
                break;
            }
        }
    }
    return id;
}

Result<PackSnapshot> snapshotPack(std::filesystem::path const& dir, ScanOptions const& opts) {
    auto manifest = dir / "manifest.json";
    if (!fsu::exists(manifest)) {
        return Error{ErrCode::NotFound, "no manifest.json in " + fsu::toUtf8(dir)};
    }
    auto idRes = parseManifest(manifest);
    if (!idRes) return idRes.error();

    PackSnapshot snap;
    snap.id  = std::move(idRes.value());
    snap.dir = dir;

    std::vector<fsu::WalkEntry> entries;
    auto                        filter = [&](std::string const& rel) { return !ignored(rel, opts.ignorePatterns); };
    auto                        st     = fsu::walkFiles(dir, entries, filter);
    if (!st) return st.error();

    snap.files.reserve(entries.size());
    std::string agg;
    agg.reserve(entries.size() * 88);

    for (auto const& e : entries) {
        FileEntry fe;
        fe.rel = e.rel;
        if (opts.computeHashes) {
            uint64_t size = 0;
            auto     h    = hashFile(e.abs, size);
            if (!h) return h.error();
            fe.size   = size;
            fe.sha256 = h.value();
        } else {
            fe.size = e.size;
        }
        snap.totalBytes += fe.size;
        snap.newestMtime = std::max(snap.newestMtime, e.mtime);
        agg += fe.rel;
        agg.push_back('\0');
        agg += fe.sha256;
        agg.push_back('\n');
        snap.files.push_back(std::move(fe));
    }

    snap.fileSetHash = Sha256::hex(agg);
    snap.fingerprint = computeFingerprint(snap.id, snap.fileSetHash);
    return snap;
}

Result<ScanReport> scanResourceDir(std::filesystem::path const& root, ScanOptions const& opts) {
    auto t0 = std::chrono::steady_clock::now();

    ScanReport report;
    if (!fsu::isDirectory(root)) {
        return Error{ErrCode::NotFound, "resource directory not found: " + fsu::toUtf8(root)};
    }

    std::vector<std::filesystem::path> packDirs;
    collectPackDirs(root, packDirs);
    std::sort(packDirs.begin(), packDirs.end());
    packDirs.erase(std::unique(packDirs.begin(), packDirs.end()), packDirs.end());

    if (packDirs.empty()) {
        report.warnings.push_back("no resource pack (manifest.json) found under " + fsu::toUtf8(root));
    }

    std::map<std::string, PackSnapshot> byUuid;
    for (auto const& dir : packDirs) {
        auto snap = snapshotPack(dir, opts);
        if (!snap) {
            report.warnings.push_back("skip " + fsu::toUtf8(dir) + ": " + snap.error().str());
            continue;
        }
        auto& s = snap.value();
        report.filesHashed += s.files.size();
        report.bytesRead += s.totalBytes;

        auto it = byUuid.find(s.id.uuid);
        if (it == byUuid.end()) {
            byUuid.emplace(s.id.uuid, std::move(s));
            continue;
        }

        // 同 uuid 多目录：记录冲突，默认保留版本更高者，交由上层策略裁决
        PackSnapshot& kept = it->second;
        Change        c;
        c.kind           = ChangeKind::Updated;
        c.id             = s.id;
        c.dir            = s.dir;
        c.oldVersion     = kept.id.version;
        c.newVersion     = s.id.version;
        c.oldFingerprint = kept.fingerprint;
        c.newFingerprint = s.fingerprint;
        c.reason = "duplicate uuid: kept " + fsu::toUtf8(kept.dir) + ", also at " + fsu::toUtf8(s.dir);
        report.conflicts.push_back(c);

        if (compareVersion(s.id.version, kept.id.version) > 0) {
            kept = std::move(s);
        }
    }

    for (auto& kv : byUuid) {
        report.packs.push_back(std::move(kv.second));
    }
    std::sort(report.packs.begin(), report.packs.end(), [](PackSnapshot const& a, PackSnapshot const& b) {
        return a.id.uuid < b.id.uuid;
    });

    auto t1          = std::chrono::steady_clock::now();
    report.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return report;
}

} // namespace mcpsync
