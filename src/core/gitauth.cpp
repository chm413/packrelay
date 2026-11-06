#include "mcpsync/gitauth.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/http.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"
#include "mcpsync/process.hpp"

#include <chrono>

namespace mcpsync {

namespace {

std::string trimCopy(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

bool contains(std::string const& hay, char const* needle) {
    return hay.find(needle) != std::string::npos;
}

// 根据 git 输出给出可操作的修复建议
std::string diagnose(std::string const& out, GitConfig const& cfg) {
    if (contains(out, "Authentication failed") || contains(out, "Invalid username or password")
        || contains(out, "could not read Username") || contains(out, "terminal prompts disabled")) {
        return "凭据无效或缺失：用 /relaygit token <PAT> 设置令牌，或 /relaygit password <user> <pass>";
    }
    if (contains(out, "Permission denied (publickey)")) {
        return "SSH 公钥未被接受：确认 /relaygit sshkey <私钥路径> 指向的密钥已加到 GitHub 账号";
    }
    if (contains(out, "Host key verification failed")) {
        return "主机密钥未受信：先在服务器上 ssh-keyscan github.com >> ~/.ssh/known_hosts";
    }
    if (contains(out, "Repository not found") || contains(out, "404")) {
        return "仓库不存在或无权访问：核对 /relaygit remote <url> 与令牌的 repo 权限";
    }
    if (contains(out, "Could not resolve host") || contains(out, "Failed to connect")
        || contains(out, "Connection timed out") || contains(out, "unable to access")) {
        return "网络不可达：检查 DNS/代理；可先 /relaygit remote 确认地址，再重试";
    }
    if (contains(out, "not a git repository")) {
        return "工作目录不是 git 仓库：先运行一次扫描让它初始化";
    }
    if (cfg.remote.empty()) {
        return "尚未配置远端：/relaygit remote <url>";
    }
    return "连通性测试失败，详见上面的 git 输出";
}

std::string modeOrDefault(std::string const& m) {
    if (m == "token_env" || m == "token_inline" || m == "credential_helper" || m == "ssh") return m;
    return {};
}

} // namespace

std::string resolveGitToken(GitConfig const& cfg) {
    if (cfg.authMode == "credential_helper" || cfg.authMode == "ssh") return {};
    if (cfg.authMode == "token_inline") {
        if (!cfg.token.empty()) return cfg.token;
        return envOr(cfg.tokenEnv, "");
    }
    // token_env（默认）
    return envOr(cfg.tokenEnv, "");
}

std::string resolveGitSshKey(GitConfig const& cfg) {
    if (!cfg.sshKey.empty()) return cfg.sshKey;
    return envOr(cfg.sshKeyEnv, "");
}

bool hasGitCredential(GitConfig const& cfg) {
    if (cfg.authMode == "ssh") return !resolveGitSshKey(cfg).empty();
    if (cfg.authMode == "credential_helper") return true; // 交给系统，视为已配置
    if (!cfg.password.empty()) return true;
    return !resolveGitToken(cfg).empty();
}

std::string maskSecret(std::string const& s) {
    if (s.empty()) return "(unset)";
    if (s.size() <= 8) return "********";
    return s.substr(0, 4) + "****" + s.substr(s.size() - 4);
}

std::vector<std::string> buildAuthArgs(GitConfig const& cfg) {
    std::vector<std::string> out;

    if (cfg.authMode == "ssh") {
        std::string key = resolveGitSshKey(cfg);
        if (key.empty()) return out;
        // 用 core.sshCommand 而不是环境变量：fetch/push/pull 都自动继承，无需逐个改调用点
        std::string cmd = "ssh -i \"" + key + "\" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new";
        out.push_back("-c");
        out.push_back("core.sshCommand=" + cmd);
        return out;
    }

    if (cfg.authMode == "credential_helper") return out;

    std::string token = resolveGitToken(cfg);
    std::string user  = cfg.username.empty() ? "x-access-token" : cfg.username;
    std::string pass  = !cfg.password.empty() ? cfg.password : token;
    if (pass.empty()) return out;

    std::string basic = base64Encode(user + ":" + pass);
    out.push_back("-c");
    out.push_back("http.extraheader=AUTHORIZATION: basic " + basic);
    return out;
}

GitProbe probeGitRemote(GitConfig const& cfg, std::filesystem::path const& cwd, int timeoutMs) {
    GitProbe pr;

    if (cfg.remote.empty()) {
        pr.hint = "尚未配置远端：/relaygit remote <url>";
        return pr;
    }

    std::string dir = fsu::toUtf8(cwd);
    if (dir.empty() || !fsu::exists(cwd)) dir = ".";

    auto args = buildAuthArgs(cfg);
    args.insert(args.end(), {"ls-remote", "--heads", cfg.remote});

    auto t0 = std::chrono::steady_clock::now();
    auto r  = gitRun(dir, args, timeoutMs > 0 ? timeoutMs : 30000);
    pr.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
    );

    if (!r) {
        pr.hint = "无法启动 git：" + r.error().str();
        return pr;
    }

    pr.exitCode = r.value().exitCode;
    pr.out      = trimCopy(r.value().out);
    pr.ok       = (pr.exitCode == 0);
    if (!pr.ok) pr.hint = diagnose(pr.out, cfg);
    return pr;
}

Status patchConfigFileGit(std::filesystem::path const& path, GitConfig const& git) {
    json::Value root = json::Value::object();

    std::string text;
    auto        st = fsu::readFile(path, text);
    if (st) {
        std::string err;
        if (!json::parse(text, root, err)) {
            return Error{ErrCode::Parse, "config json invalid, refuse to overwrite", err};
        }
        if (!root.isObject()) {
            return Error{ErrCode::Parse, "config root must be an object"};
        }
    } else if (st.error().code != ErrCode::NotFound) {
        return st.error();
    }

    json::Value g = json::Value::object();
    g.set("enabled", json::Value(git.enabled));
    g.set("workDir", json::Value(git.workDir));
    g.set("remote", json::Value(git.remote));
    g.set("branch", json::Value(git.branch));
    g.set("authMode", json::Value(git.authMode));
    g.set("tokenEnv", json::Value(git.tokenEnv));
    g.set("token", json::Value(git.token));
    g.set("username", json::Value(git.username));
    g.set("password", json::Value(git.password));
    g.set("sshKey", json::Value(git.sshKey));
    g.set("sshKeyEnv", json::Value(git.sshKeyEnv));
    g.set("authorName", json::Value(git.authorName));
    g.set("authorEmail", json::Value(git.authorEmail));
    g.set("commitTemplate", json::Value(git.commitTemplate));
    g.set("pushRetries", json::Value(git.pushRetries));
    g.set("retryBackoffMs", json::Value(git.retryBackoffMs));
    g.set("timeoutMs", json::Value(git.timeoutMs));
    g.set("subDir", json::Value(git.subDir));

    json::Array extra;
    for (auto const& e : git.extraArgs) extra.push_back(json::Value(e));
    g.set("extraArgs", json::Value(std::move(extra)));

    root.set("git", json::Value(std::move(g)));

    auto wst = fsu::writeFileAtomic(path, root.dump(2) + "\n");
    if (wst) logger().info("config: git section saved to " + fsu::toUtf8(path));
    return wst;
}

Status applyGitField(GitConfig& cfg, GitField field, std::string const& a, std::string const& b) {
    switch (field) {
    case GitField::Remote:
        if (a.empty()) return Error{ErrCode::Config, "remote url is empty"};
        if (a.find("://") == std::string::npos && a.rfind("git@", 0) != 0) {
            return Error{ErrCode::Config, "remote must be an https:// or git@ url", a};
        }
        cfg.remote = a;
        return {};

    case GitField::Branch:
        if (a.empty()) return Error{ErrCode::Config, "branch is empty"};
        cfg.branch = a;
        return {};

    case GitField::Author:
        if (a.empty()) return Error{ErrCode::Config, "author name is empty"};
        if (b.empty()) return Error{ErrCode::Config, "author email is empty"};
        if (b.find('@') == std::string::npos) return Error{ErrCode::Config, "author email looks invalid", b};
        cfg.authorName  = a;
        cfg.authorEmail = b;
        return {};

    case GitField::Token:
        if (a.empty()) return Error{ErrCode::Config, "token is empty"};
        cfg.token    = a;
        // 控制台写入明文令牌后必须切到 token_inline，否则会被 token_env 分支忽略
        cfg.authMode = "token_inline";
        return {};

    case GitField::Password:
        if (a.empty()) return Error{ErrCode::Config, "username is empty"};
        if (b.empty()) return Error{ErrCode::Config, "password is empty"};
        cfg.username = a;
        cfg.password = b;
        cfg.authMode = "token_inline";
        return {};

    case GitField::SshKey:
        if (a.empty()) return Error{ErrCode::Config, "ssh key path is empty"};
        cfg.sshKey   = a;
        cfg.authMode = "ssh";
        return {};

    case GitField::AuthMode: {
        auto m = modeOrDefault(a);
        if (m.empty()) {
            return Error{ErrCode::Config, "unknown authMode, expected token_env|token_inline|credential_helper|ssh", a};
        }
        cfg.authMode = m;
        return {};
    }

    case GitField::Clear:
        cfg.token.clear();
        cfg.password.clear();
        cfg.username.clear();
        cfg.sshKey.clear();
        // 凭据清空后若还停在 ssh/token_inline，会变成「声明了却配不全」的矛盾状态，
        // 因此回到默认的 token_env（读环境变量，缺了也只是没凭据，不会卡校验）。
        cfg.authMode = "token_env";
        return {};
    }
    return Error{ErrCode::Internal, "unhandled git field"};
}

Status validateGitForConsole(GitConfig const& cfg) {
    auto m = modeOrDefault(cfg.authMode);
    if (m.empty()) {
        return Error{ErrCode::Config, "invalid git.authMode", cfg.authMode};
    }
    if (cfg.branch.empty()) return Error{ErrCode::Config, "git.branch is empty"};
    if (cfg.authMode == "ssh" && resolveGitSshKey(cfg).empty()) {
        return Error{ErrCode::Config, "authMode=ssh but no sshKey configured"};
    }
    if (cfg.authMode == "token_inline" && cfg.token.empty() && cfg.password.empty()) {
        return Error{ErrCode::Config, "authMode=token_inline but neither token nor password is set"};
    }
    return {};
}

std::vector<std::string> describeGitConfig(GitConfig const& cfg) {
    std::vector<std::string> out;
    out.push_back(std::string("enabled: ") + (cfg.enabled ? "true" : "false"));
    out.push_back("remote: " + (cfg.remote.empty() ? std::string("(unset)") : cfg.remote));
    out.push_back("branch: " + cfg.branch);
    out.push_back("authMode: " + cfg.authMode);
    out.push_back("author: " + cfg.authorName + " <" + cfg.authorEmail + ">");

    std::string token = resolveGitToken(cfg);
    out.push_back(
        "token: " + (token.empty() ? std::string("(unset)") : maskSecret(token)) +
        (cfg.authMode == "token_inline" ? " [inline]" : (cfg.authMode == "token_env" ? " [env " + cfg.tokenEnv + "]" : ""))
    );
    out.push_back("username: " + (cfg.username.empty() ? std::string("(unset, default x-access-token)") : cfg.username));
    out.push_back("password: " + (cfg.password.empty() ? std::string("(unset)") : maskSecret(cfg.password)));
    out.push_back("sshKey: " + (resolveGitSshKey(cfg).empty() ? std::string("(unset)") : resolveGitSshKey(cfg)));
    out.push_back(std::string("credential ready: ") + (hasGitCredential(cfg) ? "yes" : "no"));
    return out;
}

} // namespace mcpsync
