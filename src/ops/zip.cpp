#include "mcpsync/zip.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/sha256.hpp"

#include <cstring>
#include <fstream>

namespace mcpsync {

namespace {

uint32_t g_crcTable[256];
bool     g_crcInit = false;

void initCrc() {
    if (g_crcInit) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
    g_crcInit = true;
}

void put16(std::ofstream& f, uint16_t v) {
    char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    f.write(b, 2);
}
void put32(std::ofstream& f, uint32_t v) {
    char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF), static_cast<char>((v >> 16) & 0xFF),
                 static_cast<char>((v >> 24) & 0xFF)};
    f.write(b, 4);
}

struct CentralRec {
    std::string name;
    uint32_t    crc{0};
    uint32_t    size{0};
    uint32_t    offset{0};
};

} // namespace

uint32_t crc32Of(void const* data, size_t len, uint32_t seed) {
    initCrc();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    auto     p = static_cast<uint8_t const*>(data);
    for (size_t i = 0; i < len; ++i) c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

Status zipDirectory(std::filesystem::path const& dir, std::filesystem::path const& outZip, ZipResult* out) {
    std::vector<fsu::WalkEntry> entries;
    auto                        st = fsu::walkFiles(dir, entries);
    if (!st) return st;
    if (entries.empty()) return Error{ErrCode::Io, "nothing to zip in " + fsu::toUtf8(dir)};

    auto parent = outZip.parent_path();
    if (!parent.empty()) {
        auto d = fsu::ensureDir(parent);
        if (!d) return d;
    }

    std::ofstream f(outZip, std::ios::binary | std::ios::trunc);
    if (!f) return Error{ErrCode::Io, "cannot open zip for write: " + fsu::toUtf8(outZip)};

    std::vector<CentralRec> central;
    central.reserve(entries.size());

    Sha256           fileHash;
    std::vector<char> buf(256 * 1024);

    for (auto const& e : entries) {
        std::ifstream in(e.abs, std::ios::binary);
        if (!in) return Error{ErrCode::Io, "cannot read " + fsu::toUtf8(e.abs)};

        uint32_t offset = static_cast<uint32_t>(f.tellp());
        uint32_t crc    = 0;
        uint64_t size   = 0;

        // 本地文件头（记录写入后再回填 CRC/大小）
        std::streampos headerPos = f.tellp();
        put32(f, 0x04034b50u);
        put16(f, 20);                      // version needed
        put16(f, 0);                       // flags
        put16(f, 0);                       // method = store
        put16(f, 0);                       // time
        put16(f, 0x21);                    // date (1980-01-01)
        put32(f, 0);                       // crc (回填)
        put32(f, 0);                       // compressed size (回填)
        put32(f, 0);                       // uncompressed size (回填)
        put16(f, static_cast<uint16_t>(e.rel.size()));
        put16(f, 0);                       // extra len
        f.write(e.rel.data(), static_cast<std::streamsize>(e.rel.size()));

        for (;;) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            crc = crc32Of(buf.data(), static_cast<size_t>(got), crc);
            fileHash.update(buf.data(), static_cast<size_t>(got));
            f.write(buf.data(), got);
            size += static_cast<uint64_t>(got);
        }

        std::streampos dataEnd = f.tellp();

        // 回填 crc/大小
        f.seekp(headerPos + std::streamoff(14));
        put32(f, crc);
        put32(f, static_cast<uint32_t>(size));
        put32(f, static_cast<uint32_t>(size));
        f.seekp(dataEnd);

        CentralRec rec;
        rec.name   = e.rel;
        rec.crc    = crc;
        rec.size   = static_cast<uint32_t>(size);
        rec.offset = offset;
        central.push_back(std::move(rec));

        if (out) {
            ZipEntryInfo info;
            info.name  = e.rel;
            info.size  = size;
            info.crc32 = crc;
            out->files.push_back(std::move(info));
        }
    }

    std::streampos centralStart = f.tellp();
    for (auto const& r : central) {
        put32(f, 0x02014b50u);
        put16(f, 20);                                   // version made by
        put16(f, 20);                                   // version needed
        put16(f, 0);                                    // flags
        put16(f, 0);                                    // method
        put16(f, 0);                                    // time
        put16(f, 0x21);                                 // date
        put32(f, r.crc);
        put32(f, r.size);
        put32(f, r.size);
        put16(f, static_cast<uint16_t>(r.name.size()));
        put16(f, 0);                                    // extra
        put16(f, 0);                                    // comment
        put16(f, 0);                                    // disk
        put16(f, 0);                                    // internal attrs
        put32(f, 0);                                    // external attrs
        put32(f, r.offset);
        f.write(r.name.data(), static_cast<std::streamsize>(r.name.size()));
    }
    std::streampos centralEnd = f.tellp();

    put32(f, 0x06054b50u);
    put16(f, 0);
    put16(f, 0);
    put16(f, static_cast<uint16_t>(central.size()));
    put16(f, static_cast<uint16_t>(central.size()));
    put32(f, static_cast<uint32_t>(centralEnd - centralStart));
    put32(f, static_cast<uint32_t>(centralStart));
    put16(f, 0);

    f.flush();
    if (!f) return Error{ErrCode::Io, "zip write failed: " + fsu::toUtf8(outZip)};
    auto bytes = static_cast<uint64_t>(f.tellp());
    f.close();

    if (out) {
        out->bytes   = bytes;
        out->entries = central.size();
        out->sha256  = fileHash.hexDigest();
    }
    return {};
}

} // namespace mcpsync
