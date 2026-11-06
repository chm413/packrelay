// mcpack-sync : 子进程执行（git 调用）
#pragma once

#include <map>
#include <string>
#include <vector>

#include "mcpsync/result.hpp"

namespace mcpsync {

struct ProcResult {
    int         exitCode{-1};
    std::string out;      // 合并 stdout+stderr（UTF-8）
    bool        timedOut{false};
};

struct ProcOptions {
    std::string                        cwd;
    std::vector<std::string>           args;
    std::map<std::string, std::string> env;      // 追加/覆盖环境变量
    int                                timeoutMs{120000};
    std::string                        stdinData;
};

// 不经过 shell，直接 CreateProcessW，参数按需加引号，避免注入
ProcResult runProcess(std::string const& exe, ProcOptions const& opts);

// 定位 git 可执行文件（PATH / 常见安装位置）
Result<std::string> findGit();

// 执行 git 子命令；失败时填充 Git 错误并附带上一条命令上下文
Result<ProcResult> gitRun(std::string const& repoDir, std::vector<std::string> const& args, int timeoutMs, std::map<std::string, std::string> const& env = {});

} // namespace mcpsync
