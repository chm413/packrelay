// mcpack-sync : 资源目录扫描器
#pragma once

#include <filesystem>
#include <vector>

#include "mcpsync/config.hpp"
#include "mcpsync/model.hpp"
#include "mcpsync/result.hpp"

namespace mcpsync {

struct ScanOptions {
    std::vector<std::string> ignorePatterns;
    bool                     requireManifest{true};
    bool                     matchModuleUuid{true};
    bool                     computeHashes{true};
};

struct ScanReport {
    std::vector<PackSnapshot> packs;      // 按 uuid 升序
    std::vector<Change>       conflicts;  // 同 uuid 多目录时保留的记录
    size_t                    filesHashed{0};
    uint64_t                  bytesRead{0};
    uint64_t                  elapsedMs{0};
    std::vector<std::string>  warnings;
};

// 解析单个 manifest.json
Result<PackIdentity> parseManifest(std::filesystem::path const& manifestPath);

// 扫描资源根目录，识别所有含 manifest.json 的目录
Result<ScanReport> scanResourceDir(std::filesystem::path const& root, ScanOptions const& opts);

// 对单个资源包目录做快照（供测试与增量刷新使用）
Result<PackSnapshot> snapshotPack(std::filesystem::path const& dir, ScanOptions const& opts);

} // namespace mcpsync
