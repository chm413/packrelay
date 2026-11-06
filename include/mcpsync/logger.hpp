// mcpack-sync : 轻量日志（线程安全、可按大小轮转）
#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <fstream>
#include <memory>

namespace mcpsync {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

LogLevel    parseLogLevel(std::string_view s);
std::string_view toString(LogLevel l) noexcept;

class Logger {
public:
    Logger();
    ~Logger();

    void setLevel(LogLevel l) noexcept { mLevel = l; }
    LogLevel level() const noexcept { return mLevel; }

    // 文件输出（失败仅告警，不影响主流程）
    void openFile(std::string const& path, size_t maxBytes = 8u << 20, int keep = 3);

    void log(LogLevel l, std::string_view msg);
    void trace(std::string_view m) { log(LogLevel::Trace, m); }
    void debug(std::string_view m) { log(LogLevel::Debug, m); }
    void info(std::string_view m) { log(LogLevel::Info, m); }
    void warn(std::string_view m) { log(LogLevel::Warn, m); }
    void error(std::string_view m) { log(LogLevel::Error, m); }

    // 供 LeviLamina 适配层挂接：把日志同时转发到 mod 的 logger
    using Sink = void (*)(LogLevel, std::string_view);
    void setSink(Sink s) noexcept { mSink = s; }

private:
    void rotateLocked();

    LogLevel                mLevel{LogLevel::Info};
    std::mutex              mMtx;
    std::ofstream           mFile;
    std::string             mPath;
    size_t                  mMaxBytes{8u << 20};
    int                     mKeep{3};
    Sink                    mSink{nullptr};
};

Logger& logger();

// 便捷封装
void logInfo(std::string const& m);
void logWarn(std::string const& m);
void logError(std::string const& m);

} // namespace mcpsync
