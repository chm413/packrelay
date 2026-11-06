// mcpack-sync : CDN 地址分发
//
// GitHub 托管模式下，资源包的下载地址就是一条拼出来的字符串：
//   https://github.com/{owner}/{repo}/releases/download/{tag}/{asset}
// 需要加速时，在它前面直接加 gh 加速前缀（如 https://ghfast.top/）：
//   https://ghfast.top/https://github.com/{owner}/{repo}/releases/download/{tag}/{asset}
//
// 本模块负责把这条地址拼完整，并落到各个分发出口（cdn.json / cdn_config.json /
// server.properties / 内嵌 302）。per-pack 地址随每次发布更新，因此「始终指向最新 release」
// 等价于「每次发布重写一次完整地址」。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mcpsync/config.hpp"
#include "mcpsync/github.hpp"
#include "mcpsync/mirror.hpp"
#include "mcpsync/model.hpp"
#include "mcpsync/result.hpp"

namespace mcpsync {

struct CdnTarget {
    std::string                   uuid;
    std::string                   name;
    std::string                   slug;
    std::string                   version;        // 语义化版本，如 "1.0.3"（用于拼 BDS 资源包标识）
    std::string                   tag;
    std::string                   assetName;
    std::string                   assetUrl;
    std::string                   publicUrl;      // 最终对外地址（含镜像）
    std::string                   mirror;
    std::vector<MirrorCandidate>  candidates;
    uint64_t                      sizeBytes{0};
    std::string                   updatedAt;
};

// BDS 侧的资源包标识 -> 完整下载地址。
//
// LeviLamina/BDS 修改磁盘上的 cdn_config.json 不会生效（CDNConfig 只在启动早期读一次），
// 必须把地址写进内存里的 CDNConfig::mPackCDNUrls（vector<pair<packId, url>>）。
// 这里把「包身份 + 拼好的地址」整理成 mod 层可直接写入的形式。
struct PackCdnEntry {
    std::string packId;    // 主键：<uuid>_<version>
    std::string uuid;
    std::string version;
    std::string url;       // 完整外链，已含可选 gh 加速前缀
    std::string directUrl; // 直连地址
};

// 由当前发布目标生成待写入 BDS 内存的映射（主键 + uuid 别名，两者都写以兼容不同版本）。
std::vector<PackCdnEntry> buildPackCdnEntries(std::vector<CdnTarget> const& targets);

// 从 cdn.json 清单文本解析出同样的映射（mod 层读文件用）。
Status parsePackCdnEntries(std::string const& manifestJson, std::vector<PackCdnEntry>& out);

// 就地写入 BDS 的 CDNConfig::mPackCDNUrls 容器：
//   - 先移除本插件负责的键（packId 与 uuid 两种写法），保留运维手工加的其他条目；
//   - 再追加 packId 与 uuid 两条指向同一外链的记录，兼容不同 BDS 版本的查找方式。
// 幂等：重复调用不会让容器无限增长。
void upsertPackCdnUrls(
    std::vector<std::pair<std::string, std::string>>& urls,
    std::vector<PackCdnEntry> const&                  entries
);

class CdnManager {
public:
    CdnManager(CdnConfig cfg, std::filesystem::path baseDir);
    ~CdnManager();

    Status init();

    // 记录一次发布结果并刷新所有 sink
    Status apply(CdnTarget const& target, std::string const& owner, std::string const& repo);

    // 直接切换重定向目标（热更新，不落盘）
    void setRedirectTarget(std::string const& url);
    std::string redirectTarget() const;

    // 内嵌 302 服务
    Status startRedirectServer();
    void   stopRedirectServer();
    bool   redirectRunning() const { return mRunning.load(); }
    int    redirectPort() const { return mCfg.redirect.port; }
    std::string stableUrl() const;

    std::vector<CdnTarget> const& targets() const { return mTargets; }

    Status writeManifest() const;
    Status patchServerProperties() const;
    Status writeBdsCdnConfig() const;

private:
    void loadPersisted();
    void serveLoop();
    void handleClient(uintptr_t sock);

    CdnConfig              mCfg;
    std::filesystem::path  mBaseDir;
    std::vector<CdnTarget> mTargets;
    mutable std::mutex     mMtx;
    std::string            mRedirectTarget;
    std::string            mOwner;
    std::string            mRepo;

    std::thread       mThread;
    std::atomic<bool> mRunning{false};
    uintptr_t         mListenSocket{0};
};

} // namespace mcpsync
