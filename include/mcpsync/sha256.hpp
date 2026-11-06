// mcpack-sync : SHA-256（自实现，零第三方依赖，便于交叉编译与审计）
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mcpsync {

class Sha256 {
public:
    Sha256();
    void update(void const* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::string hexDigest();     // 终止并返回 64 位小写十六进制

    static std::string hex(std::string_view data);

private:
    void transform(uint8_t const* block);

    uint32_t    mState[8];
    uint64_t    mBitLen{0};
    uint8_t     mBuf[64];
    size_t      mBufLen{0};
    bool        mFinal{false};
};

} // namespace mcpsync
