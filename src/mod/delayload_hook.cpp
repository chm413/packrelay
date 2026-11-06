// bedrock_runtime.dll 的延迟加载钩子。
//
// prelink 生成的 bedrock_runtime_api.lib 是对 bedrock_runtime.dll 的纯导入库，
// 该模块由加载器在运行时提供（不是磁盘文件）。delayimp 默认用 LoadLibraryA 去找，
// 而 LeviLamina/PreLoader 侧的虚拟模块只挂在 W 系列入口上，所以这里显式接管
// dliNotePreLoadLibrary，用 LoadLibraryW 再试一次。
#include <windows.h>

#include <delayimp.h>

#include <cstring>

namespace {

FARPROC WINAPI dliNotify(unsigned notify, PDelayLoadInfo info) {
    if (notify == dliNotePreLoadLibrary && info && info->szDll) {
        if (_stricmp(info->szDll, "bedrock_runtime.dll") == 0) {
            // 先看是否已经在进程里（加载器可能已经建好虚拟模块）
            if (HMODULE h = ::GetModuleHandleW(L"bedrock_runtime.dll")) {
                return reinterpret_cast<FARPROC>(h);
            }
            if (HMODULE h = ::LoadLibraryW(L"bedrock_runtime.dll")) {
                return reinterpret_cast<FARPROC>(h);
            }
        }
    }
    return nullptr;
}

} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = &dliNotify;
