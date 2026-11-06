#include "mcpsync/config.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"

#include <cstdlib>

namespace mcpsync {

namespace {

std::string jstr(json::Value const& v, std::string const& key, std::string const& def) {
    auto const* p = v.find(key);
    return (p && p->isString()) ? p->asString() : def;
}

bool jbool(json::Value const& v, std::string const& key, bool def) {
    auto const* p = v.find(key);
    return (p && p->isBool()) ? p->asBool() : def;
}

int jint(json::Value const& v, std::string const& key, int def) {
    auto const* p = v.find(key);
    return (p && p->isNumber()) ? static_cast<int>(p->asInt()) : def;
}

std::vector<std::string> jstrArray(json::Value const& v, std::string const& key) {
    std::vector<std::string> out;
    auto const*              p = v.find(key);
    if (p && p->isArray()) {
        for (auto const& e : p->asArray()) {
            if (e.isString()) out.push_back(e.asString());
        }
    }
    return out;
}

} // namespace

std::string envOr(std::string const& name, std::string const& def) {
    if (name.empty()) return def;
    char const* v = std::getenv(name.c_str());
    return (v && *v) ? std::string(v) : def;
}

std::filesystem::path Config::resolve(std::string const& p) const {
    std::filesystem::path path = fsu::fromUtf8(p);
    if (path.is_absolute()) return path;
    return baseDir / path;
}

Result<Config> loadConfig(std::filesystem::path const& path) {
    std::string text;
    auto        st = fsu::readFile(path, text);
    if (!st) return st.error();
    return parseConfig(text, path.parent_path());
}

Result<Config> parseConfig(std::string_view text, std::filesystem::path const& baseDir) {
    json::Value root;
    std::string err;
    if (!json::parse(text, root, err)) {
        return Error{ErrCode::Parse, "config json invalid", err};
    }
    if (!root.isObject()) return Error{ErrCode::Parse, "config root must be an object"};

    Config cfg;
    cfg.baseDir = baseDir;
    if (root.has("baseDir")) {
        std::filesystem::path ov = fsu::fromUtf8(root.at("baseDir").asString());
        cfg.baseDir = ov.is_absolute() ? ov : (baseDir / ov);
    }
    cfg.version = jint(root, "version", 1);
    cfg.resourceDir = jstr(root, "resourceDir", cfg.resourceDir);

    if (auto const* s = root.find("server"); s && s->isObject()) {
        cfg.server.scanIntervalSeconds = jint(*s, "scanIntervalSeconds", cfg.server.scanIntervalSeconds);
        cfg.server.scanOnStart          = jbool(*s, "scanOnStart", cfg.server.scanOnStart);
        cfg.server.scanOnWorldLoad      = jbool(*s, "scanOnWorldLoad", cfg.server.scanOnWorldLoad);
        cfg.server.debounceMs           = jint(*s, "debounceMs", cfg.server.debounceMs);
        cfg.server.shutdownTimeoutMs    = jint(*s, "shutdownTimeoutMs", cfg.server.shutdownTimeoutMs);
    }

    if (auto const* s = root.find("identity"); s && s->isObject()) {
        cfg.identity.requireManifest  = jbool(*s, "requireManifest", cfg.identity.requireManifest);
        cfg.identity.conflictPolicy   = jstr(*s, "conflictPolicy", cfg.identity.conflictPolicy);
        cfg.identity.matchModuleUuid  = jbool(*s, "matchModuleUuid", cfg.identity.matchModuleUuid);
        cfg.identity.ignorePatterns   = jstrArray(*s, "ignorePatterns");
    }
    if (cfg.identity.ignorePatterns.empty()) {
        cfg.identity.ignorePatterns = {"**/.DS_Store", "**/Thumbs.db", "**/*.tmp", "**/~$*"};
    }

    if (auto const* s = root.find("git"); s && s->isObject()) {
        cfg.git.enabled        = jbool(*s, "enabled", cfg.git.enabled);
        cfg.git.workDir        = jstr(*s, "workDir", cfg.git.workDir);
        cfg.git.remote         = jstr(*s, "remote", cfg.git.remote);
        cfg.git.branch         = jstr(*s, "branch", cfg.git.branch);
        cfg.git.authMode       = jstr(*s, "authMode", cfg.git.authMode);
        cfg.git.tokenEnv       = jstr(*s, "tokenEnv", cfg.git.tokenEnv);
        cfg.git.token          = jstr(*s, "token", cfg.git.token);
        cfg.git.username       = jstr(*s, "username", cfg.git.username);
        cfg.git.password       = jstr(*s, "password", cfg.git.password);
        cfg.git.sshKey         = jstr(*s, "sshKey", cfg.git.sshKey);
        cfg.git.sshKeyEnv      = jstr(*s, "sshKeyEnv", cfg.git.sshKeyEnv);
        cfg.git.authorName     = jstr(*s, "authorName", cfg.git.authorName);
        cfg.git.authorEmail    = jstr(*s, "authorEmail", cfg.git.authorEmail);
        cfg.git.commitTemplate = jstr(*s, "commitTemplate", cfg.git.commitTemplate);
        cfg.git.pushRetries    = jint(*s, "pushRetries", cfg.git.pushRetries);
        cfg.git.retryBackoffMs = jint(*s, "retryBackoffMs", cfg.git.retryBackoffMs);
        cfg.git.timeoutMs      = jint(*s, "timeoutMs", cfg.git.timeoutMs);
        cfg.git.subDir         = jstr(*s, "subDir", cfg.git.subDir);
        cfg.git.extraArgs      = jstrArray(*s, "extraArgs");
    }

    if (auto const* s = root.find("release"); s && s->isObject()) {
        cfg.release.waitForRelease    = jbool(*s, "waitForRelease", cfg.release.waitForRelease);
        cfg.release.pollIntervalMs    = jint(*s, "pollIntervalMs", cfg.release.pollIntervalMs);
        cfg.release.pollTimeoutMs     = jint(*s, "pollTimeoutMs", cfg.release.pollTimeoutMs);
        cfg.release.tagTemplate       = jstr(*s, "tagTemplate", cfg.release.tagTemplate);
        cfg.release.assetNameTemplate = jstr(*s, "assetNameTemplate", cfg.release.assetNameTemplate);
        cfg.release.assetSuffix       = jstr(*s, "assetSuffix", cfg.release.assetSuffix);
        cfg.release.requireAsset      = jbool(*s, "requireAsset", cfg.release.requireAsset);
    }

    if (auto const* s = root.find("github"); s && s->isObject()) {
        cfg.github.apiBase    = jstr(*s, "apiBase", cfg.github.apiBase);
        cfg.github.owner      = jstr(*s, "owner", cfg.github.owner);
        cfg.github.repo       = jstr(*s, "repo", cfg.github.repo);
        cfg.github.tokenEnv   = jstr(*s, "tokenEnv", cfg.github.tokenEnv);
        cfg.github.userAgent  = jstr(*s, "userAgent", cfg.github.userAgent);
        cfg.github.proxy      = jstr(*s, "proxy", cfg.github.proxy);
        cfg.github.timeoutMs  = jint(*s, "timeoutMs", cfg.github.timeoutMs);
        cfg.github.maxRetries = jint(*s, "maxRetries", cfg.github.maxRetries);
    }

    if (auto const* s = root.find("cdn"); s && s->isObject()) {
        cfg.cdn.enabled        = jbool(*s, "enabled", cfg.cdn.enabled);
        cfg.cdn.mode           = jstr(*s, "mode", cfg.cdn.mode);
        cfg.cdn.publicBaseUrl  = jstr(*s, "publicBaseUrl", cfg.cdn.publicBaseUrl);
        cfg.cdn.staticUrl      = jstr(*s, "staticUrl", cfg.cdn.staticUrl);
        cfg.cdn.activeMirror   = jstr(*s, "activeMirror", cfg.cdn.activeMirror);
        cfg.cdn.writeConfigFile = jbool(*s, "writeManifest", cfg.cdn.writeConfigFile);
        cfg.cdn.manifestFile   = jstr(*s, "manifestFile", cfg.cdn.manifestFile);

        if (auto const* m = s->find("mirrors"); m && m->isArray()) {
            for (auto const& e : m->asArray()) {
                if (!e.isObject()) continue;
                MirrorDef d;
                d.name        = jstr(e, "name", "");
                d.enabled     = jbool(e, "enabled", true);
                d.priority    = jint(e, "priority", 100);
                d.prefix      = jstr(e, "prefix", "");
                // template 缺失且给了 prefix 时保持空，交给前缀拼接分支处理
                d.urlTemplate = jstr(e, "template", d.prefix.empty() ? "{url}" : "");
                if (!d.name.empty()) cfg.cdn.mirrors.push_back(std::move(d));
            }
        }
        if (cfg.cdn.mirrors.empty()) {
            cfg.cdn.mirrors = {
                {"direct", true, 0, "{url}", ""},
                {"ghfast", true, 10, "", "https://ghfast.top/"},
                {"gh-proxy", false, 20, "", "https://gh-proxy.com/"},
            };
        }

        if (auto const* sp = s->find("serverProperties"); sp && sp->isObject()) {
            cfg.cdn.serverProperties.enabled = jbool(*sp, "enabled", false);
            cfg.cdn.serverProperties.file    = jstr(*sp, "file", "server.properties");
            cfg.cdn.serverProperties.key     = jstr(*sp, "key", "mcpack-cdn-url");
        }
        if (auto const* bc = s->find("bdsCdnConfig"); bc && bc->isObject()) {
            cfg.cdn.bdsCdnConfig.enabled = jbool(*bc, "enabled", false);
            cfg.cdn.bdsCdnConfig.file    = jstr(*bc, "file", "cdn_config.json");
            cfg.cdn.bdsCdnConfig.key     = jstr(*bc, "key", "CDN URL");
        }
        if (auto const* rs = s->find("redirectServer"); rs && rs->isObject()) {
            cfg.cdn.redirect.enabled = jbool(*rs, "enabled", false);
            cfg.cdn.redirect.bind    = jstr(*rs, "bind", cfg.cdn.redirect.bind);
            cfg.cdn.redirect.port    = jint(*rs, "port", cfg.cdn.redirect.port);
            cfg.cdn.redirect.path    = jstr(*rs, "path", cfg.cdn.redirect.path);
        }
    }

    if (auto const* s = root.find("log"); s && s->isObject()) {
        cfg.log.level      = jstr(*s, "level", cfg.log.level);
        cfg.log.file       = jstr(*s, "file", cfg.log.file);
        cfg.log.maxSizeMB  = jint(*s, "maxSizeMB", cfg.log.maxSizeMB);
        cfg.log.rotate     = jint(*s, "rotate", cfg.log.rotate);
    }

    // 环境变量可覆盖敏感项，避免把 token 写进仓库
    cfg.github.owner = envOr("MCPACK_GH_OWNER", cfg.github.owner);
    cfg.github.repo  = envOr("MCPACK_GH_REPO", cfg.github.repo);

    return cfg;
}

Status validateConfig(Config const& cfg) {
    if (cfg.resourceDir.empty()) {
        return Error{ErrCode::Config, "resourceDir is empty"};
    }
    if (cfg.git.enabled) {
        if (cfg.git.remote.empty()) {
            return Error{ErrCode::Config, "git.remote is required when git.enabled=true"};
        }
        if (cfg.git.branch.empty()) {
            return Error{ErrCode::Config, "git.branch is empty"};
        }
    }
    if (cfg.cdn.mode == "static" && cfg.cdn.staticUrl.empty()) {
        return Error{ErrCode::Config, "cdn.mode=static requires cdn.staticUrl"};
    }
    if (cfg.identity.conflictPolicy != "highest_version" && cfg.identity.conflictPolicy != "error" &&
        cfg.identity.conflictPolicy != "newest_mtime") {
        return Error{ErrCode::Config, "identity.conflictPolicy must be highest_version|error|newest_mtime"};
    }
    return {};
}

std::string defaultConfigJson() {
    return R"({
  "version": 1,
  "resourceDir": "resource_packs",
  "server": {
    "scanIntervalSeconds": 60,
    "scanOnStart": true,
    "scanOnWorldLoad": false,
    "debounceMs": 2000,
    "shutdownTimeoutMs": 10000
  },
  "identity": {
    "requireManifest": true,
    "conflictPolicy": "highest_version",
    "matchModuleUuid": true,
    "ignorePatterns": ["**/.DS_Store", "**/Thumbs.db", "**/*.tmp", "**/~$*"]
  },
  "git": {
    "enabled": true,
    "workDir": ".mcpack-sync/repo",
    "remote": "https://github.com/OWNER/REPO.git",
    "branch": "main",
    "authMode": "token_env",
    "tokenEnv": "MCPACK_GIT_TOKEN",
    "token": "",
    "username": "",
    "password": "",
    "sshKey": "",
    "sshKeyEnv": "MCPACK_GIT_SSH_KEY",
    "authorName": "packrelay",
    "authorEmail": "packrelay@localhost",
    "commitTemplate": "chore(pack): {name} v{version} [{uuid8}]",
    "pushRetries": 3,
    "retryBackoffMs": 2000,
    "timeoutMs": 180000,
    "subDir": "packs",
    "extraArgs": []
  },
  "release": {
    "waitForRelease": true,
    "pollIntervalMs": 3000,
    "pollTimeoutMs": 300000,
    "tagTemplate": "{slug}-v{version}-{hash8}",
    "assetNameTemplate": "{slug}-v{version}-{hash8}.zip",
    "assetSuffix": ".zip",
    "requireAsset": true
  },
  "github": {
    "apiBase": "https://api.github.com",
    "owner": "OWNER",
    "repo": "REPO",
    "tokenEnv": "MCPACK_GH_TOKEN",
    "userAgent": "packrelay/1.0",
    "proxy": "",
    "timeoutMs": 30000,
    "maxRetries": 3
  },
  "cdn": {
    "enabled": true,
    "mode": "release_asset",
    "publicBaseUrl": "",
    "staticUrl": "",
    "activeMirror": "auto",
    "writeManifest": true,
    "manifestFile": ".mcpack-sync/cdn.json",
    "mirrors": [
      { "name": "direct", "enabled": true, "priority": 0, "template": "{url}" },
      { "name": "ghfast", "enabled": true, "priority": 10, "prefix": "https://ghfast.top/" },
      { "name": "gh-proxy", "enabled": false, "priority": 20, "prefix": "https://gh-proxy.com/" }
    ],
    "serverProperties": { "enabled": false, "file": "server.properties", "key": "mcpack-cdn-url" },
    "bdsCdnConfig": { "enabled": false, "file": "cdn_config.json", "key": "CDN URL" },
    "redirectServer": { "enabled": false, "bind": "0.0.0.0", "port": 19134, "path": "/pack" }
  },
  "log": { "level": "info", "file": "logs/packrelay.log", "maxSizeMB": 8, "rotate": 3 }
}
)";
}

} // namespace mcpsync
