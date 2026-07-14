#pragma once
#include <QString>
#include <QVector>

#include "Backend.h"

namespace odv {

// The core LLM loop: one agent = one backend + one model + a system prompt.
//
// Tool-calling is NATIVE ONLY. Ollama applies each model's own chat template and
// parses its tool syntax server-side into structured tool_calls, so we never
// re-scan the model's prose with our own regexes. The PHP original grew text and
// structured fallback protocols for that and they were a net loss: a model whose
// emission format we didn't recognise would "call a tool" that silently never
// ran, and showing the text protocol in the prompt lured capable models into
// emitting pseudo-calls that bypassed the real API. A model without tool support
// is therefore an ERROR here, not a reason to start guessing.
class Agent {
public:
    Agent(const QString& backendId, const QString& model);

    QString model() const { return model_; }
    QString backendId() const { return backendId_; }

    void setSystemPrompt(const QString& s) { systemPrompt_ = s; }
    QString systemPrompt() const { return systemPrompt_; }

    // Base prompt + OLLAMADEV.md / .ollamadev.md project context, if present.
    QString buildSystemPrompt(const QString& projectRoot) const;

    // One assistant turn. Appends the assistant message (content + any raw tool
    // calls) to `messages`, so the caller can hand the same vector straight back
    // for the next turn.
    ChatTurn turn(QVector<ChatMessage>& messages, const StreamSink& sink, const CancelToken& cancel);

    // Execute every call in a batch and append the tool results to `messages`.
    //
    // Ordering contract: results are appended in the model's original call order,
    // always. Consecutive READ-ONLY calls (view/ls/grep/glob — anything with
    // mutates=false) run CONCURRENTLY, because they cannot observe each other. A
    // mutating call (write/edit/rm/bash/…) runs SERIALLY: the read batch before it
    // is drained first, then it runs alone. That preserves every happens-before the
    // model could reasonably assume — e.g. write(f) then view(f) still reads the
    // new bytes — while still parallelising the common "read six files" fan-out.
    void executeCalls(const QVector<ToolCall>& calls, QVector<ChatMessage>& messages);

    // Called at the top of every loop iteration, BEFORE the model is asked
    // anything. Whatever it returns is appended as a user message, so the caller
    // can get a word in edgeways mid-run — this is how a human steers a crew coder
    // that is already working. Return an empty string to say nothing.
    //
    // Between iterations, deliberately: a coder must never be interrupted between
    // deciding to write a file and writing it.
    using Interject = std::function<QString()>;

    // The full bounded loop: turn -> execute -> turn -> … until the model stops
    // calling tools or maxIterations is hit. Returns the final assistant text.
    QString loop(QVector<ChatMessage>& messages, int maxIterations, const StreamSink& sink,
                 const CancelToken& cancel, const Interject& interject = {});

private:
    // Ensure messages[0] is our system message.
    void ensureSystem(QVector<ChatMessage>& messages) const;

    QString backendId_;
    QString model_;
    QString systemPrompt_;
};

}  // namespace odv
