// 崩溃现场捕获。
//
// BDS 被 mod 内的未处理异常带走时，CrashLogger 只留一句"进程没了"。这里用
// SetUnhandledExceptionFilter + DbgHelp 把「异常码 + 符号化调用栈」写到 mod 目录，
// 便于定位。仅在 Windows 下生效，不改变正常退出路径。
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace mcpack_sync::crash {

namespace {

constexpr int kMaxFrames = 48;

std::string moduleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleDir),
        &self
    );
    wchar_t buf[MAX_PATH]{};
    ::GetModuleFileNameW(self, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().string();
}

void writeText(char const* path, std::string const& text) {
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

LONG WINAPI onFatal(EXCEPTION_POINTERS* ep) {
    static bool entered = false;
    if (entered) return EXCEPTION_EXECUTE_HANDLER; // 二次异常直接放行，避免死循环
    entered = true;

    std::string out;
    char        line[1024];

    auto const* rec = ep ? ep->ExceptionRecord : nullptr;
    std::snprintf(
        line,
        sizeof line,
        "=== packrelay crash ===\ncode=0x%08lX address=%p\n",
        rec ? rec->ExceptionCode : 0UL,
        rec ? rec->ExceptionAddress : nullptr
    );
    out += line;

    HANDLE process = ::GetCurrentProcess();
    HANDLE thread  = ::GetCurrentThread();

    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!::SymInitializeW(process, nullptr, TRUE)) {
        std::snprintf(line, sizeof line, "SymInitialize failed: %lu\n", ::GetLastError());
        out += line;
    }

    CONTEXT ctx{};
    if (ep && ep->ContextRecord) {
        ctx = *ep->ContextRecord;
    } else {
        ::RtlCaptureContext(&ctx);
    }

    STACKFRAME64 frame{};
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;

    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512]{};
    auto*                      sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct              = sizeof(SYMBOL_INFO);
    sym->MaxNameLen                = 512;

    for (int i = 0; i < kMaxFrames; ++i) {
        if (!::StackWalk64(
                IMAGE_FILE_MACHINE_AMD64,
                process,
                thread,
                &frame,
                &ctx,
                nullptr,
                ::SymFunctionTableAccess64,
                ::SymGetModuleBase64,
                nullptr
            )) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;

        DWORD64 disp     = 0;
        char const* name = "<unknown>";
        if (::SymFromAddr(process, frame.AddrPC.Offset, &disp, sym)) {
            name = sym->Name;
        }
        IMAGEHLP_LINE64 ln{};
        ln.SizeOfStruct  = sizeof ln;
        DWORD lineDisp   = 0;
        char const* file = "";
        DWORD       no   = 0;
        if (::SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &ln)) {
            file = ln.FileName;
            no   = ln.LineNumber;
        }
        std::snprintf(
            line,
            sizeof line,
            "#%02d %p %s+0x%llx %s:%lu\n",
            i,
            reinterpret_cast<void*>(frame.AddrPC.Offset),
            name,
            static_cast<unsigned long long>(disp),
            file,
            no
        );
        out += line;
    }

    std::string path = moduleDir() + "\\packrelay-crash.log";
    writeText(path.c_str(), out);
    ::OutputDebugStringA(out.c_str());
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void install() { ::SetUnhandledExceptionFilter(&onFatal); }

} // namespace mcpack_sync::crash
