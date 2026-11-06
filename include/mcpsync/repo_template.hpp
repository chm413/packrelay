// mcpack-sync : 目标仓库模板（workflow / 发布脚本）
#pragma once

#include <filesystem>

#include "mcpsync/result.hpp"

namespace mcpsync {

// 在 repoRoot 内落地 .github/workflows/release.yml、scripts/ci-release.sh、README.md
Status ensureRepoTemplates(std::filesystem::path const& repoRoot);

} // namespace mcpsync
