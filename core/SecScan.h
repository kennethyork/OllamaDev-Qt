#pragma once
#include <QString>
#include <QVector>

namespace odv {

// One hit. `redacted` is the ONLY view of the match anyone downstream ever gets:
// a finding travels into logs, the board, an auditor prompt and possibly a model
// running in the cloud, so it must never carry the credential it found.
struct Finding {
    QString rule;
    QString severity;  // high|med|low
    int line = 0;
    QString redacted;
};

// SECSCAN — dependency-free secret + unsafe-sink scanner. Catches hardcoded
// credentials before they land, in a commit or in a crew coder's branch. The
// crew Auditor uses it as a hard gate (a secret-bearing changeset never
// auto-lands); the `scan` command runs it on demand.
//
// Rules are tuned for precision over recall on purpose: a scanner that cries
// wolf gets switched off, and a switched-off scanner catches nothing.
class SecScan {
public:
    static QVector<Finding> scanText(const QString& text);

    // Unified diff: only ADDED lines are inspected, so findings describe what a
    // change INTRODUCES rather than what it merely touched. Line numbers are
    // resolved against the new file via the @@ hunk headers.
    static QVector<Finding> scanDiff(const QString& diff);

    // Skips binaries and very large files — neither is where a leaked key lives,
    // and both would only slow the commit gate down.
    static QVector<Finding> scanFile(const QString& path);
};

}  // namespace odv
