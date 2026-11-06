// mcpack-sync : 极简 ZIP 写入（store 模式，资源包内容本身已压缩，无需再 deflate）
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "mcpsync/result.hpp"

namespace mcpsync {

struct ZipEntryInfo {
    std::string name;
    uint64_t    size{0};
    uint32_t    crc32{0};
};

struct ZipResult {
    uint64_t                 bytes{0};
    size_t                   entries{0};
    std::string              sha256;
    std::vector<ZipEntryInfo> files;
};

// 将 dir 下所有文件按相对路径写入 zip；zip 内路径统一使用 '/'，根目录不额外套一层。
Status zipDirectory(std::filesystem::path const& dir, std::filesystem::path const& outZip, ZipResult* out = nullptr);

uint32_t crc32Of(void const* data, size_t len, uint32_t seed = 0);

} // namespace mcpsync
