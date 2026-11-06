#include "mcpsync/github.hpp"
#include "mcpsync/http.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"

#include <chrono>
#include <thread>

namespace mcpsync {

GithubClient::GithubClient(GithubConfig cfg) : mCfg(std::move(cfg)) {
    mToken = envOr(mCfg.tokenEnv, "");
    if (!mToken.empty()) {
        logger().debug("github: token loaded from env " + mCfg.tokenEnv);
    } else {
        logger().warn("github: env " + mCfg.tokenEnv + " is empty, using anonymous rate limit");
    }
}

ReleaseAsset const* Release::findAsset(std::string const& suffix) const {
    ReleaseAsset const* fallback = nullptr;
    for (auto const& a : assets) {
        if (suffix.empty()) {
            if (!fallback) fallback = &a;
            continue;
        }
        if (a.name.size() >= suffix.size() &&
            a.name.compare(a.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return &a;
        }
        if (!fallback) fallback = &a;
    }
    return fallback;
}

Result<Release> GithubClient::parseReleaseJson(std::string const& body) const {
    json::Value root;
    std::string err;
    if (!json::parse(body, root, err)) {
        return Error{ErrCode::Parse, "github release json invalid", err};
    }
    if (root.has("message") && !root.has("tag_name")) {
        return Error{ErrCode::Http, "github api message: " + root.at("message").asString(), body.substr(0, 300)};
    }
    Release r;
    r.tagName     = root.at("tag_name").asString();
    r.name        = root.at("name").asString();
    r.htmlUrl     = root.at("html_url").asString();
    r.publishedAt = root.at("published_at").asString();
    r.draft       = root.at("draft").asBool(false);
    r.prerelease  = root.at("prerelease").asBool(false);
    for (auto const& e : root.at("assets").asArray()) {
        ReleaseAsset as;
        as.name                 = e.at("name").asString();
        as.browserDownloadUrl   = e.at("browser_download_url").asString();
        as.size                 = static_cast<uint64_t>(e.at("size").asInt(0));
        as.updatedAt            = e.at("updated_at").asString();
        r.assets.push_back(std::move(as));
    }
    return r;
}

Result<Release> GithubClient::fetch(std::string const& url) {
    std::map<std::string, std::string> headers;
    headers["User-Agent"] = mCfg.userAgent;
    headers["Accept"]     = "application/vnd.github+json";
    if (!mToken.empty()) headers["Authorization"] = "Bearer " + mToken;

    auto r = httpGetJson(url, headers, mCfg.timeoutMs, mCfg.proxy, mCfg.maxRetries);
    if (!r) return r.error();
    return parseReleaseJson(r.value().body);
}

Result<Release> GithubClient::latestRelease() {
    if (mCfg.owner.empty() || mCfg.repo.empty()) {
        return Error{ErrCode::Config, "github.owner/repo not configured"};
    }
    std::string url = mCfg.apiBase + "/repos/" + mCfg.owner + "/" + mCfg.repo + "/releases/latest";
    return fetch(url);
}

Result<Release> GithubClient::releaseByTag(std::string const& tag) {
    if (mCfg.owner.empty() || mCfg.repo.empty()) {
        return Error{ErrCode::Config, "github.owner/repo not configured"};
    }
    std::string url = mCfg.apiBase + "/repos/" + mCfg.owner + "/" + mCfg.repo + "/releases/tags/" + urlEncode(tag);
    return fetch(url);
}

Result<Release> GithubClient::waitForRelease(std::string const& tag, int pollMs, int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    int  attempt = 0;
    std::string lastErr;

    for (;;) {
        ++attempt;
        auto r = releaseByTag(tag);
        if (r) {
            auto const& rel = r.value();
            if (!rel.draft) {
                logger().info("release " + tag + " is ready after " + std::to_string(attempt) + " poll(s)");
                return rel;
            }
            lastErr = "release exists but is draft";
        } else {
            lastErr = r.error().str();
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed >= timeoutMs) {
            return Error{ErrCode::Release,
                         "timeout waiting for release " + tag + " (" + std::to_string(timeoutMs) + "ms)",
                         lastErr, true};
        }
        logger().debug("waiting for release " + tag + " (attempt " + std::to_string(attempt) + "): " + lastErr);
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
}

} // namespace mcpsync
