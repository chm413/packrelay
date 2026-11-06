// 统一内存分配器接入点。
//
// LeviLamina 加载 native mod 时会检查模块是否把全局 operator new/delete 桥接到
// BDS 的 IMemoryAllocator（否则报「未使用统一的内存分配运算符」并拒绝加载）。
// 该头必须在**唯一一个** TU 里、且定义 LL_MEMORY_OPERATORS 之后包含。
#define LL_MEMORY_OPERATORS
#include "ll/api/memory/MemoryOperators.h"
