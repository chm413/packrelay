-- packrelay : LeviLamina mod 构建脚本（xmake）
--
-- 为什么用 xmake 而不是 CMake 来编 mod：
--   LL 的 mod 必须用 MSVC 构建并链接 LeviLamina.lib，而头文件树里含 entt / gsl / glm /
--   leveldb / rapidjson 等十几个第三方依赖。xmake 通过 levimc-repo 的 `levilamina` 包
--   一次性解析「BDS 头文件(bedrockdata，预编译) + 这些依赖 + LeviLamina import lib
--   + symbolprovider(prelink.exe / bedrock_runtime_data)」，避免手工拼十几条 include 路径。
--
-- 用法：
--   xmake f -c -y --target_type=server
--   xmake -y
-- 产物：build/windows/x64/release/packrelay.dll
--   发布时重命名为 packrelay.mod.dll（必须与 manifest.json 的 entry 一致）
--
-- CMake 仍然负责核心库 / CLI / 单测（见 CMakeLists.txt），两边共用同一份源码：
--   CMake → packrelay.exe + mcpsync-tests（跨平台，零第三方依赖）
--   xmake → packrelay.dll（仅 Windows + MSVC，依赖 LL 生态）

add_rules("mode.release", "mode.debug")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

-- 与目标服务器上的 LeviLamina 26.60.2 严格对齐，避免 ABI/接口漂移。
-- 该版本号需存在于 levimc-repo 的 versions.txt 中。
add_requires("levilamina 26.60.2", {configs = {target_type = get_config("target_type")}})
add_requires("levibuildscript")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("packrelay")
    -- linkrule 会调 prelink.exe + bedrock_runtime_data 生成 bedrock_runtime_api.lib，
    -- 两者由 add_requires("levibuildscript") / levilamina 的 symbolprovider 依赖带进包环境。
    -- 这里只 add_packages("levilamina")，与官方模板一致；把 levibuildscript 也 add_packages
    -- 会因为它没有 links/includedirs 而告警。
    add_rules("@levibuildscript/linkrule")
    add_packages("levilamina")

    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    set_optimize("faster")
    add_includedirs("include")

    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE", "WIN32_LEAN_AND_MEAN")
        -- LL 自身用 /EHa 构建，mod 侧保持一致；不关 C++ EH，因为 core 里用到 std::stoi 等可能抛异常的接口
        add_cxflags("/EHa", "/utf-8", "/W3", "/wd4265", "/wd4267")
        -- 网络 / 凭据 / 崩溃符号化用到的系统库
        add_syslinks("winhttp", "ws2_32", "user32", "advapi32", "shell32", "ole32", "crypt32", "dbghelp")
    end

    -- mod 适配层 + 全部 core（cli/main.cpp 含 main()，必须排除）
    add_files(
        "src/core/*.cpp",
        "src/net/*.cpp",
        "src/ops/*.cpp",
        "src/mod/*.cpp"
    )
    add_headerfiles("include/mcpsync/*.hpp")
