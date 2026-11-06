// mcpack-sync : GitHub REST 客户端
#pragma once

#include <string>
#include <vector>

#include "mcpsync/config.hpp"
#include "mcpsync/result.hpp"

namespace mcpsync {

struct ReleaseAsset {
    std::string name;
    std::string browserDownloadUrl;
    uint64_t    size{0};
    std::string updatedAt;
};

struct Release {
    std::string              tagName;
    std::string              name;
    std::string              htmlUrl;
    std::string              publishedAt;
    bool                     draft{false};
    bool                     prerelease{false};
    std::vector<ReleaseAsset> assets;

    ReleaseAsset const* findAsset(std::string const& suffix) const;
};

class GithubClient {
public:
    explicit GithubClient(GithubConfig cfg);

    Result<Release> latestRelease();
    Result<Release> releaseByTag(std::string const& tag);
    // 轮询直到 tag 出现（workflow 需要几十秒）
    Result<Release> waitForRelease(std::string const& tag, int pollMs, int timeoutMs);

    bool hasToken() const { return !mToken.empty(); }

private:
    Result<Release> parseReleaseJson(std::string const& body) const;
    Result<Release> fetch(std::string const& url);

    GithubConfig mCfg;
    std::string  mToken;
};

} // namespace mcpsync
