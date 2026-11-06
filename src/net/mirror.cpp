#include "mcpsync/mirror.hpp"
#include "mcpsync/http.hpp"

#include <algorithm>

namespace mcpsync {

namespace {

void replaceAll(std::string& s, std::string const& from, std::string const& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

} // namespace

std::string buildReleaseAssetUrl(
    std::string const& owner,
    std::string const& repo,
    std::string const& tag,
    std::string const& assetName
) {
    return "https://github.com/" + owner + "/" + repo + "/releases/download/" + tag + "/" + assetName;
}

std::string concatUrlPrefix(std::string const& prefix, std::string const& url) {
    if (prefix.empty()) return url;
    if (url.empty()) return {};
    // 前缀统一收敛为「无尾斜杠 + 单个 '/'」，避免出现 https://ghfast.top//https://...
    std::string p = prefix;
    while (!p.empty() && p.back() == '/') p.pop_back();
    p.push_back('/');
    return p + url;
}

std::string renderMirrorTemplate(
    std::string const& tmpl,
    std::string const& url,
    std::string const& owner,
    std::string const& repo,
    std::string const& tag,
    std::string const& assetName,
    std::string const& repoPath
) {
    // 先替换 url_enc，避免被 {url} 抢先匹配
    std::string out = tmpl;
    replaceAll(out, "{url_enc}", urlEncode(url));
    replaceAll(out, "{url}", url);
    replaceAll(out, "{owner}", owner);
    replaceAll(out, "{repo}", repo);
    replaceAll(out, "{tag}", tag);
    replaceAll(out, "{asset}", assetName);
    replaceAll(out, "{path}", repoPath);
    return out;
}

MirrorResolver::MirrorResolver(std::vector<MirrorDef> defs, std::string active)
: mDefs(std::move(defs)), mActive(std::move(active)) {}

std::vector<MirrorCandidate> MirrorResolver::resolve(
    std::string const& assetUrl,
    std::string const& owner,
    std::string const& repo,
    std::string const& tag,
    std::string const& assetName,
    std::string const& repoPath
) const {
    std::vector<MirrorCandidate> out;
    for (auto const& d : mDefs) {
        if (!d.enabled) continue;
        MirrorCandidate c;
        c.name     = d.name;
        c.priority = d.priority;
        if (d.urlTemplate.empty() && !d.prefix.empty()) {
            // 前缀式加速：把完整原始地址直接拼在加速域名后面
            c.url = concatUrlPrefix(d.prefix, assetUrl);
        } else {
            std::string tmpl = d.urlTemplate.empty() ? "{url}" : d.urlTemplate;
            c.url = renderMirrorTemplate(tmpl, assetUrl, owner, repo, tag, assetName, repoPath);
        }
        if (c.url.empty()) continue;
        out.push_back(std::move(c));
    }
    std::stable_sort(out.begin(), out.end(), [](MirrorCandidate const& x, MirrorCandidate const& y) {
        return x.priority < y.priority;
    });
    return out;
}

MirrorResolver::Selected MirrorResolver::select(std::vector<MirrorCandidate> const& candidates) const {
    Selected s;
    if (candidates.empty()) return s;

    if (mActive != "auto") {
        for (auto const& c : candidates) {
            if (c.name == mActive) {
                s.mirrorName = c.name;
                s.url        = c.url;
                s.isDirect   = (c.name == "direct");
                return s;
            }
        }
        // 指定镜像不存在时退回 auto
    }
    auto const& first = candidates.front();
    s.mirrorName      = first.name;
    s.url             = first.url;
    s.isDirect        = (first.name == "direct");
    return s;
}

} // namespace mcpsync
