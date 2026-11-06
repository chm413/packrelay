// mcpack-sync : 变更判定（uuid + 版本 + 内容哈希 三重比对）
#pragma once

#include <vector>

#include "mcpsync/model.hpp"
#include "mcpsync/state.hpp"

namespace mcpsync {

struct DiffOptions {
    bool matchModuleUuid{true};
};

struct DiffResult {
    std::vector<Change> changes;     // Added / Updated / Removed（Unchanged 也会给出，便于审计）
    std::vector<Change> pending;     // 需要推送的子集，按 uuid 升序
    size_t              unchanged{0};
    size_t              added{0};
    size_t              updated{0};
    size_t              removed{0};
};

// 与历史状态比对；绝不依赖 mtime，全部基于 uuid/version/fileSetHash
DiffResult diffSnapshots(std::vector<PackSnapshot> const& snaps, StateStore const& state, DiffOptions const& opts = {});

// 指纹计算：sha256(uuid + '|' + version + '|' + fileSetHash)
std::string computeFingerprint(PackIdentity const& id, std::string_view fileSetHash);

} // namespace mcpsync
