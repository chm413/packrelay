// packrelay CLI：既是独立守护进程，也是 mod 层与 CI 的验证入口
#include "mcpsync/cdn.hpp"
#include "mcpsync/config.hpp"
#include "mcpsync/diff.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/gitauth.hpp"
#include "mcpsync/logger.hpp"
#include "mcpsync/pipeline.hpp"
#include "mcpsync/scanner.hpp"
#include "mcpsync/state.hpp"
#include "mcpsync/zip.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <chrono>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

using namespace mcpsync;

void usage() {
    std::printf(
        "packrelay %s\n"
        "用法:\n"
        "  packrelay <command> [options]\n"
        "\n"
        "命令:\n"
        "  init-config     生成默认配置（--path <file>，默认 config/packrelay/config.json）\n"
        "  scan            仅扫描资源目录并与状态比对，不推送\n"
        "  once            执行一次完整流水线（扫描→推送→等 release→更新 CDN）\n"
        "  watch           常驻，按 server.scanIntervalSeconds 周期执行\n"
        "  redirect        仅启动 CDN 302 重定向服务（用于独立反代进程）\n"
        "  package         将目录打成 zip（--input <dir> --output <zip> [--json]）\n"
        "  dump-state      输出当前状态文件内容\n"
        "  cdn             查看/重渲染 CDN 落地文件（见下）\n"
        "  git             管理 git 凭据（见下）\n"
        "  version         打印版本\n"
        "\n"
        "cdn 子命令:\n"
        "  packrelay cdn                            列出当前发布记录与对外地址\n"
        "  packrelay cdn render                     按 cdn.json 重写 cdn_config.json / server.properties\n"
        "                                           （mod 改动磁盘文件不生效时必须走 BDS 内存，这里只做落地）\n"
        "\n"
        "git 子命令（修改后立即写回配置文件）:\n"
        "  packrelay git                            查看当前 git 配置（敏感项脱敏）\n"
        "  packrelay git remote <url>               设置远端\n"
        "  packrelay git branch <name>              设置分支\n"
        "  packrelay git user <name> <email>        设置提交作者\n"
        "  packrelay git token <PAT>                设置令牌（自动切 token_inline）\n"
        "  packrelay git password <user> <pass>     设置用户名密码\n"
        "  packrelay git sshkey <path>              设置 SSH 私钥（自动切 ssh）\n"
        "  packrelay git mode <mode>                token_env|token_inline|credential_helper|ssh\n"
        "  packrelay git clear                      清除 token/password/sshKey\n"
        "  packrelay git test                       连通性测试（git ls-remote）\n"
        "\n"
        "通用选项:\n"
        "  --config <file> 配置文件路径（默认 config/packrelay/config.json）\n"
        "  --dry-run       不写仓库/不推送\n"
        "  --force         忽略 unchanged 强制重新发布\n"
        "  --skip-release  不等 GitHub release（离线调试）\n"
        "  --quiet         只输出错误\n"
        "\n"
        "环境变量:\n"
        "  MCPACK_GIT_TOKEN / MCPACK_GH_TOKEN  GitHub 细粒度 token（repo 写权限）\n"
        "  MCPACK_GH_OWNER / MCPACK_GH_REPO    覆盖配置中的仓库坐标\n",
        MCPSYNC_VERSION
    );
}

struct Args {
    std::map<std::string, std::string> opts;
    std::string                        command;
    std::vector<std::string>           positional;
    bool                               dryRun{false};
    bool                               force{false};
    bool                               skipRelease{false};
    bool                               quiet{false};

    bool has(std::string const& k) const { return opts.count(k) > 0; }
    std::string get(std::string const& k, std::string const& def = {}) const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
    std::string arg(size_t i) const { return i < positional.size() ? positional[i] : std::string(); }
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--dry-run") {
            a.dryRun = true;
        } else if (s == "--force") {
            a.force = true;
        } else if (s == "--skip-release") {
            a.skipRelease = true;
        } else if (s == "--quiet") {
            a.quiet = true;
        } else if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2);
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                a.opts[key] = argv[++i];
            } else {
                a.opts[key] = "1";
            }
        } else if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

int cmdInitConfig(Args const& a) {
    auto path = fsu::fromUtf8(a.get("path", "config/packrelay/config.json"));
    if (fsu::exists(path)) {
        std::printf("配置已存在，跳过: %s\n", fsu::toUtf8(path).c_str());
        return 0;
    }
    auto st = fsu::writeFileAtomic(path, defaultConfigJson());
    if (!st) {
        std::printf("写入配置失败: %s\n", st.error().str().c_str());
        return 1;
    }
    std::printf("已生成默认配置: %s\n", fsu::toUtf8(path).c_str());
    return 0;
}

int cmdScan(Args const& a) {
    auto cfgPath = fsu::fromUtf8(a.get("config", "config/packrelay/config.json"));
    auto cfg     = loadConfig(cfgPath);
    if (!cfg) {
        std::printf("加载配置失败: %s\n", cfg.error().str().c_str());
        return 2;
    }
    Pipeline p(cfg.value());
    auto     st = p.init();
    if (!st) {
        std::printf("初始化失败: %s\n", st.error().str().c_str());
        return 3;
    }
    auto rep = p.scanOnly();
    std::printf("%s\n", rep.summary().c_str());
    return rep.ok ? 0 : 4;
}

int cmdCdn(Args const& a) {
    auto cfgPath = fsu::fromUtf8(a.get("config", "config/packrelay/config.json"));
    auto cfg     = loadConfig(cfgPath);
    if (!cfg) {
        std::printf("加载配置失败: %s\n", cfg.error().str().c_str());
        return 2;
    }
    // 目录基准：mod 层用服务器根目录；命令行默认用当前目录，可用 --dir 指定
    std::filesystem::path dir = fsu::fromUtf8(a.get("dir", "."));
    cfg.value().baseDir       = dir;

    if (!cfg.value().cdn.enabled) {
        std::printf("警告: cdn.enabled=false，仍按当前清单渲染落地文件\n");
    }
    // render 是离线操作，不要顺手把 302 服务拉起来（必须在构造 CdnManager 前改）
    cfg.value().cdn.redirect.enabled = false;
    CdnManager cdn(cfg.value().cdn, dir);
    cdn.init();

    auto const& targets = cdn.targets();
    if (targets.empty()) {
        std::printf(
            "没有可渲染的发布记录（%s）\n",
            fsu::toUtf8(dir / fsu::fromUtf8(cfg.value().cdn.manifestFile)).c_str()
        );
        return 4;
    }

    auto sub = a.arg(0);
    if (sub.empty() || sub == "show") {
        std::printf("manifest: %s\n", fsu::toUtf8(dir / fsu::fromUtf8(cfg.value().cdn.manifestFile)).c_str());
        std::printf("stable  : %s\n", cdn.stableUrl().c_str());
        for (auto const& t : targets) {
            std::printf("  %s v%s (%s)\n    %s\n", t.uuid.c_str(), t.version.c_str(), t.mirror.c_str(),
                        (t.publicUrl.empty() ? t.assetUrl : t.publicUrl).c_str());
        }
        if (sub.empty()) std::printf("\n提示: packrelay cdn render 可把这些地址重新落地到磁盘\n");
        return 0;
    }
    if (sub != "render") {
        std::printf("未知 cdn 子命令: %s（可用: show|render）\n", sub.c_str());
        return 1;
    }

    int rc = 0;
    auto s1 = cdn.writeManifest();
    if (!s1) {
        std::printf("写 %s 失败: %s\n", cfg.value().cdn.manifestFile.c_str(), s1.error().str().c_str());
        rc = 5;
    } else {
        std::printf("已写入 %s\n", cfg.value().cdn.manifestFile.c_str());
    }
    if (cfg.value().cdn.bdsCdnConfig.enabled) {
        auto s2 = cdn.writeBdsCdnConfig();
        if (!s2) {
            std::printf("写 %s 失败: %s\n", cfg.value().cdn.bdsCdnConfig.file.c_str(), s2.error().str().c_str());
            rc = 5;
        } else {
            std::printf("已写入 %s（%zu 个资源包）\n", cfg.value().cdn.bdsCdnConfig.file.c_str(), targets.size());
        }
    }
    if (cfg.value().cdn.serverProperties.enabled) {
        auto s3 = cdn.patchServerProperties();
        if (!s3) {
            std::printf("写 %s 失败: %s\n", cfg.value().cdn.serverProperties.file.c_str(), s3.error().str().c_str());
            rc = 5;
        } else {
            std::printf("已更新 %s\n", cfg.value().cdn.serverProperties.file.c_str());
        }
    }
    return rc;
}

int cmdOnce(Args const& a) {
    auto cfgPath = fsu::fromUtf8(a.get("config", "config/packrelay/config.json"));
    auto cfg     = loadConfig(cfgPath);
    if (!cfg) {
        std::printf("加载配置失败: %s\n", cfg.error().str().c_str());
        return 2;
    }
    auto v = validateConfig(cfg.value());
    if (!v) {
        std::printf("配置校验失败: %s\n", v.error().str().c_str());
        return 2;
    }
    Pipeline p(cfg.value());
    auto     st = p.init();
    if (!st) {
        std::printf("初始化失败: %s\n", st.error().str().c_str());
        return 3;
    }
    PipelineOptions opts;
    opts.dryRun      = a.dryRun;
    opts.force       = a.force;
    opts.skipRelease = a.skipRelease;
    auto rep         = p.runOnce(opts);
    std::printf("%s\n", rep.summary().c_str());
    return rep.ok ? 0 : 4;
}

int cmdWatch(Args const& a) {
    auto cfgPath = fsu::fromUtf8(a.get("config", "config/packrelay/config.json"));
    auto cfg     = loadConfig(cfgPath);
    if (!cfg) {
        std::printf("加载配置失败: %s\n", cfg.error().str().c_str());
        return 2;
    }
    Pipeline p(cfg.value());
    auto     st = p.init();
    if (!st) {
        std::printf("初始化失败: %s\n", st.error().str().c_str());
        return 3;
    }
    p.startWatcher();
    std::printf("watcher 运行中，Ctrl+C 退出\n");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int cmdRedirect(Args const& a) {
    auto cfgPath = fsu::fromUtf8(a.get("config", "config/packrelay/config.json"));
    auto cfg     = loadConfig(cfgPath);
    if (!cfg) {
        std::printf("加载配置失败: %s\n", cfg.error().str().c_str());
        return 2;
    }
    CdnManager cdn(cfg.value().cdn, cfg.value().baseDir);
    if (!cfg.value().cdn.redirect.enabled) {
        std::printf("cdn.redirectServer.enabled=false，无需启动\n");
        return 1;
    }
    auto st = cdn.init();
    if (!st) {
        std::printf("启动失败: %s\n", st.error().str().c_str());
        return 3;
    }
    std::printf("redirect 服务: %s\n", cdn.stableUrl().c_str());
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

int cmdPackage(Args const& a) {
    auto input  = fsu::fromUtf8(a.get("input", ""));
    auto output = fsu::fromUtf8(a.get("output", ""));
    if (input.empty() || output.empty()) {
        std::printf("用法: packrelay package --input <dir> --output <zip>\n");
        return 1;
    }
    ZipResult zr;
    auto      st = zipDirectory(input, output, &zr);
    if (!st) {
        std::printf("打包失败: %s\n", st.error().str().c_str());
        return 1;
    }
    if (a.has("json")) {
        std::printf("{\"zip\":\"%s\",\"bytes\":%llu,\"entries\":%zu,\"sha256\":\"%s\"}\n",
                    fsu::toUtf8(output).c_str(), static_cast<unsigned long long>(zr.bytes), zr.entries,
                    zr.sha256.c_str());
    } else {
        std::printf("zip=%s bytes=%llu entries=%zu sha256=%s\n", fsu::toUtf8(output).c_str(),
                    static_cast<unsigned long long>(zr.bytes), zr.entries, zr.sha256.c_str());
    }
    return 0;
}

int cmdDumpState(Args const& a) {
    auto cfgPath = fsu::fromUtf8(a.get("config", "config/packrelay/config.json"));
    auto cfg     = loadConfig(cfgPath);
    if (!cfg) {
        std::printf("加载配置失败: %s\n", cfg.error().str().c_str());
        return 2;
    }
    std::string text;
    auto        st = fsu::readFile(cfg.value().statePath(), text);
    if (!st) {
        std::printf("状态文件不存在: %s\n", fsu::toUtf8(cfg.value().statePath()).c_str());
        return 1;
    }
    std::printf("%s\n", text.c_str());
    return 0;
}

int cmdGit(Args const& a) {
    auto cfgPath = fsu::fromUtf8(a.get("config", "config/packrelay/config.json"));
    auto cfg     = loadConfig(cfgPath);
    if (!cfg) {
        std::printf("加载配置失败: %s\n", cfg.error().str().c_str());
        return 2;
    }

    GitConfig git = cfg.value().git;

    // 无子命令 = 查看；与游戏内 /relaygit 行为一致
    if (a.positional.empty()) {
        std::printf("配置文件: %s\n", fsu::toUtf8(cfgPath).c_str());
        for (auto const& line : describeGitConfig(git)) std::printf("  %s\n", line.c_str());
        return 0;
    }

    std::string sub = a.positional[0];

    if (sub == "test") {
        if (git.remote.empty()) {
            std::printf("git.remote 为空，先执行: packrelay git remote <url>\n");
            return 1;
        }
        std::printf("正在探测 %s (auth=%s) ...\n", git.remote.c_str(), git.authMode.c_str());
        auto pr = probeGitRemote(git, cfg.value().baseDir, 20000);
        if (pr.ok) {
            std::printf("OK: 远端可达，耗时 %dms\n", pr.elapsedMs);
            if (!pr.out.empty()) std::printf("refs:\n%s\n", pr.out.c_str());
            return 0;
        }
        std::printf("失败: exit=%d 耗时=%dms\n", pr.exitCode, pr.elapsedMs);
        if (!pr.out.empty()) std::printf("git 输出:\n%s\n", pr.out.c_str());
        std::printf("建议: %s\n", pr.hint.c_str());
        return 1;
    }

    GitField    field;
    std::string x;
    std::string y;

    if (sub == "remote") {
        field = GitField::Remote;
        x     = a.arg(1);
    } else if (sub == "branch") {
        field = GitField::Branch;
        x     = a.arg(1);
    } else if (sub == "user") {
        field = GitField::Author;
        x     = a.arg(1);
        y     = a.arg(2);
    } else if (sub == "token") {
        field = GitField::Token;
        x     = a.arg(1);
    } else if (sub == "password") {
        field = GitField::Password;
        x     = a.arg(1);
        y     = a.arg(2);
    } else if (sub == "sshkey") {
        field = GitField::SshKey;
        x     = a.arg(1);
    } else if (sub == "mode") {
        field = GitField::AuthMode;
        x     = a.arg(1);
    } else if (sub == "clear") {
        field = GitField::Clear;
    } else {
        std::printf("未知子命令: %s\n", sub.c_str());
        std::printf("可用: remote|branch|user|token|password|sshkey|mode|clear|test\n");
        return 1;
    }

    auto st = applyGitField(git, field, x, y);
    if (!st) {
        std::printf("设置失败: %s\n", st.error().str().c_str());
        return 1;
    }
    auto vs = validateGitForConsole(git);
    if (!vs) {
        std::printf("校验失败: %s\n", vs.error().str().c_str());
        return 1;
    }
    auto saved = patchConfigFileGit(cfgPath, git);
    if (!saved) {
        std::printf("保存失败: %s\n", saved.error().str().c_str());
        return 1;
    }

    std::printf("已保存到 %s\n", fsu::toUtf8(cfgPath).c_str());
    for (auto const& line : describeGitConfig(git)) std::printf("  %s\n", line.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    if (argc < 2) {
        usage();
        return 0;
    }
    std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        usage();
        return 0;
    }

    Args a = parseArgs(argc, argv);
    if (!a.quiet) a.opts["quiet"] = "";

    if (a.command == "version" || command == "--version") {
        std::printf("packrelay %s\n", MCPSYNC_VERSION);
        return 0;
    }
    if (command == "init-config") return cmdInitConfig(a);
    if (command == "scan") return cmdScan(a);
    if (command == "once") return cmdOnce(a);
    if (command == "watch") return cmdWatch(a);
    if (command == "redirect") return cmdRedirect(a);
    if (command == "dump-state") return cmdDumpState(a);
    if (command == "package") return cmdPackage(a);
    if (command == "git") return cmdGit(a);
    if (command == "cdn") return cmdCdn(a);

    std::printf("未知命令: %s\n\n", command.c_str());
    usage();
    return 1;
}
