#include "mcpsync/diff.hpp"
#include "mcpsync/sha256.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace mcpsync {

std::string computeFingerprint(PackIdentity const& id, std::string_view fileSetHash) {
    std::string base;
    base.reserve(id.uuid.size() + fileSetHash.size() + 24);
    base += id.uuid;
    base.push_back('|');
    base += versionToString(id.version);
    base.push_back('|');
    base.append(fileSetHash.data(), fileSetHash.size());
    return Sha256::hex(base);
}

DiffResult diffSnapshots(std::vector<PackSnapshot> const& snaps, StateStore const& state, DiffOptions const& opts) {
    (void)opts;
    DiffResult out;

    std::set<std::string> seen;

    for (auto const& snap : snaps) {
        seen.insert(snap.id.uuid);

        Change c;
        c.id             = snap.id;
        c.dir            = snap.dir;
        c.newVersion     = snap.id.version;
        c.newFingerprint = snap.fingerprint;

        auto const* prev = state.get(snap.id.uuid);
        if (!prev) {
            c.kind   = ChangeKind::Added;
            c.reason = "new uuid " + snap.id.uuid;
            out.added++;
            out.pending.push_back(c);
        } else {
            c.oldVersion     = prev->version;
            c.oldFingerprint = prev->fingerprint;

            bool versionChanged = compareVersion(prev->version, snap.id.version) != 0;
            bool contentChanged = prev->fileSetHash != snap.fileSetHash;

            if (!versionChanged && !contentChanged && prev->fingerprint == snap.fingerprint) {
                c.kind = ChangeKind::Unchanged;
                c.reason = "uuid+version+fileSetHash identical";
                out.unchanged++;
            } else {
                c.kind = ChangeKind::Updated;
                if (versionChanged && contentChanged) {
                    c.reason = "version " + versionToString(prev->version) + " -> " + versionToString(snap.id.version) +
                               " and content hash changed";
                } else if (versionChanged) {
                    c.reason = "version " + versionToString(prev->version) + " -> " + versionToString(snap.id.version);
                } else {
                    c.reason = "content changed (version unchanged " + versionToString(snap.id.version) + ")";
                }
                out.updated++;
                out.pending.push_back(c);
            }
        }
        out.changes.push_back(std::move(c));
    }

    // 已消失的 uuid
    for (auto const* ps : state.all()) {
        if (seen.count(ps->uuid)) continue;
        Change c;
        c.kind           = ChangeKind::Removed;
        c.id.uuid        = ps->uuid;
        c.id.name        = ps->name;
        c.id.moduleUuid  = ps->moduleUuid;
        c.oldVersion     = ps->version;
        c.newVersion     = {0, 0, 0};
        c.oldFingerprint = ps->fingerprint;
        c.reason         = "uuid no longer present in resource dir";
        out.removed++;
        out.pending.push_back(c);
        out.changes.push_back(std::move(c));
    }

    std::sort(out.pending.begin(), out.pending.end(), [](Change const& a, Change const& b) {
        return a.id.uuid < b.id.uuid;
    });
    return out;
}

} // namespace mcpsync
