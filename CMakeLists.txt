cmake_minimum_required(VERSION 3.20)
project(mcpack-sync VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(MCPSYNC_VERSION "${PROJECT_VERSION}" CACHE STRING "version string reported by CLI")

add_library(mcpsync_core STATIC
    src/core/json.cpp
    src/core/sha256.cpp
    src/core/fsutil.cpp
    src/core/logger.cpp
    src/core/model.cpp
    src/core/config.cpp
    src/core/scanner.cpp
    src/core/state.cpp
    src/core/diff.cpp
    src/core/gitauth.cpp
    src/net/process.cpp
    src/net/http.cpp
    src/net/github.cpp
    src/net/mirror.cpp
    src/ops/git.cpp
    src/ops/cdn.cpp
    src/ops/zip.cpp
    src/ops/repo_template.cpp
    src/ops/pipeline.cpp
)

target_include_directories(mcpsync_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(mcpsync_core PUBLIC
    MCPSYNC_VERSION="${MCPSYNC_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(mcpsync_core PUBLIC /utf-8 /W4 /permissive-)
else()
    target_compile_options(mcpsync_core PUBLIC -Wall -Wextra -Wno-unused-parameter)
endif()

if(WIN32)
    target_link_libraries(mcpsync_core PUBLIC winhttp ws2_32)
endif()

find_package(Threads REQUIRED)
target_link_libraries(mcpsync_core PUBLIC Threads::Threads)

add_executable(mcpsync src/cli/main.cpp)
target_link_libraries(mcpsync PRIVATE mcpsync_core)
# 对外产物名是 packrelay；target 名沿用 mcpsync，避免批量改链接引用
set_target_properties(mcpsync PROPERTIES OUTPUT_NAME packrelay)

add_executable(mcpsync-tests tests/test_main.cpp)
target_link_libraries(mcpsync-tests PRIVATE mcpsync_core)

enable_testing()
add_test(NAME unit COMMAND mcpsync-tests)

# ---------------------------------------------------------------------------
# LeviLamina mod **只由 xmake 构建**（见 xmake.lua），这里刻意不提供 mod 目标。
#
# 为什么不用 CMake：mod 需要 LL 的整套包生态 —— bedrockdata 预编译头、entt/gsl/glm/
# leveldb 等十几个依赖、prelink.exe + bedrock_runtime_data（生成 bedrock_runtime_api.lib）、
# 以及 symbolprovider（把 delay-load 的 bedrock_runtime.dll 解析到 BDS 真实地址）。
# 这些由 levimc-repo 的 `levilamina` 包一次性解析，手工在 CMake 里拼不现实。
#
# 历史上这里曾有一个 MCPSYNC_BUILD_MOD 分支，但它是错的且从未成功过：
#   - 只列了 src/mod/levilamina_mod.cpp，漏了 crash_guard.cpp / memory_operators.cpp
#     （后者缺失会被 LL 以「未使用统一的内存分配运算符」拒绝加载）
#   - 链 bedrock_server_api / bedrock_server_var，而不是 prelink 生成的导入库
#   - 输出目录名 mcpack_sync 与 manifest 的 name=packrelay 不一致
# 保留它只会诱导别人走一条产出「LL 拒绝加载的 dll」的路，因此整段删除。
# ---------------------------------------------------------------------------

message(STATUS "mcpack-sync ${MCPSYNC_VERSION} | LL mod: built by xmake (see xmake.lua)")
