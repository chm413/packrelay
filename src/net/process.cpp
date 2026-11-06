#include "mcpsync/process.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/logger.hpp"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace mcpsync {

namespace {

#ifdef _WIN32
std::wstring toWide(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string fromWide(std::wstring const& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// 按 Microsoft CRT 规则为参数加引号
std::wstring quoteArg(std::wstring const& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}
#endif

} // namespace

ProcResult runProcess(std::string const& exe, ProcOptions const& opts) {
    ProcResult res;
#ifdef _WIN32
    std::wstring cmdLine = quoteArg(toWide(exe));
    for (auto const& a : opts.args) {
        cmdLine.push_back(L' ');
        cmdLine += quoteArg(toWide(a));
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        res.out = "CreatePipe failed";
        return res;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdinRead = nullptr, stdinWrite = nullptr;
    if (!opts.stdinData.empty()) {
        if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0)) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            res.out = "CreatePipe(stdin) failed";
            return res;
        }
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    } else {
        // 没有要喂给子进程的输入时，把它的 stdin 指到 NUL 设备。
        // 不能继承父进程的 stdin：本进程（BDS）的控制台线程正在读同一个管道读端，
        // 子进程再持有它，一来可能抢走本属于控制台的输入，二来实测会让 git 子进程
        // 在启动阶段阻塞等待（内存不涨、无 index.lock、直到超时被杀）。
        stdinRead = CreateFileW(L"NUL:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, 0, nullptr);
        if (stdinRead == INVALID_HANDLE_VALUE) stdinRead = nullptr;
    }

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError  = writePipe;
    si.hStdInput  = stdinRead ? stdinRead : GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    // 环境块：继承当前环境 + 覆盖项
    std::wstring envBlock;
    {
        LPWCH cur = GetEnvironmentStringsW();
        if (cur) {
            for (LPWCH p = cur; *p;) {
                std::wstring entry(p);
                p += entry.size() + 1;
                if (entry.empty()) break;
                auto eq = entry.find(L'=');
                if (eq != std::wstring::npos) {
                    std::string key = fromWide(entry.substr(0, eq));
                    if (opts.env.count(key)) continue; // 由覆盖项替换
                }
                envBlock += entry;
                envBlock.push_back(L'\0');
            }
            FreeEnvironmentStringsW(cur);
        }
        for (auto const& kv : opts.env) {
            envBlock += toWide(kv.first);
            envBlock.push_back(L'=');
            envBlock += toWide(kv.second);
            envBlock.push_back(L'\0');
        }
        envBlock.push_back(L'\0');
    }

    std::wstring cwdW = toWide(opts.cwd);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        envBlock.empty() ? nullptr : envBlock.data(),
        cwdW.empty() ? nullptr : cwdW.c_str(),
        &si,
        &pi
    );

    CloseHandle(writePipe);
    if (stdinRead) CloseHandle(stdinRead);

    if (!ok) {
        DWORD e = GetLastError();
        CloseHandle(readPipe);
        if (stdinWrite) CloseHandle(stdinWrite);
        res.out = "CreateProcess failed (error " + std::to_string(e) + ") for: " + exe;
        return res;
    }

    if (stdinWrite) {
        DWORD written = 0;
        WriteFile(stdinWrite, opts.stdinData.data(), static_cast<DWORD>(opts.stdinData.size()), &written, nullptr);
        CloseHandle(stdinWrite);
    }

    // 后台读取，避免管道写满导致死锁
    std::string captured;
    std::thread reader([&]() {
        char  buf[4096];
        DWORD got = 0;
        while (ReadFile(readPipe, buf, sizeof buf, &got, nullptr) && got > 0) {
            captured.append(buf, got);
        }
    });

    DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(opts.timeoutMs > 0 ? opts.timeoutMs : 120000));
    if (wait == WAIT_TIMEOUT) {
        res.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exitCode = static_cast<int>(code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (reader.joinable()) reader.join();
    CloseHandle(readPipe);

    res.out = std::move(captured);
#else
    (void)exe;
    (void)opts;
    res.out = "runProcess not implemented on this platform";
#endif
    return res;
}

Result<std::string> findGit() {
#ifdef _WIN32
    // 1) PATH 中的 git
    wchar_t buf[MAX_PATH * 2];
    DWORD   n = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH * 2, buf, nullptr);
    if (n > 0) return fromWide(std::wstring(buf, n));

    // 2) 常见安装位置
    char const* candidates[] = {
        "C:\\Program Files\\Git\\cmd\\git.exe",
        "C:\\Program Files (x86)\\Git\\cmd\\git.exe",
    };
    for (auto c : candidates) {
        if (fsu::exists(fsu::fromUtf8(c))) return std::string(c);
    }
    return Error{ErrCode::Process, "git executable not found in PATH or default locations"};
#else
    return std::string("git");
#endif
}

Result<ProcResult> gitRun(
    std::string const&                        repoDir,
    std::vector<std::string> const&           args,
    int                                       timeoutMs,
    std::map<std::string, std::string> const& env
) {
    static std::string gitExe;
    if (gitExe.empty()) {
        auto g = findGit();
        if (!g) return g.error();
        gitExe = g.value();
    }

    ProcOptions opts;
    opts.cwd       = repoDir;
    opts.args      = args;
    opts.timeoutMs = timeoutMs;
    opts.env       = env;
    // 禁止交互式凭据提示，避免服务端卡死
    opts.env["GIT_TERMINAL_PROMPT"] = "0";
    opts.env["GCM_INTERACTIVE"]     = "never";
    opts.env["GIT_PAGER"]           = "cat";
    // 排查子进程启动期阻塞：trace 会打进被捕获的输出里
    opts.env["GIT_TRACE"]              = "1";
    opts.env["GIT_TRACE_PERFORMANCE"]  = "1";
    opts.env["GIT_TRACE_SETUP"]        = "1";

    auto r = runProcess(gitExe, opts);
    if (r.timedOut) {
        std::string cmdline;
        for (auto const& a : args) {
            cmdline += a;
            cmdline.push_back(' ');
        }
        return Error{ErrCode::Process, "git timed out: " + cmdline, r.out, true};
    }
    return r;
}

} // namespace mcpsync
