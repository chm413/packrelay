// mcpack-sync : Git 发布器（提交 + 推送，幂等）
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mcpsync/config.hpp"
#include "mcpsync/model.hpp"
#include "mcpsync/result.hpp"

namespace mcpsync {

struct PublishResult {
    bool        committed{false};   // false 表示内容无变化，跳过提交
    bool        pushed{false};
    std::string commit;
    std::string commitMessage;
    std::string branch;
    std::string tag;
};

struct RepoIndexEntry {
    std::string uuid;
    std::string moduleUuid;
    std::string name;
    std::string slug;
    Version     version{0, 0, 0};
    std::string fileSetHash;
    std::string fingerprint;
    size_t      fileCount{0};
    uint64_t    totalBytes{0};
    std::string path;       // 仓库内相对路径 packs/<slug>
    std::string tag;        // 由 release.tagTemplate 预渲染，workflow 直接使用
    std::string assetName;
    std::string updatedAt;
};

class GitPublisher {
public:
    GitPublisher(GitConfig cfg, std::filesystem::path repoRoot);

    // 建立/校正工作副本与远端分支关系
    Status prepare();

    // 将资源包镜像到仓库 packs/<slug> 并提交推送
    Result<PublishResult> publish(PackSnapshot const& snap, std::string const& message);

    // 从仓库移除某个资源包（uuid 消失时）
    Result<PublishResult> removePack(std::string const& uuid, std::string const& slug, std::string const& message);

    // 维护仓库根 index.json（供 CDN 与 workflow 使用）
    Status writeIndex(std::vector<RepoIndexEntry> const& entries);

    std::filesystem::path const& repoRoot() const { return mRepo; }

private:
    Result<PublishResult> commitAndPush(std::string const& message);
    std::vector<std::string> authArgs() const;
    Status                   ensureRemote();

    GitConfig             mCfg;
    std::filesystem::path mRepo;
    std::string           mGitExe;
    bool                  mPrepared{false};
};

} // namespace mcpsync
