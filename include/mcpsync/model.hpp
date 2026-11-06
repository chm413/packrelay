// mcpack-sync : 领域模型
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mcpsync {

using Version = std::array<int, 3>;

std::string versionToString(Version const& v);          // "1.0.3"
bool        parseVersion(std::string_view s, Version& out);
int         compareVersion(Version const& a, Version const& b);
bool        versionFromJson(void const* jv, Version& out); // 预留

// 资源包身份标识：uuid 是唯一主键，moduleUuid 用于跟随模块 uuid 的旧包
struct PackIdentity {
    std::string uuid;             // header.uuid
    std::string moduleUuid;       // 首个 resources 模块 uuid（可空）
    std::string name;             // header.name（可能是 i18n key）
    std::string description;
    Version     version{0, 0, 0};
    Version     minEngine{0, 0, 0};
    int         formatVersion{2};
    std::string productType;

    std::string shortUuid() const { return uuid.size() >= 8 ? uuid.substr(0, 8) : uuid; }
};

// 单个文件指纹
struct FileEntry {
    std::string rel;        // 相对资源包根的 '/'-分隔路径
    uint64_t    size{0};
    std::string sha256;
};

// 一次扫描得到的某个资源包快照
struct PackSnapshot {
    PackIdentity            id;
    std::filesystem::path   dir;          // 资源包根目录
    std::vector<FileEntry>  files;        // 按 rel 升序
    std::string             fileSetHash;  // 全量文件集哈希
    std::string             fingerprint;  // sha256(uuid|version|fileSetHash)
    uint64_t                totalBytes{0};
    uint64_t                newestMtime{0};

    std::string slug() const;             // 用于目录/资产名的安全标识
};

// 变更类型
enum class ChangeKind { Added, Updated, Removed, Unchanged };

const char* toString(ChangeKind k) noexcept;

struct Change {
    ChangeKind     kind{ChangeKind::Unchanged};
    PackIdentity   id;                   // Removed 时取历史 state 中的身份
    Version        oldVersion{0, 0, 0};
    Version        newVersion{0, 0, 0};
    std::string    oldFingerprint;
    std::string    newFingerprint;
    std::string    reason;               // 人类可读判定依据
    std::filesystem::path dir;           // Added/Updated 时的源目录
};

} // namespace mcpsync
