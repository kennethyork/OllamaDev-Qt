#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

QT_BEGIN_NAMESPACE
class QProcess;
QT_END_NAMESPACE

namespace odv {

// Mic capture. Recording is owned by the CALLER: start() spawns a recorder as a
// QProcess child of this object, stop() terminates THAT child and returns the
// finalised wav. Reached as Stt::Recorder (aliased below) — it lives at namespace
// scope because moc refuses Q_OBJECT inside a nested class, and we want the
// metaobject so the desktop can grow level/duration signals later.
//
// The PHP original backgrounded the recorder through a shell
// (`shell_exec("arecord … & echo $!")`) and later did `kill -INT <pid>` on the
// number it scraped back. If that pid had already exited and been recycled, the
// signal went to an unrelated process. Here we hold the QProcess handle and
// terminate the handle, so we can only ever stop the child we started — and if
// this object is destroyed the child is stopped with it rather than orphaned.
class SttRecorder : public QObject {
    Q_OBJECT
public:
    explicit SttRecorder(QObject* parent = nullptr);
    ~SttRecorder() override;

    // Spawns arecord | ffmpeg | parecord (whichever is installed) writing 16 kHz
    // mono S16_LE wav — the format whisper wants, so no resample step.
    bool start(QString* err = nullptr);

    // SIGTERM to our child, which makes every one of the three recorders rewrite
    // the RIFF/data sizes in the wav header before exiting, then SIGKILL only if
    // it ignores us. Returns the wav path ('' if nothing was captured). Safe to
    // call twice.
    QString stop();

    bool isRecording() const;

    static bool canRecord();     // is any supported recorder installed?
    static QString recorderBin();  // the one we would spawn ('' if none)

private:
    QProcess* proc_ = nullptr;  // owned; parented to this
    QString wav_;
};

// Speech-to-text, 100% local. We never run a model ourselves; we drive one of
// three local engines, in this precedence:
//
//   1. stt.host    — an OpenAI-compatible POST /v1/audio/transcriptions
//                    (whisper.cpp server, faster-whisper, vosk-server, …)
//   2. stt.command — any local CLI, with "{file}" standing in for the audio path
//   3. auto        — a whisper.cpp binary + ggml model auto-provisioned into
//                    ~/.ollamadev/stt, so /voice works with zero configuration
//
// Nothing leaves the machine on any of the three paths. Provisioning is the one
// step that touches the network, once, exactly like an `ollama pull`.
class Stt {
public:
    // Explicitly configured (a local HTTP server or a custom CLI).
    static bool enabled();

    // Usable at all: configured, OR a whisper engine is already present, so the
    // mic "just works" with no config. Callers that gate a UI want this one.
    static bool available();

    // '' on failure, with the reason in *err. Logs to the voice history.
    static QString transcribe(const QString& audioPath, QString* err = nullptr);

    // Callers say Stt::Recorder; see SttRecorder above for why it is not nested.
    using Recorder = SttRecorder;

    // The active whisper size. 'whisper-1' (the OpenAI HTTP placeholder) maps to
    // 'base' so a config written for the HTTP path still works locally.
    static QString modelSize();
    static void setModelSize(const QString& size);
    static QStringList modelSizes();

    // Voice history: append-only JSONL, newest last. Entries are
    // {ts, model, engine, text}.
    static QVector<QJsonObject> history(int n = 10);
    static void clearHistory();

    // --- auto-provisioned whisper.cpp engine ---------------------------------
    // label is "engine" or "model ggml-base.bin"; total is -1 while unknown.
    using Progress = std::function<void(const QString& label, qint64 done, qint64 total)>;

    // Fetch this platform's whisper.cpp binary + the ggml model for `size` into
    // ~/.ollamadev/stt. One-time; needs the network once, then fully local.
    static bool provision(const Progress& onProgress = {}, const QString& size = QString(),
                          QString* err = nullptr);

    static bool hasEngine();        // a whisper.cpp binary is present
    static bool hasModel();         // a ggml model is present
    static QString sttDir();        // ~/.ollamadev/stt
    static QString detectedEngine();  // whisper.cpp | openai-whisper | faster-whisper | ''
};

}  // namespace odv
