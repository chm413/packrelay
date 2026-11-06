// packrelay : LeviLamina 适配层
//
// 对外名称：packrelay（mod id / 指令前缀 / 日志前缀 / 仓库名）
// 内部标识：C++ namespace mcpsync、include/mcpsync/*.hpp、状态目录 .mcpack-sync/
//   这三处刻意不改：改动要触及 40+ 文件且无对外收益，且 .mcpack-sync/ 一改会让
//   已有部署丢掉 state.json（＝失忆重扫重传一遍）。
//
// 职责边界：只做「接线」——把 BDS/LeviLamina 的生命周期、命令、日志、目录约定
// 接到 mcpsync_core 上；所有业务逻辑都在 core 里，便于脱离 BDS 单测与 CI 验证。
//
// 构建要求：MSVC（cl.exe）。LeviLamina.dll 导出的是 MSVC 修饰名
//   ?getModDir@Mod@mod@ll@@QEAAAEBVpath@filesystem@std@@XZ
// MinGW(g++) 的 Itanium ABI 无法解析这些符号，链接必然失败。
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/server/CDNConfig.h"
#include "mc/server/ServerInstance.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

#include "mcpsync/cdn.hpp"
#include "mcpsync/config.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/gitauth.hpp"
#include "mcpsync/logger.hpp"
#include "mcpsync/pipeline.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <thread>

namespace mcpack_sync::crash {
void install();
}

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mcpack_sync {

namespace fs = std::filesystem;

class McpackSyncMod {
public:
    static McpackSyncMod& getInstance() {
        static McpackSyncMod instance;
        return instance;
    }

    McpackSyncMod() = default;

    // 静态实例析构发生在 DLL_PROCESS_DETACH。此时若 mBootstrap 仍是 joinable
    // （线程函数已返回但从未 join/detach），std::thread 的析构会调用 std::terminate
    // → abort → 0xC0000409，把「正常退出」变成崩溃。实测崩溃栈正是
    //   McpackSyncMod::~McpackSyncMod -> std::thread::~thread -> ucrtbase(int 0x29)
    // 退出流程里 LL 并不总会调用 unload()，所以必须在这里兜底。
    ~McpackSyncMod() {
        mBootstrapStop.store(true);
        if (!mBootstrap.joinable()) return;
        if (mBootstrapDone.load()) {
            mBootstrap.join(); // 线程已返回，join 立即完成
        } else {
            // ExitProcess 阶段其他线程已被系统终止，detach 是安全的
            mBootstrap.detach();
        }
    }

    bool load();
    bool unload();
    bool enable() { return true; }
    bool disable() { return true; }

    std::shared_ptr<ll::mod::Mod> self() { return ll::mod::NativeMod::current(); }

private:
    bool reload(std::string& err);
    void registerCommands();

    // 把一次 git 凭据修改写进配置并重建流水线（让 GitPublisher 用上新凭据）
    bool updateGit(mcpsync::GitField field, std::string const& a, std::string const& b, std::string& err);

    // ---- 资源包外链下发 ----
    // 读取 cdn.json 里拼好的完整外链，调度到服务器线程写入 BDS 内存。
    void pushCdnToServer();
    // 实际写入 CDNConfig::mPackCDNUrls；必须在服务器线程执行。
    size_t applyCdnEntries(std::vector<mcpsync::PackCdnEntry> const& entries);
    // 打印 BDS 内存中现有的 (packId -> url)，用于确认主键格式。
    void dumpServerCdn(CommandOutput& output);

    // ---- 服务器就绪 ----
    // 把 onServerReady() 投递到服务器线程（命令注册/写 BDS 内存都必须在服务器线程做）
    void postServerReady();
    void onServerReady();

    std::unique_ptr<mcpsync::Pipeline> mPipeline;
    std::shared_ptr<ll::event::Listener<ll::event::server::ServerStartedEvent>>  mStartListener;
    std::shared_ptr<ll::event::Listener<ll::event::server::ServerStoppingEvent>> mStopListener;
    std::mutex        mReloadMtx;
    std::thread       mBootstrap;
    std::atomic<bool> mBootstrapStop{false};
    std::atomic<bool> mBootstrapDone{false};   // 线程函数已返回（join 可立即完成）
    std::atomic<bool> mReadyDone{false};
    bool              mBdsRuntime{false};      // 加载器是否提供 bedrock_runtime.dll
    bool              mCommandsRegistered{false};
    fs::path          mServerRoot;
    fs::path          mConfigPath;
};

namespace {

mcpsync::LogLevel toSync(ll::io::LogLevel l) {
    switch (l) {
    case ll::io::LogLevel::Trace: return mcpsync::LogLevel::Trace;
    case ll::io::LogLevel::Debug: return mcpsync::LogLevel::Debug;
    case ll::io::LogLevel::Info: return mcpsync::LogLevel::Info;
    case ll::io::LogLevel::Warn: return mcpsync::LogLevel::Warn;
    case ll::io::LogLevel::Error: return mcpsync::LogLevel::Error;
    default: return mcpsync::LogLevel::Info;
    }
}

// 把 core 的日志转发到 BDS 控制台。
// 不要在这里再加 [packrelay] 前缀：LL 的 Logger 已经会打 "[mod 名]"，之前加过一版
// 结果日志变成 "[packrelay] [packrelay] xxx"，前缀重复。
void forwardLog(mcpsync::LogLevel level, std::string_view msg) {
    auto mod = ll::mod::NativeMod::current();
    if (!mod) return;
    auto& lg = mod->getLogger();
    std::string line(msg.data(), msg.size());
    switch (level) {
    case mcpsync::LogLevel::Trace:
    case mcpsync::LogLevel::Debug: lg.debug(line); break;
    case mcpsync::LogLevel::Info: lg.info(line); break;
    case mcpsync::LogLevel::Warn: lg.warn(line); break;
    case mcpsync::LogLevel::Error: lg.error(line); break;
    default: break;
    }
}

} // namespace

// ---- /relaygit 子命令参数 ----
// LL 的命令参数通过聚合类型的成员名绑定，required("<name>") 在编译期校验成员存在。
struct GitRemoteParam {
    std::string url;
};

struct GitBranchParam {
    std::string name;
};

struct GitUserParam {
    std::string name;
    std::string email;
};

struct GitTokenParam {
    std::string token;
};

struct GitPasswordParam {
    std::string username;
    std::string password;
};

struct GitSshKeyParam {
    std::string path;
};

struct GitModeParam {
    std::string mode;
};

bool McpackSyncMod::load() {
    mcpack_sync::crash::install();

    auto nativeMod = ll::mod::NativeMod::current();
    if (!nativeMod) return false;

    // mods/<name>/ -> mods/ -> server root
    mServerRoot = nativeMod->getModDir().parent_path().parent_path();
    nativeMod->getLogger().info("loading packrelay, server root = {}", mServerRoot.string());

    {
        // 加载器侧是否提供了 bedrock_runtime.dll（prelink 生成的导入库全靠它）。
        // 这里只探测，不硬依赖：没有它也能跑扫描/推送，只是不能用 BDS 命令 API。
        HMODULE h = ::GetModuleHandleW(L"bedrock_runtime.dll");
        if (!h) h = ::LoadLibraryW(L"bedrock_runtime.dll");
        mBdsRuntime = (h != nullptr);
        nativeMod->getLogger().info(
            "bedrock_runtime.dll available = {} (commands {})",
            mBdsRuntime,
            mBdsRuntime ? "enabled" : "disabled"
        );
    }

    mcpsync::logger().setSink(&forwardLog);

    std::string err;
    if (!reload(err)) {
        nativeMod->getLogger().error("failed to load config: {}", err);
        return false;
    }

    // 不能在 load() 阶段注册命令：CommandRegistrar::getRegistry() 会直接解引用
    // service::getCommandRegistry()，而 CommandRegistry 要等 ServerInstance 构造完才存在，
    // 提前调用会空指针解引用把 BDS 带崩（已由崩溃栈实测确认）。
    //
    // 双保险：
    //  1) 首选事件 ServerStartedEvent（官方路径）；
    //  2) 事件在某些 LL/BDS 组合下不一定派发（实测本机就没触发），因此再起一个轮询线程，
    //     等 getCommandRegistry() 可用后用 ServerThreadExecutor 投递到服务器线程执行同一段逻辑。
    // 两条路径都走 postServerReady()，内部有 once 保护，重复调用无副作用。
    bool const startOk = ll::event::EventBus::getInstance().addListener<ll::event::server::ServerStartedEvent>(
        mStartListener = ll::event::Listener<ll::event::server::ServerStartedEvent>::create(
            [this](ll::event::server::ServerStartedEvent&) { postServerReady(); }
        )
    );
    nativeMod->getLogger().info("ServerStartedEvent listener registered = {}", startOk);

    mBootstrap = std::thread([this] {
        struct DoneGuard {
            std::atomic<bool>& flag;
            ~DoneGuard() { flag.store(true); }
        } doneGuard{mBootstrapDone};

        for (int i = 0; i < 600 && !mBootstrapStop.load(); ++i) {
            if (ll::service::bedrock::getCommandRegistry()) {
                postServerReady();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    mStopListener = ll::event::Listener<ll::event::server::ServerStoppingEvent>::create(
        [this](ll::event::server::ServerStoppingEvent&) {
            self()->getLogger().info("server stopping, flushing state");
            if (!mPipeline) return;
            mPipeline->stopWatcher();
            mPipeline->state().save();
        }
    );
    ll::event::EventBus::getInstance().addListener<ll::event::server::ServerStoppingEvent>(mStopListener);

    return true;
}

bool McpackSyncMod::unload() {
    auto self = ll::mod::NativeMod::current();
    mBootstrapStop.store(true);
    if (mBootstrap.joinable()) mBootstrap.join();
    if (mStartListener) {
        ll::event::EventBus::getInstance().removeListener(mStartListener);
        mStartListener.reset();
    }
    if (mStopListener) {
        ll::event::EventBus::getInstance().removeListener(mStopListener);
        mStopListener.reset();
    }
    if (mPipeline) {
        mPipeline->stopWatcher();
        mPipeline->state().save();
        mPipeline.reset();
    }
    if (self) self->getLogger().info("packrelay unloaded");
    return true;
}

bool McpackSyncMod::reload(std::string& err) {
    std::lock_guard<std::mutex> lk(mReloadMtx);
    auto                        self = ll::mod::NativeMod::current();
    if (!self) {
        err = "no current mod";
        return false;
    }

    fs::path cfgPath = self->getConfigDir() / "config.json";
    mConfigPath      = cfgPath;
    if (!mcpsync::fsu::exists(cfgPath)) {
        auto st = mcpsync::fsu::writeFileAtomic(cfgPath, mcpsync::defaultConfigJson());
        if (!st) {
            err = st.error().str();
            return false;
        }
        self->getLogger().warn("config created at {}, please fill in git.remote / github.owner/repo", cfgPath.string());
    }

    auto cfg = mcpsync::loadConfig(cfgPath);
    if (!cfg) {
        err = cfg.error().str();
        return false;
    }
    // 所有相对路径以服务器根目录为基准，保证 mod 升级/重装后状态不漂移
    cfg.value().baseDir = mServerRoot;

    auto v = mcpsync::validateConfig(cfg.value());
    if (!v) {
        // 配置不完整不阻塞服务器启动，只降级为"仅扫描"
        self->getLogger().warn("config incomplete, running in scan-only mode: {}", v.error().str());
        cfg.value().git.enabled  = false;
        cfg.value().cdn.enabled  = false;
        cfg.value().release.waitForRelease = false;
    }

    if (mPipeline) {
        mPipeline->stopWatcher();
        mPipeline->state().save();
    }

    auto pipe = std::make_unique<mcpsync::Pipeline>(cfg.value());
    auto st   = pipe->init();
    if (!st) {
        err = st.error().str();
        return false;
    }
    // 每次发布刷新 CDN 后，把新拼好的外链推回 BDS 内存，实现无重启热生效
    pipe->onCdnPublished = [this]() { pushCdnToServer(); };
    pipe->startWatcher();
    mPipeline = std::move(pipe);

    self->getLogger().info("packrelay ready: resourceDir={}, git.enabled={}",
                           mcpsync::fsu::toUtf8(cfg.value().resourcePath()), cfg.value().git.enabled);
    return true;
}

bool McpackSyncMod::updateGit(mcpsync::GitField field, std::string const& a, std::string const& b, std::string& err) {
    if (!mPipeline) {
        err = "packrelay not initialized";
        return false;
    }
    if (mConfigPath.empty()) {
        err = "config path unknown";
        return false;
    }

    // 在副本上改，校验通过再落盘，避免半截状态写进文件
    mcpsync::GitConfig git = mPipeline->config().git;

    auto st = mcpsync::applyGitField(git, field, a, b);
    if (!st) {
        err = st.error().str();
        return false;
    }
    auto vs = mcpsync::validateGitForConsole(git);
    if (!vs) {
        err = vs.error().str();
        return false;
    }
    auto saved = mcpsync::patchConfigFileGit(mConfigPath, git);
    if (!saved) {
        err = "save failed: " + saved.error().str();
        return false;
    }

    // GitPublisher 在构造时拷贝了 git 配置，必须重建流水线才能用上新凭据
    if (!reload(err)) return false;
    return true;
}

void McpackSyncMod::registerCommands() {
    using namespace ll::command;

    auto& registrar = CommandRegistrar::getServerInstance();

    // 根命令 /relay（游戏内管理员可用）：status / scan / cdn / push / dump / reload
    auto& root = registrar.getOrCreateCommand(
        "relay",
        "packrelay: resource pack release and cdn url relay",
        CommandPermissionLevel::GameDirectors
    );

    auto doStatus = [this](CommandOrigin const&, CommandOutput& output) {
        if (!mPipeline) {
            output.error("packrelay not initialized");
            return;
        }
        auto rep = mPipeline->lastReport();
        if (!rep) {
            output.success("no run yet, run count = " + std::to_string(mPipeline->runCount()));
            return;
        }
        output.success(rep->summary());
        for (auto const& t : mPipeline->cdn().targets()) {
            output.success("cdn " + t.slug + " -> " + t.publicUrl + " (" + t.mirror + ")");
        }
    };

    auto doScan = [this](CommandOrigin const&, CommandOutput& output) {
        if (!mPipeline) {
            output.error("packrelay not initialized");
            return;
        }
        mPipeline->requestScan();
        output.success("scan requested (async)");
    };

    auto doCdn = [this](CommandOrigin const&, CommandOutput& output) {
        if (!mPipeline) {
            output.error("packrelay not initialized");
            return;
        }
        output.success("manifest: " + mPipeline->cdn().stableUrl());
        for (auto const& t : mPipeline->cdn().targets()) {
            output.success("  " + t.uuid + " v" + t.version + " (" + t.mirror + ") -> " + t.publicUrl);
        }
    };

    auto doPush = [this](CommandOrigin const&, CommandOutput& output) {
        if (!mPipeline) {
            output.error("packrelay not initialized");
            return;
        }
        pushCdnToServer();
        output.success("scheduled: pushing cdn urls to server thread");
    };

    auto doDump = [this](CommandOrigin const&, CommandOutput& output) { dumpServerCdn(output); };

    auto doReload = [this](CommandOrigin const&, CommandOutput& output) {
        std::string err;
        if (reload(err)) {
            output.success("config reloaded");
        } else {
            output.error("reload failed: " + err);
        }
    };

    // 无参数等价于 /relay status，少敲一层
    root.overload<>().execute(doStatus);
    root.overload<>().text("status").execute(doStatus);
    root.overload<>().text("scan").execute(doScan);
    root.overload<>().text("cdn").execute(doCdn);
    root.overload<>().text("push").execute(doPush);
    root.overload<>().text("dump").execute(doDump);
    root.overload<>().text("reload").execute(doReload);

    // ============ /relaygit —— 控制台管理 git 凭据 ============
    // 单独一条根命令，因为凭据管理必须是 Host（仅控制台）；
    // 上面的 /relay 是 GameDirectors，游戏内管理员能查状态但不能改令牌。
    // 所有修改都会立即写回配置文件并重建流水线，不需要重启服务器。
    auto& gitCmd =
        registrar.getOrCreateCommand("relaygit", "packrelay: manage git credentials", CommandPermissionLevel::Host);

    auto printGit = [this](CommandOutput& output) {
        output.success("config: " + mcpsync::fsu::toUtf8(mConfigPath));
        for (auto const& line : mcpsync::describeGitConfig(mPipeline->config().git)) {
            output.success("  " + line);
        }
    };

    // /relaygit —— 查看当前 git 配置（敏感项已脱敏）
    gitCmd.overload<>().execute([this, printGit](CommandOrigin const&, CommandOutput& output) {
        if (!mPipeline) {
            output.error("packrelay not initialized");
            return;
        }
        printGit(output);
    });

    // /relaygit remote <url>
    gitCmd.overload<GitRemoteParam>().text("remote").required("url").execute(
        [this, printGit](CommandOrigin const&, CommandOutput& output, GitRemoteParam const& p) {
            std::string err;
            if (!updateGit(mcpsync::GitField::Remote, p.url, {}, err)) {
                output.error(err);
                return;
            }
            output.success("remote saved: " + p.url);
            printGit(output);
        }
    );

    // /relaygit branch <name>
    gitCmd.overload<GitBranchParam>().text("branch").required("name").execute(
        [this](CommandOrigin const&, CommandOutput& output, GitBranchParam const& p) {
            std::string err;
            if (!updateGit(mcpsync::GitField::Branch, p.name, {}, err)) {
                output.error(err);
                return;
            }
            output.success("branch saved: " + p.name);
        }
    );

    // /relaygit user <name> <email>
    gitCmd.overload<GitUserParam>().text("user").required("name").required("email").execute(
        [this](CommandOrigin const&, CommandOutput& output, GitUserParam const& p) {
            std::string err;
            if (!updateGit(mcpsync::GitField::Author, p.name, p.email, err)) {
                output.error(err);
                return;
            }
            output.success("commit author saved: " + p.name + " <" + p.email + ">");
        }
    );

    // /relaygit token <token>  —— GitHub PAT，写完自动切到 token_inline
    gitCmd.overload<GitTokenParam>().text("token").required("token").execute(
        [this, printGit](CommandOrigin const&, CommandOutput& output, GitTokenParam const& p) {
            std::string err;
            if (!updateGit(mcpsync::GitField::Token, p.token, {}, err)) {
                output.error(err);
                return;
            }
            output.success("token saved (masked): " + mcpsync::maskSecret(p.token));
            printGit(output);
        }
    );

    // /relaygit password <username> <password>
    gitCmd.overload<GitPasswordParam>().text("password").required("username").required("password").execute(
        [this](CommandOrigin const&, CommandOutput& output, GitPasswordParam const& p) {
            std::string err;
            if (!updateGit(mcpsync::GitField::Password, p.username, p.password, err)) {
                output.error(err);
                return;
            }
            output.success("credential saved: " + p.username + " / " + mcpsync::maskSecret(p.password));
        }
    );

    // /relaygit sshkey <path>  —— 写完自动切到 ssh 模式
    gitCmd.overload<GitSshKeyParam>().text("sshkey").required("path").execute(
        [this](CommandOrigin const&, CommandOutput& output, GitSshKeyParam const& p) {
            std::string err;
            if (!updateGit(mcpsync::GitField::SshKey, p.path, {}, err)) {
                output.error(err);
                return;
            }
            output.success("ssh key saved: " + p.path + " (authMode=ssh)");
        }
    );

    // /relaygit mode <token_env|token_inline|credential_helper|ssh>
    gitCmd.overload<GitModeParam>().text("mode").required("mode").execute(
        [this](CommandOrigin const&, CommandOutput& output, GitModeParam const& p) {
            std::string err;
            if (!updateGit(mcpsync::GitField::AuthMode, p.mode, {}, err)) {
                output.error(err);
                return;
            }
            output.success("authMode saved: " + p.mode);
        }
    );

    // /relaygit clear —— 清除 token / password / username / sshKey
    gitCmd.overload<>().text("clear").execute([this, printGit](CommandOrigin const&, CommandOutput& output) {
        std::string err;
        if (!updateGit(mcpsync::GitField::Clear, {}, {}, err)) {
            output.error(err);
            return;
        }
        output.success("credentials cleared");
        printGit(output);
    });

    // /relaygit test —— git 连通性测试（同步执行，最长 20s）
    gitCmd.overload<>().text("test").execute([this](CommandOrigin const&, CommandOutput& output) {
        if (!mPipeline) {
            output.error("packrelay not initialized");
            return;
        }
        auto const& git = mPipeline->config().git;
        if (git.remote.empty()) {
            output.error("git.remote is empty, use /relaygit remote <url>");
            return;
        }
        output.success("probing " + git.remote + " (auth=" + git.authMode + ") ...");

        auto pr = mcpsync::probeGitRemote(git, mServerRoot, 20000);
        if (pr.ok) {
            output.success("OK: remote reachable in " + std::to_string(pr.elapsedMs) + "ms");
            if (!pr.out.empty()) output.success("refs: " + pr.out.substr(0, 200));
            return;
        }
        output.success(
            "failed: exit=" + std::to_string(pr.exitCode) + " elapsed=" + std::to_string(pr.elapsedMs) + "ms"
        );
        if (!pr.out.empty()) output.success("git: " + pr.out.substr(0, 200));
        output.error(pr.hint.empty() ? "connectivity test failed" : pr.hint);
    });
}

size_t McpackSyncMod::applyCdnEntries(std::vector<mcpsync::PackCdnEntry> const& entries) {
    if (entries.empty()) return 0;

    auto inst = ll::service::bedrock::getServerInstance();
    if (!inst) {
        mcpsync::logger().warn("cdn: ServerInstance unavailable, cannot push to BDS");
        return 0;
    }

    // ServerInstance::mCDNConfig 是 public 的 TypedStorage<NonOwnerPointer<CDNConfig>>，
    // NonOwnerPointer::get() 会在持有者失效时返回 nullptr，必须判空。
    ::CDNConfig* cdnConfig = inst->mCDNConfig.get().get();
    if (!cdnConfig) {
        mcpsync::logger().warn("cdn: ServerInstance::mCDNConfig is null, cannot push to BDS");
        return 0;
    }

    auto& urls = cdnConfig->mPackCDNUrls.get();

    // 具体 upsert 规则在 core 里（可单测）；这里只负责拿到 BDS 的容器
    mcpsync::upsertPackCdnUrls(urls, entries);

    mcpsync::logger().info(
        "cdn: pushed " + std::to_string(entries.size()) + " pack(s) into BDS CDNConfig, mPackCDNUrls now " +
        std::to_string(urls.size()) + " entries"
    );
    // 把真实主键打出来：BDS 到底按 <uuid> 还是 <uuid>_<version> 取外链，这里能直接看到
    size_t shown = 0;
    for (auto const& kv : urls) {
        if (shown++ >= 4) break;
        mcpsync::logger().info("cdn:   [" + kv.first + "] -> " + kv.second);
    }
    return entries.size();
}

void McpackSyncMod::postServerReady() {
    ll::thread::ServerThreadExecutor::getDefault().execute([this] { onServerReady(); });
}

void McpackSyncMod::onServerReady() {
    if (!mPipeline) return;
    if (mReadyDone.exchange(true)) return;

    auto log = [](std::string const& s) {
        if (auto m = ll::mod::NativeMod::current()) m->getLogger().info(s);
    };
    auto warn = [](std::string const& s) {
        if (auto m = ll::mod::NativeMod::current()) m->getLogger().warn(s);
    };

    // 重新探测一次：load() 阶段加载器还没把虚拟模块挂上去，这时探测必然失败。
    // 注意：写 BDS 内存（CDNConfig）走的是 LeviLamina.dll 导出的 getServerInstance()
    // 加上头文件里的内联偏移访问，完全不依赖 bedrock_runtime.dll；
    // 只有 BDS 命令 API（CommandOutput::success / CommandRegistry::parse /
    // Bedrock::type_id）需要它，缺了必须在链接期之外拦掉，否则 delay-load 会带走进程。
    if (!mBdsRuntime) {
        HMODULE h = ::GetModuleHandleW(L"bedrock_runtime.dll");
        if (!h) h = ::LoadLibraryW(L"bedrock_runtime.dll");
        mBdsRuntime = (h != nullptr);
    }

    log(std::string("server ready: bedrock_runtime.dll ") + (mBdsRuntime ? "available" : "unavailable"));

    // 1) 先用上次落盘的清单把外链补进 BDS 内存。
    //    重启后 CDNConfig 是空的（磁盘配置只在 ServerInstance 构造时读一次），
    //    不补这一遍，首个玩家拿到的是服务端直传而不是外链。
    log("server ready: pushing cdn urls into BDS memory");
    pushCdnToServer();
    log("server ready: cdn push dispatched");

    // 2) 控制台指令。必须在服务器线程、且在 CommandRegistry 建好之后注册。
    if (mBdsRuntime) {
        if (!mCommandsRegistered) {
            log("server ready: registering console commands");
            registerCommands();
            mCommandsRegistered = true;
            log("packrelay console commands registered (/relay, /relaygit)");
        }
    } else {
        warn(
            "bedrock_runtime.dll is not provided by this loader, so BDS command symbols cannot be resolved. "
            "Console commands (/relay, /relaygit) are disabled; scanning, git push, release and "
            "the CDN url push into BDS memory are unaffected. The same operations are available through the "
            "packrelay CLI: packrelay git remote|token|test, packrelay cdn show|render, packrelay scan|once."
        );
    }

    if (mPipeline->config().server.scanOnStart) {
        log("server ready, scheduling resource scan");
        mPipeline->requestScan();
    }
}

void McpackSyncMod::pushCdnToServer() {
    auto inst = ll::service::bedrock::getServerInstance();
    if (!inst) return;
    if (!mPipeline) return;

    auto const& cfg      = mPipeline->config();
    fs::path    manifest = cfg.baseDir / mcpsync::fsu::fromUtf8(cfg.cdn.manifestFile);

    std::string text;
    auto        st = mcpsync::fsu::readFile(manifest, text);
    if (!st) {
        mcpsync::logger().warn("cdn: manifest not readable (" + mcpsync::fsu::toUtf8(manifest) + "): " + st.error().str());
        return;
    }

    std::vector<mcpsync::PackCdnEntry> entries;
    auto                              ps = mcpsync::parsePackCdnEntries(text, entries);
    if (!ps) {
        mcpsync::logger().warn("cdn: manifest unusable: " + ps.error().str());
        return;
    }

    // 本函数可能来自流水线线程或事件回调，统一排到服务器线程再改 BDS 对象。
    // 用 LL 自己的 ServerThreadExecutor，而不是已被标记 deprecated 的
    // ServerInstance::queueForServerThread（BDS 1.26.60 的 runtime data 里已无该符号）。
    ll::thread::ServerThreadExecutor::getDefault().execute(
        [this, entries = std::move(entries)]() mutable { applyCdnEntries(entries); }
    );
}

void McpackSyncMod::dumpServerCdn(CommandOutput& output) {
    auto inst = ll::service::bedrock::getServerInstance();
    if (!inst) {
        output.error("ServerInstance unavailable");
        return;
    }
    ::CDNConfig* cdnConfig = inst->mCDNConfig.get().get();
    if (!cdnConfig) {
        output.error("CDNConfig is null");
        return;
    }
    auto const& urls = cdnConfig->mPackCDNUrls.get();
    output.success("CDNConfig::mPackCDNUrls: " + std::to_string(urls.size()) + " entries");
    for (auto const& kv : urls) {
        output.success("  " + kv.first + " -> " + kv.second);
    }
}

} // namespace mcpack_sync

LL_REGISTER_MOD(mcpack_sync::McpackSyncMod, mcpack_sync::McpackSyncMod::getInstance());
