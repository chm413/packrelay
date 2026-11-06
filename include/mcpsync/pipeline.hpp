// mcpack-sync : 流水线编排
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mcpsync/cdn.hpp"
#include "mcpsync/config.hpp"
#include "mcpsync/diff.hpp"
#include "mcpsync/git.hpp"
#include "mcpsync/github.hpp"
#include "mcpsync/state.hpp"

namespace mcpsync {

struct PipelineOptions {
    bool dryRun{false};        // 只扫描+比对，不推送
    bool force{false};         // 忽略 unchanged，强制重新发布
    bool skipRelease{false};   // 不等 release（离线/调试）
};

struct PackOutcome {
    std::string uuid;
    std::string slug;
    ChangeKind  kind{ChangeKind::Unchanged};
    bool        ok{true};
    std::string stage;          // 失败所在阶段
    std::string message;
    std::string commit;
    std::string tag;
    std::string cdnUrl;
    uint64_t    elapsedMs{0};
};

struct PipelineReport {
    bool                        ok{true};
    std::string                 startedAt;
    std::string                 finishedAt;
    uint64_t                    elapsedMs{0};
    size_t                      packsScanned{0};
    size_t                      filesHashed{0};
    uint64_t                    bytesRead{0};
    size_t                      added{0};
    size_t                      updated{0};
    size_t                      removed{0};
    size_t                      unchanged{0};
    std::vector<PackOutcome>    outcomes;
    std::vector<std::string>    warnings;
    std::string                 error;

    std::string summary() const;
};

class Pipeline {
public:
    explicit Pipeline(Config cfg);
    ~Pipeline();

    Status init();

    // 单次执行：扫描 -> 比对 -> 推送 -> 等 release -> 更新 CDN
    PipelineReport runOnce(PipelineOptions opts = {});

    // 仅扫描与比对（不触碰 git / 网络）
    PipelineReport scanOnly();

    // 周期执行（引擎自身调度；mod 场景下由事件驱动）
    void startWatcher();
    void stopWatcher();
    bool watching() const { return mWatching.load(); }

    // 供 mod 事件回调：请求尽快执行一次
    void requestScan();
    uint64_t runCount() const { return mRunCount.load(); }
    PipelineReport const* lastReport() const;

    StateStore&  state() { return mState; }
    CdnManager&  cdn() { return mCdn; }
    Config&      config() { return mCfg; }

    // 每次成功刷新 CDN 后的回调。注意：在流水线工作线程上调用，
    // 回调内若需碰 BDS 对象必须自行切到服务器线程（queueForServerThread）。
    std::function<void()> onCdnPublished;

private:
    bool processChange(Change const& ch, PipelineOptions const& opts, PackOutcome& out);
    // 推送完成后：等 release -> 解析镜像 -> 更新 CDN -> 落状态（可单独重试）
    bool finishRelease(PackState& st, PackSnapshot const& snap, PipelineOptions const& opts, PackOutcome& out);
    void watcherLoop();

    Config                    mCfg;
    StateStore                mState;
    GitPublisher              mGit;
    GithubClient              mGh;
    MirrorResolver            mMirror;
    CdnManager                mCdn;

    std::mutex                mRunMtx;
    std::mutex                mCvMtx;
    std::condition_variable   mCv;
    std::atomic<bool>         mWatching{false};
    std::atomic<bool>         mStop{false};
    std::atomic<bool>         mScanRequested{false};
    std::atomic<uint64_t>     mRunCount{0};
    std::thread               mWatchThread;

    mutable std::mutex        mReportMtx;
    std::unique_ptr<PipelineReport> mLastReport;
};

} // namespace mcpsync
