// mcpack-sync : 配置模型与加载/校验
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "mcpsync/json.hpp"
#include "mcpsync/result.hpp"

namespace mcpsync {

struct ServerConfig {
    int  scanIntervalSeconds{60};
    bool scanOnStart{true};
    bool scanOnWorldLoad{false};
    int  debounceMs{2000};
    int  shutdownTimeoutMs{10000};
};

struct IdentityConfig {
    bool                     requireManifest{true};
    std::string              conflictPolicy{"highest_version"}; // highest_version | error | newest_mtime
    bool                     matchModuleUuid{true};
    std::vector<std::string> ignorePatterns;
};

struct GitConfig {
    bool                     enabled{true};
    std::string              workDir{".mcpack-sync/repo"};
    std::string              remote;
    std::string              branch{"main"};
    // token_env      —— 从 tokenEnv 指向的环境变量取 PAT
    // token_inline   —— 直接用 token 字段（控制台 /mcpackgit token 写入这里并落盘）
    // credential_helper —— 交给系统 git 凭据管理器
    // ssh            —— 用 sshKey 指定的私钥
    std::string              authMode{"token_env"};
    std::string              tokenEnv{"MCPACK_GIT_TOKEN"};
    std::string              token;          // 明文 PAT（token_inline 模式）
    std::string              username;       // HTTPS 用户名，空则默认 x-access-token
    std::string              password;       // HTTPS 密码（与 username 配对，优先于 token）
    std::string              sshKey;         // SSH 私钥路径
    std::string              sshKeyEnv{"MCPACK_GIT_SSH_KEY"};
    std::string              authorName{"mcpack-sync"};
    std::string              authorEmail{"mcpack-sync@localhost"};
    std::string              commitTemplate{"chore(pack): {name} v{version} [{uuid8}]"};
    int                      pushRetries{3};
    int                      retryBackoffMs{2000};
    int                      timeoutMs{180000};
    std::string              subDir{"packs"};
    std::vector<std::string> extraArgs;
};

struct ReleaseConfig {
    bool        waitForRelease{true};
    int         pollIntervalMs{3000};
    int         pollTimeoutMs{300000};
    std::string tagTemplate{"{slug}-v{version}-{hash8}"};
    std::string assetNameTemplate{"{slug}-v{version}-{hash8}.zip"};
    std::string assetSuffix{".zip"};
    bool        requireAsset{true};
};

struct GithubConfig {
    std::string apiBase{"https://api.github.com"};
    std::string owner;
    std::string repo;
    std::string tokenEnv{"MCPACK_GH_TOKEN"};
    std::string userAgent{"mcpack-sync/1.0"};
    std::string proxy;
    int         timeoutMs{30000};
    int         maxRetries{3};
};

struct MirrorDef {
    std::string name;
    bool        enabled{true};
    int         priority{100};
    // 方式一：模板，支持 {url} {url_enc} {owner} {repo} {tag} {asset} {path}
    std::string urlTemplate;
    // 方式二：前缀式加速（ghfast / gh-proxy 这类），直接拼在原地址前
    // prefix="https://ghfast.top/" + url="https://github.com/o/r/releases/download/t/a"
    //   -> "https://ghfast.top/https://github.com/o/r/releases/download/t/a"
    // urlTemplate 非空时优先使用 urlTemplate。
    std::string prefix;
};

struct CdnSinkConfig {
    bool        enabled{false};
    std::string file;
    std::string key;
};

struct RedirectSinkConfig {
    bool        enabled{false};
    std::string bind{"0.0.0.0"};
    int         port{19134};
    std::string path{"/pack"};
};

struct CdnConfig {
    bool                 enabled{true};
    std::string          mode{"release_asset"};  // release_asset | mirror_template | static
    std::string          publicBaseUrl;
    std::string          staticUrl;
    std::vector<MirrorDef> mirrors;
    std::string          activeMirror{"auto"};
    bool                 writeConfigFile{true};
    std::string          manifestFile{".mcpack-sync/cdn.json"};
    CdnSinkConfig        serverProperties;
    // BDS 官方 cdn_config.json（1.26.x 起存在，日志里可见 "No CDN config file found"）
    CdnSinkConfig        bdsCdnConfig;
    RedirectSinkConfig   redirect;
};

struct LogConfig {
    std::string level{"info"};
    std::string file{"logs/mcpack-sync.log"};
    int         maxSizeMB{8};
    int         rotate{3};
};

struct Config {
    int             version{1};
    std::string     resourceDir{"resource_packs"};
    std::filesystem::path baseDir;     // 配置文件所在目录，用于解析相对路径
    std::filesystem::path configPath;
    ServerConfig    server;
    IdentityConfig  identity;
    GitConfig       git;
    ReleaseConfig   release;
    GithubConfig    github;
    CdnConfig       cdn;
    LogConfig       log;

    std::filesystem::path resolve(std::string const& p) const;
    std::filesystem::path resourcePath() const { return resolve(resourceDir); }
    std::filesystem::path statePath() const { return resolve(".mcpack-sync/state.json"); }
    std::filesystem::path repoPath() const { return resolve(git.workDir); }
};

// 从文件加载；不存在时返回 ErrCode::NotFound
Result<Config> loadConfig(std::filesystem::path const& path);
// 仅解析不落盘，供测试使用
Result<Config> parseConfig(std::string_view text, std::filesystem::path const& baseDir);
// 校验必填项（github.owner/repo、git.remote 等）
Status validateConfig(Config const& cfg);
// 生成默认配置文本
std::string defaultConfigJson();

// 环境变量取值（空字符串表示未设置）
std::string envOr(std::string const& name, std::string const& def = {});

} // namespace mcpsync
