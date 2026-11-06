// mcpack-sync : 错误与结果类型
// 设计原则：不跨 DLL/API 边界抛异常，统一用 Result<T> 传递可诊断的错误。
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace mcpsync {

enum class ErrCode {
    Ok = 0,
    Config,        // 配置缺失/非法
    NotFound,      // 文件/路径/资源不存在
    Io,            // 文件读写失败
    Parse,         // JSON / manifest 解析失败
    Scan,          // 扫描阶段失败
    Process,       // 子进程启动/执行失败
    Git,           // git 操作失败
    Network,       // 网络/HTTP 失败
    Http,          // 非 2xx 响应
    Release,       // release 等待超时/缺失
    Conflict,      // 同 uuid 多目录冲突
    Internal,      // 不变量被破坏
};

const char* toString(ErrCode c) noexcept;

struct Error {
    ErrCode     code{ErrCode::Internal};
    std::string message;   // 面向人的描述
    std::string detail;    // 原始输出 / 上下文（可为空）
    bool        retryable{false};

    Error() = default;
    Error(ErrCode c, std::string msg, std::string det = {}, bool retry = false)
    : code(c), message(std::move(msg)), detail(std::move(det)), retryable(retry) {}

    std::string str() const;
};

template <class T>
class Result {
public:
    Result(T value) : mData(std::move(value)) {}
    Result(Error err) : mData(std::move(err)) {}

    bool ok() const noexcept { return mData.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T&       value() & { return std::get<0>(mData); }
    T const& value() const& { return std::get<0>(mData); }
    T&&      value() && { return std::move(std::get<0>(mData)); }

    Error&       error() & { return std::get<1>(mData); }
    Error const& error() const& { return std::get<1>(mData); }

    T valueOr(T fallback) const { return ok() ? std::get<0>(mData) : std::move(fallback); }

private:
    std::variant<T, Error> mData;
};

// 无返回值的便捷结果
class Status {
public:
    Status() = default;
    Status(Error err) : mError(std::move(err)) {}

    bool ok() const noexcept { return !mError.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    Error const& error() const { return *mError; }

private:
    std::optional<Error> mError;
};

} // namespace mcpsync
