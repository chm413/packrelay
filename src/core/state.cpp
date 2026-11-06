#include "mcpsync/state.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/json.hpp"
#include "mcpsync/logger.hpp"

namespace mcpsync {

namespace {

json::Value versionJson(Version const& v) {
    json::Array a;
    a.emplace_back(v[0]);
    a.emplace_back(v[1]);
    a.emplace_back(v[2]);
    return json::Value(std::move(a));
}

Version versionFrom(json::Value const& v) {
    Version out{0, 0, 0};
    if (v.isArray()) {
        int i = 0;
        for (auto const& e : v.asArray()) {
            if (i >= 3) break;
            out[i++] = static_cast<int>(e.asInt(0));
        }
    }
    return out;
}

} // namespace

StateStore::StateStore(std::filesystem::path path) : mPath(std::move(path)) {}

Result<bool> StateStore::load() {
    if (!fsu::exists(mPath)) {
        mPacks.clear();
        return false;
    }
    std::string text;
    auto        st = fsu::readFile(mPath, text);
    if (!st) return st.error();

    json::Value root;
    std::string err;
    if (!json::parse(text, root, err)) {
        return Error{ErrCode::Parse, "state file corrupt: " + fsu::toUtf8(mPath), err};
    }
    mPacks.clear();
    mLastFullScan = root.at("lastFullScan").asString();

    for (auto const& e : root.at("packs").asArray()) {
        if (!e.isObject()) continue;
        PackState ps;
        ps.uuid        = e.at("uuid").asString();
        ps.moduleUuid  = e.at("moduleUuid").asString();
        ps.name        = e.at("name").asString();
        ps.slug        = e.at("slug").asString();
        ps.version     = versionFrom(e.at("version"));
        ps.fileSetHash = e.at("fileSetHash").asString();
        ps.fingerprint = e.at("fingerprint").asString();
        ps.fileCount   = static_cast<size_t>(e.at("fileCount").asInt(0));
        ps.totalBytes  = static_cast<uint64_t>(e.at("totalBytes").asInt(0));
        ps.lastScanAt  = e.at("lastScanAt").asString();

        auto const& p = e.at("pushed");
        if (p.isObject()) {
            ps.pushed.commit = p.at("commit").asString();
            ps.pushed.remote = p.at("remote").asString();
            ps.pushed.branch = p.at("branch").asString();
            ps.pushed.at     = p.at("at").asString();
        }
        auto const& r = e.at("release");
        if (r.isObject()) {
            ps.release.tag         = r.at("tag").asString();
            ps.release.assetName   = r.at("assetName").asString();
            ps.release.assetUrl    = r.at("assetUrl").asString();
            ps.release.publicUrl   = r.at("publicUrl").asString();
            ps.release.publishedAt = r.at("publishedAt").asString();
        }
        ps.releasePending = e.at("releasePending").asBool(false);
        if (!ps.uuid.empty()) mPacks[ps.uuid] = std::move(ps);
    }
    mDirty = false;
    return true;
}

Status StateStore::save() const {
    json::Value root = json::Value::object();
    root.set("format", json::Value(kFormatVersion));
    root.set("lastFullScan", json::Value(mLastFullScan));

    json::Array packs;
    for (auto const& kv : mPacks) {
        auto const& ps = kv.second;
        json::Value e  = json::Value::object();
        e.set("uuid", json::Value(ps.uuid));
        e.set("moduleUuid", json::Value(ps.moduleUuid));
        e.set("name", json::Value(ps.name));
        e.set("slug", json::Value(ps.slug));
        e.set("version", versionJson(ps.version));
        e.set("fileSetHash", json::Value(ps.fileSetHash));
        e.set("fingerprint", json::Value(ps.fingerprint));
        e.set("fileCount", json::Value(static_cast<int64_t>(ps.fileCount)));
        e.set("totalBytes", json::Value(static_cast<uint64_t>(ps.totalBytes)));
        e.set("lastScanAt", json::Value(ps.lastScanAt));

        json::Value pushed = json::Value::object();
        pushed.set("commit", json::Value(ps.pushed.commit));
        pushed.set("remote", json::Value(ps.pushed.remote));
        pushed.set("branch", json::Value(ps.pushed.branch));
        pushed.set("at", json::Value(ps.pushed.at));
        e.set("pushed", std::move(pushed));

        json::Value rel = json::Value::object();
        rel.set("tag", json::Value(ps.release.tag));
        rel.set("assetName", json::Value(ps.release.assetName));
        rel.set("assetUrl", json::Value(ps.release.assetUrl));
        rel.set("publicUrl", json::Value(ps.release.publicUrl));
        rel.set("publishedAt", json::Value(ps.release.publishedAt));
        e.set("release", std::move(rel));
        e.set("releasePending", json::Value(ps.releasePending));

        packs.push_back(std::move(e));
    }
    root.set("packs", json::Value(std::move(packs)));

    return fsu::writeFileAtomic(mPath, root.dump(2) + "\n");
}

PackState const* StateStore::get(std::string const& uuid) const {
    auto it = mPacks.find(uuid);
    return it == mPacks.end() ? nullptr : &it->second;
}

PackState* StateStore::get(std::string const& uuid) {
    auto it = mPacks.find(uuid);
    return it == mPacks.end() ? nullptr : &it->second;
}

void StateStore::put(PackState st) {
    mPacks[st.uuid] = std::move(st);
    mDirty          = true;
}

void StateStore::erase(std::string const& uuid) {
    if (mPacks.erase(uuid)) mDirty = true;
}

std::vector<PackState const*> StateStore::all() const {
    std::vector<PackState const*> out;
    out.reserve(mPacks.size());
    for (auto const& kv : mPacks) out.push_back(&kv.second);
    return out;
}

} // namespace mcpsync
