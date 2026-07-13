#pragma once
#include <QString>
#include <QVector>

namespace odv {

// One diagnostic, already in the wire format's coordinate system: LINE AND
// COLUMN ARE 0-BASED. Every linter we drive reports 1-based lines, so the
// conversion happens once, at the parse, and nowhere else — a half-converted
// position is the classic LSP bug where every squiggle sits one line low.
struct Diagnostic {
    int line = 0;
    int col = 0;
    int endLine = 0;
    int endCol = 0;
    int severity = 1;  // LSP: 1 error, 2 warning, 3 info, 4 hint
    QString message;
    QString source;  // which tool said so: "php -l", "gcc", "rustc", …
};

// Diagnostics from REAL compilers and linters — php -l, python -m py_compile,
// go vet, gcc/g++ -fsyntax-only, rustc — not from a model's opinion of the code.
//
// Every one is spawned as a QProcess child with an argv ARRAY (never a shell
// string), and every flag used here was checked against the tool's own --help
// rather than remembered: `python` alone does not exist on most Linux boxes
// (it is python3), and `go vet` takes packages, not files.
class Linters {
public:
    // Lint `text` AS IF it were the contents of `path`.
    //
    // The buffer, not the file on disk: an editor wants squiggles on the code in
    // front of the user, which is normally unsaved. So the text is written to a
    // private temp dir and the linter runs there — which also keeps each tool's
    // droppings (__pycache__, .rmeta, object files) out of the user's tree.
    static QVector<Diagnostic> run(const QString& path, const QString& text);

    // Name of the tool that would run for this path, or "" when none is installed.
    static QString toolFor(const QString& path);
};

struct LspOptions {
    // 0 = stdio. stdio is what editors actually launch, so it is the default; TCP
    // exists for debugging and for editors that prefer a socket.
    int port = 0;
    QString backend;  // defaults to model.backend
    QString model;    // defaults to the backend's default model
};

// A Language Server, so any LSP-speaking editor can use this agent directly:
// AI completion from the model, plus hover, go-to-definition and real diagnostics.
//
// STDOUT IS THE PROTOCOL on the stdio path. One stray byte — a qDebug, a printf,
// a Qt warning, a child process that inherited fd 1 — desynchronises the client's
// framing parser for the rest of the session, and the editor simply goes quiet.
// serve() therefore points fd 1 at fd 2 for its whole life and keeps the real
// channel as a private descriptor that only the frame writer holds.
class LspServer {
public:
    static int serve(const LspOptions& o);
};

}  // namespace odv
