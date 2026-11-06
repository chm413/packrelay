// mcpack-sync : 下载加速镜像解析
#pragma once

#include <string>
#include <vector>

#include "mcpsync/config.hpp"
#include "mcpsync/github.hpp"

namespace mcpsync {

struct MirrorCandidate {
    std::string name;
    int         priority{100};
    std::string url;
};

class MirrorResolver {
public:
    MirrorResolver(std::vector<MirrorDef> defs, std::string active); // active: "auto" 或镜像名

    // 按优先级生成候选地址（含原始 GitHub 直连，若已启用）
    std::vector<MirrorCandidate> resolve(
        std::string const& assetUrl,
        std::string const& owner,
        std::string const& repo,
        std::string const& tag,
        std::string const& assetName,
        std::string const& repoPath
    ) const;

    // 选定对外地址：active 指定优先；auto 时取优先级最高的启用项
    struct Selected {
        std::string mirrorName;
        std::string url;
        bool        isDirect{false};
    };
    Selected select(std::vector<MirrorCandidate> const& candidates) const;

    std::vector<MirrorDef> const& defs() const { return mDefs; }

private:
    std::vector<MirrorDef> mDefs;
    std::string            mActive;
};

// GitHub release 资产的完整原始地址：
//   https://github.com/{owner}/{repo}/releases/download/{tag}/{asset}
// 这是 GitHub 托管模式下的下载地址基线，后续加速都在它之上拼接。
std::string buildReleaseAssetUrl(
    std::string const& owner,
    std::string const& repo,
    std::string const& tag,
    std::string const& assetName
);

// 前缀式加速拼接：prefix 直接贴在 url 前面，自动补齐单个 '/'，不对 url 做任何编码。
//   concatUrlPrefix("https://ghfast.top/", "https://github.com/a/b") -> "https://ghfast.top/https://github.com/a/b"
std::string concatUrlPrefix(std::string const& prefix, std::string const& url);

// 模板渲染：{url} 原始地址，{url_enc} URL 编码，{owner}/{repo}/{tag}/{asset}/{path}
std::string renderMirrorTemplate(
    std::string const& tmpl,
    std::string const& url,
    std::string const& owner,
    std::string const& repo,
    std::string const& tag,
    std::string const& assetName,
    std::string const& repoPath
);

} // namespace mcpsync
