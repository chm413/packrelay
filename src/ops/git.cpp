#include "mcpsync/git.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/gitauth.hpp"
#include "mcpsync/http.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"
#include "mcpsync/process.hpp"
#include "mcpsync/repo_template.hpp"

#include <algorithm>
#include <thread>

namespace mcpsync {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) ++i;
    return s.substr(i);
}

std::string renderMessage(std::string tmpl, PackSnapshot const& snap) {
    std::string out = tmpl;
    auto        sub = [&](std::string const& k, std::string const& v) {
        size_t pos = 0;
        while ((pos = out.find(k, pos)) != std::string::npos) {
            out.replace(pos, k.size(), v);
            pos += v.size();
        }
    };
    sub("{name}", snap.id.name.empty() ? snap.slug() : snap.id.name);
    sub("{version}", versionToString(snap.id.version));
    sub("{uuid}", snap.id.uuid);
    sub("{uuid8}", snap.id.shortUuid());
    sub("{slug}", snap.slug());
    sub("{files}", std::to_string(snap.files.size()));
    return out;
}

bool looksLikeAuthError(std::string const& out) {
    return out.find("Authentication failed") != std::string::npos ||
           out.find("could not read Username") != std::string::npos ||
           out.find("Permission denied") != std::string::npos || out.find("403") != std::string::npos ||
           out.find("Invalid username or password") != std::string::npos;
}

} // namespace

GitPublisher::GitPublisher(GitConfig cfg, std::filesystem::path repoRoot)
: mCfg(std::move(cfg)), mRepo(std::move(repoRoot)) {
    auto g = findGit();
    if (g) mGitExe = g.value();
}

std::vector<std::string> GitPublisher::authArgs() const {
    // 凭据解析与 SSH 参数统一在 gitauth 里，控制台与流水线共用同一套规则
    return buildAuthArgs(mCfg);
}

Status GitPublisher::ensureRemote() {
    auto r = gitRun(fsu::toUtf8(mRepo), {"remote", "get-url", "origin"}, 15000);
    if (!r) return r.error();
    auto& pr = r.value();
    if (pr.exitCode != 0) {
        auto add = gitRun(fsu::toUtf8(mRepo), {"remote", "add", "origin", mCfg.remote}, 15000);
        if (!add) return add.error();
        if (add.value().exitCode != 0) {
            return Error{ErrCode::Git, "git remote add failed", add.value().out};
        }
        logger().info("git: added remote origin -> " + mCfg.remote);
        return {};
    }
    std::string current = trim(pr.out);
    if (current != mCfg.remote) {
        auto set = gitRun(fsu::toUtf8(mRepo), {"remote", "set-url", "origin", mCfg.remote}, 15000);
        if (set && set.value().exitCode == 0) {
            logger().info("git: updated remote origin " + current + " -> " + mCfg.remote);
        }
    }
    return {};
}

Status GitPublisher::prepare() {
    if (mPrepared) return {};
    if (mGitExe.empty()) return Error{ErrCode::Process, "git executable not found"};

    auto st = fsu::ensureDir(mRepo);
    if (!st) return st;

    bool hasGit = fsu::exists(mRepo / ".git");
    if (!hasGit) {
        auto init = gitRun(fsu::toUtf8(mRepo), {"init", "-q"}, mCfg.timeoutMs);
        if (!init) return init.error();
        if (init.value().exitCode != 0) {
            return Error{ErrCode::Git, "git init failed", init.value().out};
        }
        logger().info("git: initialized repo at " + fsu::toUtf8(mRepo));
    }

    auto st2 = ensureRemote();
    if (!st2) return st2;

    // 身份与行为（仅本仓库）
    gitRun(fsu::toUtf8(mRepo), {"config", "user.name", mCfg.authorName}, 15000);
    gitRun(fsu::toUtf8(mRepo), {"config", "user.email", mCfg.authorEmail}, 15000);
    gitRun(fsu::toUtf8(mRepo), {"config", "core.autocrlf", "false"}, 15000);
    gitRun(fsu::toUtf8(mRepo), {"config", "core.quotepath", "false"}, 15000);
    gitRun(fsu::toUtf8(mRepo), {"config", "gc.auto", "0"}, 15000);

    // 拉取远端分支（空仓库时允许失败）
    {
        auto args = authArgs();
        args.insert(args.end(), {"fetch", "--prune", "origin", mCfg.branch});
        auto f = gitRun(fsu::toUtf8(mRepo), args, mCfg.timeoutMs);
        if (f && f.value().exitCode == 0) {
            auto target   = "origin/" + mCfg.branch;
            bool onTarget = false;
            auto co       = gitRun(fsu::toUtf8(mRepo), {"checkout", "-B", mCfg.branch, target}, 30000);
            onTarget      = co && co.value().exitCode == 0;
            if (!onTarget) {
                // 上一轮中断（超时/崩溃）可能在镜像仓库里留下未提交改动，
                // 此时 checkout 会以 "Your local changes would be overwritten" 失败。
                // 镜像仓库内容每轮都由扫描结果重建，所以硬对齐远端是安全且幂等的。
                logger().warn("git: checkout failed, hard resetting mirror repo to " + target + " and retrying");
                auto rs = gitRun(fsu::toUtf8(mRepo), {"reset", "--hard", target}, 60000);
                if (!rs || rs.value().exitCode != 0) {
                    logger().warn("git: hard reset failed: " + (rs ? trim(rs.value().out) : rs.error().str()));
                }
                auto retry = gitRun(fsu::toUtf8(mRepo), {"checkout", "-B", mCfg.branch, target}, 30000);
                onTarget   = retry && retry.value().exitCode == 0;
            }
            if (!onTarget) {
                auto co2 = gitRun(fsu::toUtf8(mRepo), {"checkout", "-B", mCfg.branch}, 30000);
                if (!co2 || co2.value().exitCode != 0) {
                    return Error{
                        ErrCode::Git, "git checkout failed", co2 ? trim(co2.value().out) : std::string()
                    };
                }
            }
        } else {
            logger().debug("git: fetch origin/" + mCfg.branch + " unavailable (empty or unreachable), start local branch");
            auto co = gitRun(fsu::toUtf8(mRepo), {"checkout", "-B", mCfg.branch}, 30000);
            if (!co || co.value().exitCode != 0) {
                return Error{ErrCode::Git, "git checkout -B failed", co ? co.value().out : std::string()};
            }
        }
    }

    // 首次准备时把 workflow / 发布脚本铺进仓库，保证 Actions 链路开箱可用
    auto tpl = ensureRepoTemplates(mRepo);
    if (!tpl) {
        logger().warn("git: failed to write repo templates: " + tpl.error().str());
    }

    mPrepared = true;
    return {};
}

Result<PublishResult> GitPublisher::commitAndPush(std::string const& message) {
    PublishResult pr;
    pr.branch        = mCfg.branch;
    pr.commitMessage = message;

    // 1) 暂存
    {
        auto add = gitRun(fsu::toUtf8(mRepo), {"add", "-A"}, 120000);
        if (!add) return add.error();
        if (add.value().exitCode != 0) return Error{ErrCode::Git, "git add failed", add.value().out};
    }

    // 2) 是否有变化（幂等：无变化不产生空提交）
    {
        auto status = gitRun(fsu::toUtf8(mRepo), {"status", "--porcelain"}, 30000);
        if (!status) return status.error();
        if (trim(status.value().out).empty()) {
            auto head = gitRun(fsu::toUtf8(mRepo), {"rev-parse", "HEAD"}, 15000);
            pr.commit    = head ? trim(head.value().out) : "";
            pr.committed = false;
            logger().info("git: working tree clean, skip commit (idempotent)");
            return pr;
        }
    }

    // 3) 提交
    {
        auto commit = gitRun(fsu::toUtf8(mRepo), {"commit", "-q", "-m", message}, 60000);
        if (!commit) return commit.error();
        if (commit.value().exitCode != 0) {
            return Error{ErrCode::Git, "git commit failed", commit.value().out};
        }
        pr.committed = true;
    }

    {
        auto head = gitRun(fsu::toUtf8(mRepo), {"rev-parse", "HEAD"}, 15000);
        if (head && head.value().exitCode == 0) pr.commit = trim(head.value().out);
    }

    // 4) 推送（带重试 + 非快进自动 rebase 一次）
    int  attempt = 0;
    bool rebased = false;
    for (;;) {
        auto args = authArgs();
        args.insert(args.end(), {"push", "--porcelain", "origin", std::string("HEAD:") + mCfg.branch});
        auto p = gitRun(fsu::toUtf8(mRepo), args, mCfg.timeoutMs);
        if (!p) p = Result<ProcResult>(p.error());

        bool ok = p && p.value().exitCode == 0;
        if (ok) {
            pr.pushed = true;
            logger().info("git: pushed " + pr.commit.substr(0, 8) + " to origin/" + mCfg.branch);
            return pr;
        }

        std::string out  = p ? p.value().out : p.error().str();
        bool        auth = looksLikeAuthError(out);
        bool        nonFf = out.find("non-fast-forward") != std::string::npos ||
                     out.find("fetch first") != std::string::npos || out.find("rejected") != std::string::npos;

        if (nonFf && !rebased) {
            logger().warn("git: push rejected (non-fast-forward), trying pull --rebase once");
            auto rargs = authArgs();
            rargs.insert(rargs.end(), {"pull", "--rebase", "--autostash", "origin", mCfg.branch});
            auto rb = gitRun(fsu::toUtf8(mRepo), rargs, mCfg.timeoutMs);
            rebased = true;
            if (rb && rb.value().exitCode == 0) continue;
            return Error{ErrCode::Git, "git pull --rebase failed", rb ? rb.value().out : std::string()};
        }

        if (auth) {
            return Error{ErrCode::Git,
                         "git push authentication failed; set env " + mCfg.tokenEnv +
                             " (needs repo write scope) or use authMode=credential_helper",
                         out};
        }

        ++attempt;
        if (attempt > mCfg.pushRetries) {
            return Error{ErrCode::Git, "git push failed after retries", out, true};
        }
        int delay = mCfg.retryBackoffMs * attempt;
        logger().warn("git: push failed, retry " + std::to_string(attempt) + "/" + std::to_string(mCfg.pushRetries) +
                      " in " + std::to_string(delay) + "ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

Result<PublishResult> GitPublisher::publish(PackSnapshot const& snap, std::string const& message) {
    auto st = prepare();
    if (!st) return st.error();

    auto dest = mRepo / fsu::fromUtf8(mCfg.subDir) / fsu::fromUtf8(snap.slug());
    auto m    = fsu::mirrorTree(snap.dir, dest);
    if (!m) return m.error();

    logger().info("git: staged " + std::to_string(snap.files.size()) + " file(s) -> " +
                  fsu::toUtf8(dest.lexically_relative(mRepo)));
    return commitAndPush(message.empty() ? renderMessage(mCfg.commitTemplate, snap) : message);
}

Result<PublishResult> GitPublisher::removePack(std::string const& uuid, std::string const& slug, std::string const& msg) {
    auto st = prepare();
    if (!st) return st.error();

    auto dest = mRepo / fsu::fromUtf8(mCfg.subDir) / fsu::fromUtf8(slug);
    if (!fsu::exists(dest)) {
        logger().warn("git: pack dir not present, nothing to remove: " + fsu::toUtf8(dest));
        PublishResult pr;
        pr.committed = false;
        pr.pushed    = false;
        return pr;
    }
    auto rm = gitRun(fsu::toUtf8(mRepo), {"rm", "-r", "-q", "--ignore-unmatch", fsu::toUtf8(dest.lexically_relative(mRepo))}, 60000);
    if (rm && rm.value().exitCode != 0) {
        return Error{ErrCode::Git, "git rm failed", rm.value().out};
    }
    fsu::removeAll(dest);

    std::string message = msg;
    if (message.empty()) message = "chore(pack): remove " + slug + " [" + uuid.substr(0, 8) + "]";
    return commitAndPush(message);
}

Status GitPublisher::writeIndex(std::vector<RepoIndexEntry> const& entries) {
    json::Value root = json::Value::object();
    root.set("generatedBy", json::Value("mcpack-sync"));
    root.set("generatedAt", json::Value(fsu::nowIso8601()));
    json::Array arr;
    for (auto const& e : entries) {
        json::Value o = json::Value::object();
        o.set("uuid", json::Value(e.uuid));
        o.set("moduleUuid", json::Value(e.moduleUuid));
        o.set("name", json::Value(e.name));
        o.set("slug", json::Value(e.slug));
        json::Array v;
        v.emplace_back(e.version[0]);
        v.emplace_back(e.version[1]);
        v.emplace_back(e.version[2]);
        o.set("version", json::Value(std::move(v)));
        o.set("versionString", json::Value(versionToString(e.version)));
        o.set("fileSetHash", json::Value(e.fileSetHash));
        o.set("fingerprint", json::Value(e.fingerprint));
        o.set("fileCount", json::Value(static_cast<int64_t>(e.fileCount)));
        o.set("totalBytes", json::Value(static_cast<uint64_t>(e.totalBytes)));
        o.set("path", json::Value(e.path));
        o.set("tag", json::Value(e.tag));
        o.set("assetName", json::Value(e.assetName));
        o.set("updatedAt", json::Value(e.updatedAt));
        arr.push_back(std::move(o));
    }
    root.set("packs", json::Value(std::move(arr)));
    return fsu::writeFileAtomic(mRepo / "index.json", root.dump(2) + "\n");
}

} // namespace mcpsync
