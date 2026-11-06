// mcpack-sync : 文件系统与路径工具（Windows 优先，路径内部统一 UTF-8）
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mcpsync/result.hpp"

namespace mcpsync::fsu {

namespace fs = std::filesystem;

// UTF-8 <-> 原生路径
fs::path    fromUtf8(std::string_view s);
std::string toUtf8(fs::path const& p);

bool exists(fs::path const& p);
bool isDirectory(fs::path const& p);

Status ensureDir(fs::path const& p);
Status readFile(fs::path const& p, std::string& out);
Status writeFile(fs::path const& p, std::string_view data);
// 原子写：写临时文件 -> FlushFileBuffers -> MoveFileEx(REPLACE_EXISTING)
Status writeFileAtomic(fs::path const& p, std::string_view data);

uint64_t fileSize(fs::path const& p);
// 自 1601-01-01 起的 100ns 计数，用于变更检测的快速路径
uint64_t fileWriteTime(fs::path const& p);

std::string relativeUtf8(fs::path const& base, fs::path const& target);
std::string nowIso8601();
uint64_t    nowUnix();

// 递归枚举普通文件（不跟随符号链接），rel 路径使用 '/' 分隔
struct WalkEntry {
    fs::path    abs;
    std::string rel;
    uint64_t    size{0};
    uint64_t    mtime{0};
};
Status walkFiles(
    fs::path const&                                  root,
    std::vector<WalkEntry>&                          out,
    std::function<bool(std::string const& rel)> const& filter = {}
);

// 通配符匹配：支持 '*'（不跨 /）、'**'（跨 /）、'?'；大小写不敏感
bool matchGlob(std::string_view pattern, std::string_view text);

// 递归复制目录（覆盖目标已存在文件）
Status copyTree(fs::path const& src, fs::path const& dst);
// 让 dst 与 src 完全一致：复制新增/变更，删除 dst 中多余项
Status mirrorTree(fs::path const& src, fs::path const& dst);

Status removeAll(fs::path const& p);

std::string sanitizeName(std::string_view s);   // 去掉路径非法字符
std::string slugify(std::string_view s);

} // namespace mcpsync::fsu
