#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

#include "Agent.h"
#include "Backend.h"
#include "Session.h"
#include "Tui.h"

namespace odv {

struct ReplOptions {
    QString backend;    // empty => model.backend, else "ollama"
    QString model;      // empty => the session's model, else the backend default
    bool resume = false;  // -c / --continue: pick up the latest session for this cwd
    QString sessionId;    // resume a specific session
    // `ollamadev chat` is the REPL's /chat mode chosen up front: tools stay off for
    // the whole thread, so a plain conversation never triggers a tool it can't run.
    bool chat = false;
    // A prompt passed on the command line (`ollamadev chat "…"`) runs as the first
    // turn, then the loop goes interactive — a piped stdin just hits EOF and exits.
    QString initialPrompt;
};

// The interactive chat loop: banner, boxed line editor, a streaming answer with
// the collapsing thinking box, markdown, slash commands, @file mentions, and a
// Ctrl-C that INTERRUPTS the response instead of killing the process.
//
// Cancellation is cooperative all the way down (CancelToken): SIGINT only sets a
// flag, a timer turns that into a token cancel between chunks, and the agent
// stops between tool iterations. Nothing is ever killed mid-write, so an
// interrupted turn cannot leave a half-written file behind.
class Repl {
public:
    explicit Repl(ReplOptions opt);
    ~Repl();

    int run();

private:
    struct Slash {
        bool handled = false;  // recognised as a command
        bool quit = false;
        // A user's own command (.ollamadev/commands/<name>.md) expands to a prompt
        // template rather than doing something itself — so it is a TURN, not a
        // builtin. Non-empty means "run this as the user's message".
        QString prompt;
    };

    // --- input --------------------------------------------------------------
    std::optional<QString> readInput();
    LineEditor::Completion complete(const QString& line, int cursor) const;
    QStringList pathCandidates(const QString& token) const;
    static std::optional<QString> readRawLine();  // unbuffered; used for y/N prompts
    bool confirm(const QString& question) const;

    // --- turn ---------------------------------------------------------------
    void runTurn(const QString& text);
    void restyleAnswer(const QString& answer) const;
    void announceCalls(const QVector<ToolCall>& calls) const;
    void reportResults(int firstNewMessage) const;

    // --- undo ---------------------------------------------------------------
    void snapshot(const QVector<ToolCall>& calls);
    QString undoLast();
    QStringList touchedThisTurn_;

    // --- commands -----------------------------------------------------------
    Slash slash(const QString& input);
    QString cmdHelp() const;
    QString cmdModels() const;
    QString cmdModel(const QString& name);
    QString cmdContext() const;
    QString cmdPermission(const QString& args);
    QString cmdPlan(const QString& args);
    QString cmdCrew(const QString& args);
    QString cmdInit();
    QString cmdRetry();
    QString cmdCd(const QString& dir);
    QString cmdLs(const QString& dir) const;
    QString cmdOutputStyle(const QString& args);

    // --- misc ---------------------------------------------------------------
    void banner() const;
    bool preflight() const;
    void resumeNotice() const;
    QString shortCwd() const;
    QStringList installedModels() const;
    void rebuildPrompts();
    void syncSystemMessage();
    int estimateTokens() const;

    ReplOptions opt_;
    Session session_;
    std::unique_ptr<Agent> agent_;
    QString backendId_;
    QString model_;
    bool chatOnly_ = false;
    bool interrupted_ = false;
    QStringList history_;
    QString agentPrompt_;
    QString chatPrompt_;
    mutable QStringList modelCache_;
};

}  // namespace odv
