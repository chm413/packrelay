#include "mcpsync/pipeline.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"
#include "mcpsync/mirror.hpp"
#include "mcpsync/scanner.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>

namespace mcpsync {

namespace {

std::string renderPlaceholders(std::string tmpl, PackSnapshot const& snap) {
    struct Kv {
        char const* k;
        std::string v;
    };
    Kv kv[] = {
        {"{slug}", snap.slug()},
        {"{version}", versionToString(snap.id.version)},
        {"{uuid}", snap.id.uuid},
        {"{uuid8}", snap.id.shortUuid()},
        {"{hash8}", snap.fingerprint.substr(0, 8)},
        {"{name}", snap.id.name.empty() ? snap.slug() : snap.id.name},
    };
    std::string out = tmpl;
    for (auto const& e : kv) {
        size_t pos = 0;
        while ((pos = out.find(e.k, pos)) != std::string::npos) {
            out.replace(pos, std::strlen(e.k), e.v);
            pos += e.v.size();
        }
    }
    return out;
}

// 拼接 URL：消除 base 结尾与 tail 开头重复的 '/'
std::string urlJoin(std::string base, std::string const& tail) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    size_t i = 0;
    while (i < tail.size() && tail[i] == '/') ++i;
    return base + "/" + tail.substr(i);
}

RepoIndexEntry toIndexEntry(PackSnapshot const& snap) {
    RepoIndexEntry e;
    e.uuid        = snap.id.uuid;
    e.moduleUuid  = snap.id.moduleUuid;
    e.name        = snap.id.name;
    e.slug        = snap.slug();
    e.version     = snap.id.version;
    e.fileSetHash = snap.fileSetHash;
    e.fingerprint = snap.fingerprint;
    e.fileCount   = snap.files.size();
    e.totalBytes  = snap.totalBytes;
    e.updatedAt   = fsu::nowIso8601();
    e.path        = "packs/" + e.slug;
    return e;
}

uint64_t elapsedSince(std::chrono::steady_clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
    );
}

} // namespace

std::string PipelineReport::summary() const {
    std::ostringstream os;
    os << (ok ? "OK" : "FAIL") << " packs=" << packsScanned << " files=" << filesHashed << " added=" << added
       << " updated=" << updated << " removed=" << removed << " unchanged=" << unchanged
       << " elapsed=" << elapsedMs << "ms";
    for (auto const& o : outcomes) {
        os << "\n  - [" << toString(o.kind) << "] " << o.slug << " " << (o.ok ? "ok" : "FAILED@" + o.stage)
           << (o.message.empty() ? "" : (" : " + o.message));
        if (!o.commit.empty()) os << " commit=" << o.commit.substr(0, 8);
        if (!o.tag.empty()) os << " tag=" << o.tag;
        if (!o.cdnUrl.empty()) os << " cdn=" << o.cdnUrl;
    }
    if (!error.empty()) os << "\n  error: " << error;
    return os.str();
}

Pipeline::Pipeline(Config cfg)
: mCfg(std::move(cfg))
, mState(mCfg.statePath())
, mGit(mCfg.git, mCfg.repoPath())
, mGh(mCfg.github)
, mMirror(mCfg.cdn.mirrors, mCfg.cdn.activeMirror)
, mCdn(mCfg.cdn, mCfg.baseDir) {}

Pipeline::~Pipeline() {
    stopWatcher();
    mCdn.stopRedirectServer();
}

Status Pipeline::init() {
    auto lvl = parseLogLevel(mCfg.log.level);
    logger().setLevel(lvl);
    if (!mCfg.log.file.empty()) {
        logger().openFile(fsu::toUtf8(mCfg.resolve(mCfg.log.file)),
                          static_cast<size_t>(mCfg.log.maxSizeMB) << 20, mCfg.log.rotate);
    }

    std::error_code ec;
    fsu::ensureDir(mCfg.baseDir / ".mcpack-sync");

    auto loaded = mState.load();
    if (!loaded) {
        // 状态损坏：备份后重建，避免"永远认为已是最新"导致漏推
        auto bad = mCfg.statePath();
        auto bak = bad;
        bak += ".corrupt." + std::to_string(fsu::nowUnix());
        std::error_code rec;
        std::filesystem::rename(bad, bak, rec);
        logger().error("state file corrupt, backed up to " + fsu::toUtf8(bak) + " : " + loaded.error().str());
    } else if (loaded.value()) {
        logger().info("state loaded: " + std::to_string(mState.size()) + " pack(s) from " + fsu::toUtf8(mState.path()));
    } else {
        logger().info("state file not found, starting fresh: " + fsu::toUtf8(mState.path()));
    }

    auto cdnSt = mCdn.init();
    if (!cdnSt) logger().warn("cdn init: " + cdnSt.error().str());

    if (mCfg.git.enabled) {
        auto st = mGit.prepare();
        if (!st) {
            logger().error("git prepare failed: " + st.error().str());
            return st;
        }
    }
    return {};
}

PipelineReport Pipeline::scanOnly() {
    PipelineReport rep;
    rep.startedAt = fsu::nowIso8601();
    auto t0       = std::chrono::steady_clock::now();

    ScanOptions so;
    so.ignorePatterns  = mCfg.identity.ignorePatterns;
    so.requireManifest = mCfg.identity.requireManifest;
    so.matchModuleUuid = mCfg.identity.matchModuleUuid;

    auto scan = scanResourceDir(mCfg.resourcePath(), so);
    if (!scan) {
        rep.ok    = false;
        rep.error = scan.error().str();
        rep.elapsedMs = elapsedSince(t0);
        rep.finishedAt = fsu::nowIso8601();
        return rep;
    }
    auto const& sr = scan.value();
    rep.packsScanned = sr.packs.size();
    rep.filesHashed  = sr.filesHashed;
    rep.bytesRead    = sr.bytesRead;
    for (auto const& w : sr.warnings) rep.warnings.push_back(w);
    for (auto const& c : sr.conflicts) {
        rep.warnings.push_back("uuid conflict: " + c.reason);
    }

    DiffOptions dop;
    dop.matchModuleUuid = mCfg.identity.matchModuleUuid;
    auto diff           = diffSnapshots(sr.packs, mState, dop);
    rep.added           = diff.added;
    rep.updated         = diff.updated;
    rep.removed         = diff.removed;
    rep.unchanged       = diff.unchanged;

    for (auto const& ch : diff.changes) {
        PackOutcome o;
        o.uuid    = ch.id.uuid;
        o.slug    = ch.id.uuid.substr(0, 8);
        o.kind    = ch.kind;
        o.ok      = true;
        o.stage   = "diff";
        o.message = ch.reason;
        rep.outcomes.push_back(std::move(o));
    }

    rep.elapsedMs  = elapsedSince(t0);
    rep.finishedAt = fsu::nowIso8601();
    rep.ok         = true;
    return rep;
}

bool Pipeline::finishRelease(PackState& st, PackSnapshot const& snap, PipelineOptions const& opts, PackOutcome& out) {
    CdnTarget target;
    target.uuid      = snap.id.uuid;
    target.name      = snap.id.name;
    target.slug      = snap.slug();
    target.version   = versionToString(snap.id.version);
    target.tag       = renderPlaceholders(mCfg.release.tagTemplate, snap);
    target.assetName = renderPlaceholders(mCfg.release.assetNameTemplate, snap);
    target.updatedAt = fsu::nowIso8601();

    if (!mCfg.cdn.enabled) {
        st.release.tag         = target.tag;
        st.release.assetName   = target.assetName;
        st.release.publishedAt = target.updatedAt;
        st.releasePending      = false;
        out.tag                = target.tag;
        out.message            = "cdn disabled, release tracking skipped";
        return true;
    }

    if (mCfg.cdn.mode == "static") {
        // 静态托管：包由外部（对象存储 / 自建站 / GitHub Pages）提供，不在 GitHub Release 上，
        // 因此不轮询 Release API；地址 = cdn.staticUrl + "/" + assetName。
        // 镜像前缀是为 GitHub 域名设计的，套到自有域名上只会拼出错误地址，故这里不套镜像。
        target.assetUrl  = urlJoin(mCfg.cdn.staticUrl, target.assetName);
        target.mirror    = "static";
        target.publicUrl = target.assetUrl;

        auto cst = mCdn.apply(target, mCfg.github.owner, mCfg.github.repo);
        if (!cst) logger().warn("cdn apply: " + cst.error().str());
        else if (onCdnPublished && !mStop.load()) onCdnPublished();

        st.release.tag         = target.tag;
        st.release.assetName   = target.assetName;
        st.release.assetUrl    = target.assetUrl;
        st.release.publicUrl   = target.publicUrl;
        st.release.publishedAt = target.updatedAt;
        st.releasePending      = false;
        out.tag                = target.tag;
        out.cdnUrl             = target.publicUrl;
        out.message            = "cdn.mode=static, url composed from cdn.staticUrl";
        return true;
    }

    if (!mCfg.release.waitForRelease || opts.skipRelease) {
        // 不等 release API 时用 GitHub 资产 URL 规则直接拼接完整地址：
        // https://github.com/<owner>/<repo>/releases/download/<tag>/<asset>
        // workflow 创建的就是同名 tag/asset，因此地址稳定可预测（也用于 API 限流时的兜底）。
        target.assetUrl   = buildReleaseAssetUrl(mCfg.github.owner, mCfg.github.repo, target.tag, target.assetName);
        target.candidates = mMirror.resolve(target.assetUrl, mCfg.github.owner, mCfg.github.repo, target.tag,
                                            target.assetName, "packs/" + target.slug);
        auto sel         = mMirror.select(target.candidates);
        target.mirror    = sel.mirrorName;
        target.publicUrl = sel.url;

        auto cst = mCdn.apply(target, mCfg.github.owner, mCfg.github.repo);
        if (!cst) logger().warn("cdn apply: " + cst.error().str());
        else if (onCdnPublished && !mStop.load()) onCdnPublished();

        st.release.tag         = target.tag;
        st.release.assetName   = target.assetName;
        st.release.assetUrl    = target.assetUrl;
        st.release.publicUrl   = target.publicUrl;
        st.release.publishedAt = target.updatedAt;
        st.releasePending      = false;
        out.tag                = target.tag;
        out.cdnUrl             = target.publicUrl;
        out.message            = "release polling skipped, cdn url derived from tag";
        return true;
    }

    auto rel = mGh.waitForRelease(target.tag, mCfg.release.pollIntervalMs, mCfg.release.pollTimeoutMs, &mStop);
    if (!rel) {
        st.releasePending = true;
        out.stage         = "release";
        out.message       = rel.error().str() + " (commit " + st.pushed.commit.substr(0, 8) + " pushed; next run resumes here)";
        return false;
    }

    auto const& r  = rel.value();
    auto const* as = r.findAsset(mCfg.release.assetSuffix);
    if (!as && mCfg.release.requireAsset) {
        st.releasePending = true;
        out.stage         = "release-asset";
        out.message       = "release " + r.tagName + " has no asset ending with " + mCfg.release.assetSuffix;
        return false;
    }
    if (as) {
        target.assetName = as->name;
        target.assetUrl  = as->browserDownloadUrl;
        target.sizeBytes = as->size;
    }
    target.candidates = mMirror.resolve(target.assetUrl, mCfg.github.owner, mCfg.github.repo, r.tagName,
                                        target.assetName, "packs/" + target.slug);

    auto sel         = mMirror.select(target.candidates);
    target.mirror    = sel.mirrorName;
    target.publicUrl = sel.url;

    auto cst = mCdn.apply(target, mCfg.github.owner, mCfg.github.repo);
    if (!cst) logger().warn("cdn apply: " + cst.error().str());
    else if (onCdnPublished && !mStop.load()) onCdnPublished();

    out.tag    = r.tagName;
    out.cdnUrl = target.publicUrl;

    st.release.tag         = r.tagName;
    st.release.assetName   = target.assetName;
    st.release.assetUrl    = target.assetUrl;
    st.release.publicUrl   = target.publicUrl;
    st.release.publishedAt = r.publishedAt.empty() ? target.updatedAt : r.publishedAt;
    st.releasePending      = false;
    return true;
}

bool Pipeline::processChange(Change const& ch, PipelineOptions const& opts, PackOutcome& out) {
    auto t0  = std::chrono::steady_clock::now();
    out.kind = ch.kind;
    out.uuid = ch.id.uuid;

    // 需要源快照（Removed 除外）
    PackSnapshot snap;
    if (ch.kind != ChangeKind::Removed) {
        ScanOptions so;
        so.ignorePatterns  = mCfg.identity.ignorePatterns;
        so.requireManifest = mCfg.identity.requireManifest;
        auto s             = snapshotPack(ch.dir, so);
        if (!s) {
            out.ok      = false;
            out.stage   = "snapshot";
            out.message = s.error().str();
            return false;
        }
        snap     = std::move(s.value());
        out.slug = snap.slug();
    } else {
        auto const* prev = mState.get(ch.id.uuid);
        out.slug         = (prev && !prev->slug.empty()) ? prev->slug : fsu::slugify(ch.id.uuid.substr(0, 8));
    }

    // Removed：仅删除仓库目录 + 状态
    if (ch.kind == ChangeKind::Removed) {
        if (mCfg.git.enabled && !opts.dryRun) {
            auto r = mGit.removePack(ch.id.uuid, out.slug, "");
            if (!r) {
                out.ok      = false;
                out.stage   = "git-remove";
                out.message = r.error().str();
                return false;
            }
        }
        if (!opts.dryRun) mState.erase(ch.id.uuid);
        out.ok        = true;
        out.stage     = "remove";
        out.message   = "removed from repo";
        out.elapsedMs = elapsedSince(t0);
        return true;
    }

    if (opts.dryRun) {
        out.ok        = true;
        out.stage     = "dry-run";
        out.message   = ch.reason;
        out.elapsedMs = elapsedSince(t0);
        return true;
    }

    // tag/asset 名在写 index.json 之前确定，保证仓库索引与插件取回的名字一致
    std::string desiredTag   = renderPlaceholders(mCfg.release.tagTemplate, snap);
    std::string desiredAsset = renderPlaceholders(mCfg.release.assetNameTemplate, snap);

    std::string commitSha;
    if (mCfg.git.enabled) {
        std::vector<RepoIndexEntry> entries;
        for (auto const* ps : mState.all()) {
            if (ps->uuid == snap.id.uuid) continue;
            RepoIndexEntry e;
            e.uuid        = ps->uuid;
            e.moduleUuid  = ps->moduleUuid;
            e.name        = ps->name;
            e.slug        = ps->slug;
            e.version     = ps->version;
            e.fileSetHash = ps->fileSetHash;
            e.fingerprint = ps->fingerprint;
            e.fileCount   = ps->fileCount;
            e.totalBytes  = ps->totalBytes;
            e.updatedAt   = ps->lastScanAt;
            e.path        = "packs/" + ps->slug;
            e.tag         = ps->release.tag;
            e.assetName   = ps->release.assetName;
            entries.push_back(std::move(e));
        }
        auto idxEntry      = toIndexEntry(snap);
        idxEntry.tag       = desiredTag;
        idxEntry.assetName = desiredAsset;
        entries.push_back(std::move(idxEntry));

        auto ist = mGit.writeIndex(entries);
        if (!ist) {
            out.ok      = false;
            out.stage   = "index";
            out.message = ist.error().str();
            return false;
        }

        auto pub = mGit.publish(snap, "");
        if (!pub) {
            out.ok      = false;
            out.stage   = "git-push";
            out.message = pub.error().str();
            return false;
        }
        commitSha  = pub.value().commit;
        out.commit = commitSha;
        if (!pub.value().committed) out.message = "no content change, commit skipped";
    } else {
        out.message = "git disabled";
    }

    // 推送成功后立即落状态：即使后续 release 阶段失败，也不会重复提交/重复上传
    PackState st;
    st.uuid        = snap.id.uuid;
    st.moduleUuid  = snap.id.moduleUuid;
    st.name        = snap.id.name;
    st.slug        = snap.slug();
    st.version     = snap.id.version;
    st.fileSetHash = snap.fileSetHash;
    st.fingerprint = snap.fingerprint;
    st.fileCount   = snap.files.size();
    st.totalBytes  = snap.totalBytes;
    st.lastScanAt  = fsu::nowIso8601();
    st.pushed.commit    = commitSha;
    st.pushed.remote    = mCfg.git.remote;
    st.pushed.branch    = mCfg.git.branch;
    st.pushed.at        = st.lastScanAt;
    st.releasePending   = true;
    mState.put(st);
    if (auto s = mState.save(); !s) logger().warn("pipeline: state save (pre-release) failed: " + s.error().str());

    bool ok = finishRelease(*mState.get(snap.id.uuid), snap, opts, out);
    if (auto s = mState.save(); !s) logger().warn("pipeline: state save failed: " + s.error().str());

    out.ok        = ok;
    out.stage     = ok ? "done" : out.stage;
    out.elapsedMs = elapsedSince(t0);
    return ok;
}

PipelineReport Pipeline::runOnce(PipelineOptions opts) {
    std::lock_guard<std::mutex> runLock(mRunMtx);

    PipelineReport rep;
    rep.startedAt = fsu::nowIso8601();
    auto t0       = std::chrono::steady_clock::now();

    ScanOptions so;
    so.ignorePatterns  = mCfg.identity.ignorePatterns;
    so.requireManifest = mCfg.identity.requireManifest;
    so.matchModuleUuid = mCfg.identity.matchModuleUuid;

    logger().info("pipeline: scanning " + fsu::toUtf8(mCfg.resourcePath()));
    auto scan = scanResourceDir(mCfg.resourcePath(), so);
    if (!scan) {
        rep.ok         = false;
        rep.error      = scan.error().str();
        rep.elapsedMs  = elapsedSince(t0);
        rep.finishedAt = fsu::nowIso8601();
        logger().error("pipeline: scan failed: " + rep.error);
        return rep;
    }
    auto const& sr    = scan.value();
    rep.packsScanned = sr.packs.size();
    rep.filesHashed  = sr.filesHashed;
    rep.bytesRead    = sr.bytesRead;
    for (auto const& w : sr.warnings) rep.warnings.push_back(w);
    for (auto const& c : sr.conflicts) rep.warnings.push_back("uuid conflict: " + c.reason);
    logger().info("pipeline: " + std::to_string(sr.packs.size()) + " pack(s), " + std::to_string(sr.filesHashed) +
                  " file(s), " + std::to_string(sr.bytesRead / 1024) + " KiB, " + std::to_string(sr.elapsedMs) + "ms");

    DiffOptions dop;
    dop.matchModuleUuid = mCfg.identity.matchModuleUuid;
    auto diff           = diffSnapshots(sr.packs, mState, dop);
    rep.added           = diff.added;
    rep.updated         = diff.updated;
    rep.removed         = diff.removed;
    rep.unchanged       = diff.unchanged;

    // --force：忽略 unchanged，全量重新发布（用于修复历史误判/切换仓库）
    if (opts.force) {
        diff.pending.clear();
        for (auto const& snap : sr.packs) {
            Change c;
            c.kind           = ChangeKind::Updated;
            c.id             = snap.id;
            c.dir            = snap.dir;
            c.newVersion     = snap.id.version;
            c.newFingerprint = snap.fingerprint;
            c.reason         = "forced republish";
            diff.pending.push_back(std::move(c));
        }
        logger().info("pipeline: --force, republishing " + std::to_string(diff.pending.size()) + " pack(s)");
    }

    // 冲突策略
    if (!sr.conflicts.empty()) {
        if (mCfg.identity.conflictPolicy == "error") {
            rep.ok    = false;
            rep.error = "duplicate pack uuid detected: " + sr.conflicts.front().reason;
            rep.elapsedMs  = elapsedSince(t0);
            rep.finishedAt = fsu::nowIso8601();
            return rep;
        }
        logger().warn("pipeline: " + std::to_string(sr.conflicts.size()) +
                      " uuid conflict(s), policy=" + mCfg.identity.conflictPolicy);
    }

    std::set<std::string> handledThisRun;
    for (auto const& ch : diff.pending) {
        PackOutcome out;
        bool        ok = processChange(ch, opts, out);
        handledThisRun.insert(ch.id.uuid);
        if (!ok) {
            rep.ok = false;
            if (rep.error.empty()) rep.error = out.stage + ": " + out.message;
        }
        rep.outcomes.push_back(std::move(out));
    }

    // 恢复：上次推送成功但 release/CDN 未确认的包，只补发布阶段，绝不重复提交
    for (auto const* psc : mState.all()) {
        if (!psc->releasePending) continue;
        if (handledThisRun.count(psc->uuid)) continue; // 本轮刚失败过，不要在同一次运行里立刻重试
        auto it = std::find_if(sr.packs.begin(), sr.packs.end(),
                               [&](PackSnapshot const& x) { return x.id.uuid == psc->uuid; });
        if (it == sr.packs.end()) continue;

        PackOutcome out;
        out.uuid = psc->uuid;
        out.slug = psc->slug;
        out.kind = ChangeKind::Updated;
        bool ok  = finishRelease(*mState.get(psc->uuid), *it, opts, out);
        out.ok   = ok;
        if (!ok) {
            rep.ok = false;
            if (rep.error.empty()) rep.error = out.stage + ": " + out.message;
        }
        logger().info("pipeline: resumed release stage for " + out.slug + " -> " + (ok ? "ok" : out.stage));
        rep.outcomes.push_back(std::move(out));
        mState.save();
    }

    // 未变更的包也纳入审计输出
    for (auto const& ch : diff.changes) {
        if (ch.kind != ChangeKind::Unchanged) continue;
        PackOutcome out;
        out.uuid      = ch.id.uuid;
        out.slug      = mState.get(ch.id.uuid) ? mState.get(ch.id.uuid)->slug : ch.id.uuid.substr(0, 8);
        out.kind      = ChangeKind::Unchanged;
        out.ok        = true;
        out.stage     = "skip";
        out.message   = ch.reason;
        out.commit    = mState.get(ch.id.uuid) ? mState.get(ch.id.uuid)->pushed.commit : "";
        out.tag       = mState.get(ch.id.uuid) ? mState.get(ch.id.uuid)->release.tag : "";
        out.cdnUrl    = mState.get(ch.id.uuid) ? mState.get(ch.id.uuid)->release.publicUrl : "";
        rep.outcomes.push_back(std::move(out));
    }

    if (!opts.dryRun && mState.dirty()) {
        mState.setLastFullScan(rep.startedAt);
        auto st = mState.save();
        if (!st) {
            rep.ok = false;
            if (rep.error.empty()) rep.error = "state save: " + st.error().str();
            logger().error("pipeline: state save failed: " + st.error().str());
        }
    }

    rep.elapsedMs  = elapsedSince(t0);
    rep.finishedAt = fsu::nowIso8601();
    mRunCount.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(mReportMtx);
        mLastReport = std::make_unique<PipelineReport>(rep);
    }

    if (rep.ok) {
        logger().info("pipeline: done in " + std::to_string(rep.elapsedMs) + "ms");
    } else {
        logger().error("pipeline: finished with errors: " + rep.error);
    }
    return rep;
}

void Pipeline::requestScan() {
    mScanRequested.store(true);
    mCv.notify_all();
}

void Pipeline::startWatcher() {
    if (mWatching.exchange(true)) return;
    mStop.store(false);
    mWatchThread = std::thread([this] { watcherLoop(); });
    logger().info("pipeline: watcher started, interval=" + std::to_string(mCfg.server.scanIntervalSeconds) + "s");
}

void Pipeline::stopWatcher() {
    if (!mWatching.exchange(false)) return;
    mStop.store(true);
    mCv.notify_all();
    if (mWatchThread.joinable()) mWatchThread.join();
    logger().info("pipeline: watcher stopped");
}

bool Pipeline::stopWatcherWithTimeout(int timeoutMs) {
    if (!mWatching.exchange(false)) return true;
    mStop.store(true);
    mCv.notify_all();

    if (!mWatchThread.joinable()) return true;

    if (timeoutMs <= 0) {
        mWatchThread.join();
        logger().info("pipeline: watcher stopped");
        return true;
    }

    std::unique_lock<std::mutex> lk(mExitMtx);
    bool const exited = mExitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] {
        return mWatchExited.load();
    });
    lk.unlock();

    if (exited) {
        mWatchThread.join();
        logger().info("pipeline: watcher stopped within " + std::to_string(timeoutMs) + "ms budget");
        return true;
    }

    // 超时：交给调用方决定对象生命周期，这里只能 detach（否则 join 会永久阻塞停服）
    mWatchThread.detach();
    logger().warn(
        "pipeline: watcher still running after " + std::to_string(timeoutMs) +
        "ms, detached; it will finish in the background and the process is exiting anyway"
    );
    return false;
}

void Pipeline::watcherLoop() {
    struct ExitSignal {
        std::atomic<bool>&        flag;
        std::mutex&               mtx;
        std::condition_variable&  cv;
        ~ExitSignal() {
            {
                std::lock_guard<std::mutex> lk(mtx);
                flag.store(true);
            }
            cv.notify_all();
        }
    } exitSignal{mWatchExited, mExitMtx, mExitCv};

    if (mCfg.server.scanOnStart) {
        auto rep = runOnce();
        if (!rep.ok) logger().warn("initial scan reported errors: " + rep.error);
    }

    // scanIntervalSeconds == 0 表示「只在显式请求时扫描」。
    // 这里不能用 std::max(5, interval) 之类的兜底：那会把「关闭周期扫描」
    // 悄悄变成「每 5 秒扫一次」，在 pack 多的时候会让服务器持续吃 CPU。
    int const intervalSec = mCfg.server.scanIntervalSeconds;

    while (!mStop.load()) {
        bool requested = false;
        {
            std::unique_lock<std::mutex> lk(mCvMtx);
            auto pred = [this] { return mStop.load() || mScanRequested.load(); };
            if (intervalSec <= 0) {
                // 无限等待，直到有请求或要求停止
                mCv.wait(lk, pred);
            } else {
                // 超时也会醒来，走一次周期扫描
                mCv.wait_for(lk, std::chrono::seconds(intervalSec), pred);
            }
            requested = mScanRequested.exchange(false);
        }
        if (mStop.load()) break;

        // 未请求且没有周期扫描配置 → 只是被 stop 唤醒，回到循环顶部退出
        if (!requested && intervalSec <= 0) continue;

        if (requested && mCfg.server.debounceMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(mCfg.server.debounceMs));
            if (mStop.load()) break;
        }
        runOnce();
    }
}

PipelineReport const* Pipeline::lastReport() const {
    std::lock_guard<std::mutex> lk(mReportMtx);
    return mLastReport.get();
}

} // namespace mcpsync
