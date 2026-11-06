#include "mcpsync/logger.hpp"
#include "mcpsync/fsutil.hpp"

#include <cstdio>
#include <ctime>
#include <iostream>

namespace mcpsync {

LogLevel parseLogLevel(std::string_view s) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "info") return LogLevel::Info;
    if (s == "warn" || s == "warning") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    if (s == "off" || s == "none") return LogLevel::Off;
    return LogLevel::Info;
}

std::string_view toString(LogLevel l) noexcept {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    default: return "OFF  ";
    }
}

Logger::Logger() = default;
Logger::~Logger() = default;

void Logger::openFile(std::string const& path, size_t maxBytes, int keep) {
    std::lock_guard<std::mutex> lk(mMtx);
    mPath     = path;
    mMaxBytes = maxBytes;
    mKeep     = keep;
    auto p    = fsu::fromUtf8(path);
    auto dir  = p.parent_path();
    if (!dir.empty()) fsu::ensureDir(dir);
    mFile.open(p, std::ios::binary | std::ios::app);
    if (!mFile) {
        std::cerr << "[packrelay] cannot open log file: " << path << "\n";
    }
}

void Logger::rotateLocked() {
    if (mPath.empty() || !mFile.is_open()) return;
    mFile.close();
    auto base = fsu::fromUtf8(mPath);
    // x.log.(keep-1) -> x.log.keep
    for (int i = mKeep - 1; i >= 1; --i) {
        auto from = fsu::fromUtf8(mPath + "." + std::to_string(i));
        auto to   = fsu::fromUtf8(mPath + "." + std::to_string(i + 1));
        std::error_code ec;
        if (fsu::exists(from)) {
            std::filesystem::rename(from, to, ec);
        }
    }
    if (mKeep >= 1) {
        std::error_code ec;
        std::filesystem::rename(base, fsu::fromUtf8(mPath + ".1"), ec);
    }
    mFile.open(base, std::ios::binary | std::ios::trunc);
}

void Logger::log(LogLevel l, std::string_view msg) {
    if (l < mLevel || mLevel == LogLevel::Off) return;

    char ts[32];
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    std::string line;
    line.reserve(msg.size() + 24);
    line += '[';
    line += ts;
    line += "] [";
    line += toString(l);
    line += "] ";
    line.append(msg.data(), msg.size());

    // 有宿主 sink（BDS 控制台）时不再自己写 stdout：
    // 否则同一条日志会出现两遍，而且 std::cout 与 BDS 自己的输出没有同步，
    // 实测会出现 "entries16:03:50.820 IFO ..." 这种两行粘在一起的乱序。
    bool const hasSink = static_cast<bool>(mSink);
    {
        std::lock_guard<std::mutex> lk(mMtx);
        if (!hasSink) {
            std::cout << line << '\n';
            std::cout.flush();
        }
        if (mFile.is_open()) {
            mFile << line << '\n';
            mFile.flush();
            auto pos = static_cast<size_t>(mFile.tellp());
            if (pos > mMaxBytes) rotateLocked();
        }
    }
    // sink 在锁外调用：它会转去宿主的 logger，避免重入死锁
    if (hasSink) mSink(l, msg);
}

Logger& logger() {
    static Logger inst;
    return inst;
}

void logInfo(std::string const& m) { logger().info(m); }
void logWarn(std::string const& m) { logger().warn(m); }
void logError(std::string const& m) { logger().error(m); }

} // namespace mcpsync
