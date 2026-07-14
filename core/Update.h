#pragma once
#include <QString>
#include <functional>

namespace odv {

// SELF-UPDATE — replace this binary with the latest GitHub release.
//
// The PHP `update` did roughly this and none of its safety: no checksum, no
// backup, no rollback, no permission check, a rename() across filesystems that
// silently fails, an asset fallback that would happily install ANOTHER PLATFORM'S
// binary rather than admit it found nothing, and it wrote over an unresolved
// argv[0] — so invoked through a symlink or a relative path it overwrote the
// wrong file. Overwriting the running program is the one operation you do not get
// to be casual about: if it goes wrong the user has no working tool left to fix it
// with.
struct UpdateInfo {
    bool ok = false;
    QString error;

    QString current;      // the version we are
    QString latest;       // the version on GitHub
    bool newer = false;   // is `latest` actually newer than `current`?
    QString assetName;
    QString assetUrl;
    qint64 assetSize = 0;
    QString target;       // the file we would overwrite (resolved, never argv[0])
    QString notes;
};

class Update {
public:
    // Ask GitHub what the latest release is and which asset fits this platform.
    // Never writes anything.
    static UpdateInfo check();

    // Download it and swap it in. Refuses rather than guesses:
    //   * no asset for THIS os/arch — never "close enough", you cannot run it;
    //   * the target is not writable — say so, do not half-do it;
    //   * the download is short or empty — a truncated binary is a brick.
    //
    // The old binary is kept as <target>.bak until the new one is in place, and is
    // put back if the swap fails. The temp file is created NEXT TO the target, not
    // in /tmp, because a rename across filesystems fails and a copy is not atomic.
    static bool install(const UpdateInfo& info, QString* err,
                        const std::function<void(qint64, qint64)>& progress = {});
};

}  // namespace odv
