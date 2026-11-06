// mcpack-sync : 持久化状态（幂等与防重复推送的关键）
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "mcpsync/model.hpp"
#include "mcpsync/result.hpp"

namespace mcpsync {

struct PushedInfo {
    std::string commit;
    std::string remote;
    std::string branch;
    std::string at;
};

struct ReleaseInfo {
    std::string tag;
    std::string assetName;
    std::string assetUrl;      // 原始 GitHub 资产地址
    std::string publicUrl;     // 经镜像解析后的对外地址
    std::string publishedAt;
};

struct PackState {
    std::string uuid;
    std::string moduleUuid;
    std::string name;
    std::string slug;
    Version     version{0, 0, 0};
    std::string fileSetHash;
    std::string fingerprint;
    size_t      fileCount{0};
    uint64_t    totalBytes{0};
    std::string lastScanAt;
    PushedInfo  pushed;
    ReleaseInfo release;
    // 已推送但 release/CDN 尚未确认；下次运行只补这一步，不重复提交
    bool        releasePending{false};
};

class StateStore {
public:
    static constexpr int kFormatVersion = 1;

    explicit StateStore(std::filesystem::path path);

    std::filesystem::path const& path() const { return mPath; }

    Result<bool> load();                 // 文件不存在时返回 false 且视为空状态
    Status       save() const;           // 原子写

    PackState const* get(std::string const& uuid) const;
    PackState*       get(std::string const& uuid);
    void             put(PackState st);
    void             erase(std::string const& uuid);

    std::vector<PackState const*> all() const;
    size_t                        size() const { return mPacks.size(); }

    bool   dirty() const noexcept { return mDirty; }
    void   markDirty() noexcept { mDirty = true; }
    void   clearDirty() noexcept { mDirty = false; }
    void   setLastFullScan(std::string iso) { mLastFullScan = std::move(iso); }
    std::string const& lastFullScan() const { return mLastFullScan; }

private:
    std::filesystem::path                mPath;
    std::map<std::string, PackState>     mPacks;   // key = uuid（有序，保证序列化稳定）
    std::string                          mLastFullScan;
    bool                                 mDirty{false};
};

} // namespace mcpsync
