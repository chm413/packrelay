// mcpack-sync : Git 凭据管理与连通性测试
//
// 控制台指令需要能在运行期改 git 用户/密码/密钥，并把结果落回配置文件。
// 所有可测逻辑都放这里，mod 层只做「解析命令参数 -> 调用本模块 -> 回显」。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mcpsync/config.hpp"
#include "mcpsync/result.hpp"

namespace mcpsync {

// ---- 凭据解析 ----

// 实际生效的 token：authMode=token_inline 用 git.token，token_env 读环境变量，
// credential_helper/ssh 不返回 token。
std::string resolveGitToken(GitConfig const& cfg);

// 实际生效的 SSH 私钥路径：git.sshKey 优先，其次 sshKeyEnv 指向的环境变量。
std::string resolveGitSshKey(GitConfig const& cfg);

// 是否已配置任何可用凭据（token / 密码 / SSH 私钥）。
bool hasGitCredential(GitConfig const& cfg);

// 脱敏：<=8 字符整体打星，否则保留首尾各 4 位，中间固定 4 个星号。
std::string maskSecret(std::string const& s);

// ---- git 认证参数 ----

// 构造 `git -c ...` 前缀参数，供 fetch/push 与连通性测试共用：
//   HTTPS —— 写 http.extraheader，避免 token 落到 .git/config 与 reflog
//   SSH   —— 写 core.sshCommand，指定私钥且跳过交互
std::vector<std::string> buildAuthArgs(GitConfig const& cfg);

// ---- 连通性测试 ----

struct GitProbe {
    bool        ok{false};
    int         exitCode{-1};
    int         elapsedMs{0};
    std::string out;   // git 输出（已裁剪）
    std::string hint;  // 失败时的修复建议
};

// 执行 `git ls-remote --heads <remote>`：同时验证网络可达与凭据可用。
GitProbe probeGitRemote(GitConfig const& cfg, std::filesystem::path const& cwd, int timeoutMs);

// ---- 配置持久化 ----

// 只重写 config.json 的 git 段，保留其他字段；文件不存在时新建。
Status patchConfigFileGit(std::filesystem::path const& path, GitConfig const& git);

// ---- 控制台修改 ----

enum class GitField {
    Remote,    // a = remote url
    Branch,    // a = branch
    Author,    // a = name, b = email
    Token,     // a = token（自动切到 token_inline）
    Password,  // a = username, b = password
    SshKey,    // a = 私钥路径（自动切到 ssh）
    AuthMode,  // a = token_env|token_inline|credential_helper|ssh
    Clear,     // 清空 token / password / username / sshKey
};

// 把一次控制台修改应用到内存配置，返回非法输入的错误。
Status applyGitField(GitConfig& cfg, GitField field, std::string const& a, std::string const& b = {});

// 控制台侧校验（比 validateConfig 宽松：允许 remote 为空，只检查明显矛盾）
Status validateGitForConsole(GitConfig const& cfg);

// 一行摘要，用于 /mcpackgit 状态回显（敏感项已脱敏）。
std::vector<std::string> describeGitConfig(GitConfig const& cfg);

} // namespace mcpsync
