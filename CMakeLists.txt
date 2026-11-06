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
# LeviLamina mod 目标：仅在 MSVC + 已提供 LL SDK（头文件 + 导入库）时构建。
# 原因：LeviLamina.dll / bedrock_server.exe 由 MSVC 编译并导出 MSVC 修饰名
#       （如 ?getModDir@Mod@mod@ll@@QEAAAEBVpath@filesystem@std@@XZ），
#       MinGW 的 Itanium ABI 无法链接这些符号，必须使用 MSVC 工具链。
# ---------------------------------------------------------------------------
option(MCPSYNC_BUILD_MOD "Build the LeviLamina mod target" OFF)
set(LL_SDK_DIR "" CACHE PATH "LeviLamina SDK dir (contains include/ and lib/)")

if(MCPSYNC_BUILD_MOD)
    if(NOT MSVC)
        message(FATAL_ERROR
            "MCPSYNC_BUILD_MOD=ON requires MSVC (cl.exe). "
            "LeviLamina exports MSVC-mangled symbols; MinGW cannot link them.")
    endif()
    if(NOT EXISTS "${LL_SDK_DIR}/include/ll/api/mod/Mod.h")
        message(FATAL_ERROR "LL_SDK_DIR='${LL_SDK_DIR}' does not look like a LeviLamina SDK")
    endif()

    add_library(mcpack_sync_mod SHARED src/mod/levilamina_mod.cpp)
    target_link_libraries(mcpack_sync_mod PRIVATE mcpsync_core)
    target_include_directories(mcpack_sync_mod PRIVATE
        ${LL_SDK_DIR}/include
        ${LL_SDK_DIR}/include/mc
    )
    target_link_directories(mcpack_sync_mod PRIVATE ${LL_SDK_DIR}/lib)
    target_link_libraries(mcpack_sync_mod PRIVATE LeviLamina bedrock_server_api bedrock_server_var)
    set_target_properties(mcpack_sync_mod PROPERTIES SUFFIX ".mod.dll" PREFIX "")

    # mod 的 manifest.json 与默认配置随产物一起落地
    add_custom_command(TARGET mcpack_sync_mod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mcpack_sync_mod>/mcpack_sync"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/src/mod/manifest.json"
                "$<TARGET_FILE_DIR:mcpack_sync_mod>/mcpack_sync/manifest.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/config/config.example.json"
                "$<TARGET_FILE_DIR:mcpack_sync_mod>/mcpack_sync/config.json"
    )
endif()

message(STATUS "mcpack-sync ${MCPSYNC_VERSION} | mod target: ${MCPSYNC_BUILD_MOD}")
