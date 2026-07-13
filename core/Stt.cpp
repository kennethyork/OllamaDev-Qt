#include "Stt.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include "Config.h"
#include "Json.h"
#include "Version.h"

namespace odv {

namespace {

// Whisper is trained on 16 kHz mono; capturing anything else just forces the
// engine to resample and buys nothing.
constexpr int kRate = 16000;
constexpr int kChannels = 1;

// A recording nobody stops must still end. arecord/ffmpeg take a duration flag
// themselves, which keeps the cap working even when the caller is blocked on a
// synchronous read (the CLI's "press Enter to stop") and no Qt event loop is
// spinning to fire a timer.
int maxSeconds() {
    const int n = Config::integer(QStringLiteral("stt.maxSeconds"), 300);
    return n > 0 ? n : 300;
}

QString which(const QString& bin) { return QStandardPaths::findExecutable(bin); }

QString trimSlash(QString s) {
    while (s.size() > 1 && (s.endsWith(u'/') || s.endsWith(u'\\'))) s.chop(1);
    return s;
}

// Run a program to completion and hand back stdout. No shell anywhere in this
// file: every argument is passed as its own argv entry, so an audio path with a
// space, a quote or a `;` in it is data, never syntax.
QString runCapture(const QString& program, const QStringList& args, int timeoutMs,
                   QString* err = nullptr) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args);
    if (!p.waitForStarted(5000)) {
        if (err) *err = QStringLiteral("could not start %1").arg(program);
        return {};
    }
    if (!p.waitForFinished(timeoutMs)) {
        // Our child, our cleanup — kill the handle we hold, never a name.
        p.kill();
        p.waitForFinished(2000);
        if (err) *err = QStringLiteral("%1 timed out").arg(program);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (err) {
            const QString e = QString::fromUtf8(p.readAllStandardError()).trimmed();
            *err = e.isEmpty() ? QStringLiteral("%1 failed (exit %2)").arg(program).arg(p.exitCode())
                               : e;
        }
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput());
}

// Split a configured command line into argv WITHOUT a shell: honours single and
// double quotes and backslash escapes. stt.command is user-authored, but the
// audio path is substituted into an already-split token (see viaCommand), so a
// hostile filename can never grow into extra arguments.
QStringList splitCommand(const QString& cmd) {
    QStringList out;
    QString cur;
    bool have = false;
    QChar quote;
    for (int i = 0; i < cmd.size(); ++i) {
        const QChar c = cmd.at(i);
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else {
                cur += c;
            }
            continue;
        }
        if (c == u'\'' || c == u'"') {
            quote = c;
            have = true;
            continue;
        }
        if (c == u'\\' && i + 1 < cmd.size()) {
            cur += cmd.at(++i);
            have = true;
            continue;
        }
        if (c.isSpace()) {
            if (have || !cur.isEmpty()) {
                out << cur;
                cur.clear();
                have = false;
            }
            continue;
        }
        cur += c;
        have = true;
    }
    if (have || !cur.isEmpty()) out << cur;
    return out;
}

QString bundledDir() {
    const QString d = QString::fromLocal8Bit(qgetenv("OLLAMADEV_STT_DIR")).trimmed();
    return (!d.isEmpty() && QFileInfo(d).isDir()) ? trimSlash(d) : QString();
}

// The release-asset name for this platform: whisper-<os>-<arch>[.exe].
QString platformTarget() {
    const QString k = QSysInfo::kernelType();
    const QString os = (k == QLatin1String("winnt"))  ? QStringLiteral("windows")
                       : (k == QLatin1String("darwin")) ? QStringLiteral("mac")
                                                        : QStringLiteral("linux");
    const QString cpu = QSysInfo::currentCpuArchitecture().toLower();
    const QString arch = (cpu.contains(QLatin1String("arm")) ||
                          cpu.contains(QLatin1String("aarch64")))
                             ? QStringLiteral("arm64")
                             : QStringLiteral("x64");
    return QStringLiteral("whisper-%1-%2%3")
        .arg(os, arch, os == QLatin1String("windows") ? QStringLiteral(".exe") : QString());
}

QStringList engineDirs() {
    QStringList d;
    const QString b = bundledDir();
    if (!b.isEmpty()) d << b;  // a desktop bundle wins over the fetched copy
    d << Stt::sttDir();
    return d;
}

// A usable whisper.cpp binary: bundled → provisioned → PATH, or ''.
QString whisperCppBin() {
    const QString name = platformTarget();
    for (const QString& dir : engineDirs()) {
        const QString p = dir + u'/' + name;
        if (QFileInfo(p).isFile()) {
            QFile::setPermissions(p, QFile::permissions(p) | QFileDevice::ExeOwner |
                                         QFileDevice::ExeGroup | QFileDevice::ExeOther);
            return p;
        }
    }
    for (const QString& b : {QStringLiteral("whisper-cli"), QStringLiteral("whisper-cpp")}) {
        const QString p = which(b);
        if (!p.isEmpty()) return p;
    }
    return {};
}

QString ggmlModelName(const QString& size) {
    const QString s = size.isEmpty() ? Stt::modelSize() : size;
    // whisper.cpp ships the turbo weights under their full name.
    return QStringLiteral("ggml-%1.bin")
        .arg(s == QLatin1String("turbo") ? QStringLiteral("large-v3-turbo") : s);
}

// The ggml model for `size`, else ANY ggml-*.bin we have, so a provisioned model
// is still used when stt.model names a size that was never downloaded.
QString ggmlModelFile(const QString& size = QString()) {
    const QString name = ggmlModelName(size);
    const QStringList dirs = engineDirs();
    for (const QString& dir : dirs) {
        const QString p = dir + u'/' + name;
        if (QFileInfo(p).isFile()) return p;
    }
    for (const QString& dir : dirs) {
        const QStringList found =
            QDir(dir).entryList({QStringLiteral("ggml-*.bin")}, QDir::Files, QDir::Name);
        if (!found.isEmpty()) return dir + u'/' + found.first();
    }
    return {};
}

QString historyFile() {
    return trimSlash(Config::dataDir()) + QStringLiteral("/voice-history.jsonl");
}

void logHistory(const QString& text, const QString& model, const QString& engine) {
    const QString t = text.trimmed();
    if (t.isEmpty()) return;
    const QString file = historyFile();
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QJsonObject entry{{"ts", QDateTime::currentSecsSinceEpoch()},
                            {"model", model},
                            {"engine", engine},
                            {"text", t}};
    f.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
}

// stt.language: '' means auto-detect. whisper.cpp does NOT treat a missing -l as
// auto — its default is "en", so an unset language would silently force English
// on a German speaker. Pass the engine's explicit auto token instead.
QString language() { return Config::str(QStringLiteral("stt.language")).trimmed(); }

QString scratchBase() {
    return QDir::tempPath() + QStringLiteral("/odv-stt-%1-%2")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(QDateTime::currentMSecsSinceEpoch());
}

// --- the three transcription paths ------------------------------------------

// POST the audio to a local OpenAI-compatible endpoint.
QString viaHttp(const QString& host, const QString& file, QString* err) {
    QString url = trimSlash(host);
    if (!url.endsWith(QLatin1String("/transcriptions"))) {
        url += QStringLiteral("/v1/audio/transcriptions");
    }

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Authorization", "Bearer local");
    // stt.host is normally http://localhost, but if someone points it at a remote
    // box over TLS we verify the peer like any other client. Never VerifyNone.
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(tls);
    req.setTransferTimeout(120000);

    auto* mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    auto* audio = new QFile(file);
    if (!audio->open(QIODevice::ReadOnly)) {
        delete audio;
        delete mp;
        if (err) *err = QStringLiteral("cannot read %1").arg(file);
        return {};
    }
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/octet-stream"));
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"file\"; filename=\"%1\"")
                           .arg(QFileInfo(file).fileName()));
    filePart.setBodyDevice(audio);
    audio->setParent(mp);
    mp->append(filePart);

    auto textPart = [](const QString& name, const QString& value) {
        QHttpPart p;
        p.setHeader(QNetworkRequest::ContentDispositionHeader,
                    QStringLiteral("form-data; name=\"%1\"").arg(name));
        p.setBody(value.toUtf8());
        return p;
    };
    mp->append(textPart(QStringLiteral("model"),
                        Config::str(QStringLiteral("stt.model"), QStringLiteral("whisper-1"))));
    mp->append(textPart(QStringLiteral("response_format"), QStringLiteral("json")));
    const QString lang = language();
    if (!lang.isEmpty()) mp->append(textPart(QStringLiteral("language"), lang));

    // A local QNetworkAccessManager + QEventLoop, matching OllamaBackend: the
    // manager is thread-affine, and transcribe() is called from worker threads.
    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.post(req, mp);
    mp->setParent(reply);  // the multipart must outlive the request body upload

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError netErr = reply->error();
    const QString netErrStr = reply->errorString();
    reply->deleteLater();

    if (netErr != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (err) {
            *err = netErr != QNetworkReply::NoError
                       ? netErrStr
                       : QStringLiteral("stt.host returned HTTP %1").arg(status);
        }
        return {};
    }

    const QJsonObject o = json::objectFrom(QString::fromUtf8(body));
    if (o.contains(QLatin1String("text"))) return o.value(QLatin1String("text")).toString().trimmed();
    // Some servers reply with the bare transcript rather than {"text": …}.
    return QString::fromUtf8(body).trimmed();
}

// Run a user-configured CLI. "{file}" is replaced with the audio path; if the
// template has no placeholder the path is appended as the final argument.
QString viaCommand(const QString& tpl, const QString& file, QString* err) {
    QStringList argv = splitCommand(tpl);
    if (argv.isEmpty()) {
        if (err) *err = QStringLiteral("stt.command is empty");
        return {};
    }

    bool substituted = false;
    for (QString& tok : argv) {
        if (tok.contains(QLatin1String("{file}"))) {
            tok.replace(QLatin1String("{file}"), file);
            substituted = true;
        }
    }
    if (!substituted) argv << file;

    const QString program = argv.takeFirst();
    return runCapture(program, argv, 600000, err).trimmed();
}

// Zero-config transcription through whichever whisper engine is present.
QString viaAuto(const QString& file, const QString& engine, QString* err) {
    const QString lang = language();
    const QString size = Stt::modelSize();

    if (engine == QLatin1String("whisper.cpp")) {
        const QString bin = whisperCppBin();
        QString model = ggmlModelFile();
        // An advanced user can point stt.model straight at a .bin.
        if (model.isEmpty()) {
            const QString raw = Config::str(QStringLiteral("stt.model")).trimmed();
            if (!raw.isEmpty() && QFileInfo(raw).isFile()) model = raw;
        }
        if (bin.isEmpty() || model.isEmpty()) {
            if (err) *err = QStringLiteral("no whisper.cpp engine or ggml model — run `ollamadev voice --setup`");
            return {};
        }

        // Flags verified against `whisper-cli --help`: -m model, -f file,
        // -l language ('auto' to detect), -nt no timestamps, -np no prints,
        // -otxt + -of <base> writes <base>.txt.
        const QString of = scratchBase();
        QStringList args{"-m",   model, "-f", file, "-l", lang.isEmpty() ? QStringLiteral("auto") : lang,
                         "-nt",  "-np", "-otxt", "-of", of};
        runCapture(bin, args, 600000, err);

        const QString txt = of + QStringLiteral(".txt");
        QFile f(txt);
        if (!f.open(QIODevice::ReadOnly)) {
            if (err && err->isEmpty()) *err = QStringLiteral("whisper.cpp produced no transcript");
            return {};
        }
        const QString out = QString::fromUtf8(f.readAll()).trimmed();
        f.close();
        f.remove();  // exactly the one file we created — never a glob
        if (err && !out.isEmpty()) err->clear();
        return out;
    }

    if (engine == QLatin1String("openai-whisper")) {
        // Flags verified against `whisper --help`: --model, --language,
        // --output_format txt, --output_dir, --device, --fp16.
        // fp16 is a CUDA thing; forcing cpu+False keeps this working (loudly) on
        // a box with no GPU, which is the common case.
        const QString dir = QDir::tempPath();
        QStringList args{file,   "--model",         size, "--output_format", "txt",
                         "--output_dir", dir, "--device", "cpu", "--fp16", "False"};
        if (!lang.isEmpty()) args << "--language" << lang;
        runCapture(QStringLiteral("whisper"), args, 900000, err);

        const QString txt = dir + u'/' + QFileInfo(file).completeBaseName() + QStringLiteral(".txt");
        QFile f(txt);
        if (!f.open(QIODevice::ReadOnly)) {
            if (err && err->isEmpty()) *err = QStringLiteral("whisper produced no transcript");
            return {};
        }
        const QString out = QString::fromUtf8(f.readAll()).trimmed();
        f.close();
        f.remove();
        if (err && !out.isEmpty()) err->clear();
        return out;
    }

    if (engine == QLatin1String("faster-whisper")) {
        QStringList args{file, "--model", size, "--device", "cpu"};
        if (!lang.isEmpty()) args << "--language" << lang;
        return runCapture(QStringLiteral("faster-whisper"), args, 900000, err).trimmed();
    }

    if (err) *err = QStringLiteral("no local speech-to-text engine found");
    return {};
}

// Stream a URL to disk with progress. Downloads land on a .part file and are
// renamed only on success, so an interrupted fetch can never leave a truncated
// binary that looks provisioned.
bool download(const QString& url, const QString& dest, const Stt::Progress& onProgress,
              const QString& label, QString* err) {
    QFile out(dest + QStringLiteral(".part"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("cannot write %1").arg(dest);
        return false;
    }

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("OllamaDev/%1").arg(QLatin1String(ODV_VERSION)));
    // GitHub and Hugging Face both 302 to a CDN. NoLessSafeRedirectPolicy follows
    // that but refuses an https→http downgrade, so the TLS guarantee survives the
    // hop. Peer verification stays on: we are fetching an executable.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    req.setSslConfiguration(tls);
    // An inactivity timeout, not a total one — a 1.5 GB model may legitimately
    // take a long time, but a stalled socket should not hang forever.
    req.setTransferTimeout(60000);

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::readyRead, [&] { out.write(reply->readAll()); });
    if (onProgress) {
        QObject::connect(reply, &QNetworkReply::downloadProgress,
                         [&](qint64 done, qint64 total) { onProgress(label, done, total); });
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.write(reply->readAll());
    out.close();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
    if (!ok && err) {
        *err = reply->error() != QNetworkReply::NoError
                   ? reply->errorString()
                   : QStringLiteral("HTTP %1 fetching %2").arg(status).arg(label);
    }
    reply->deleteLater();

    if (!ok) {
        out.remove();
        return false;
    }
    QFile::remove(dest);  // the file we are replacing, by exact name
    if (!QFile::rename(dest + QStringLiteral(".part"), dest)) {
        if (err) *err = QStringLiteral("cannot finalise %1").arg(dest);
        out.remove();
        return false;
    }
    return true;
}

}  // namespace

// --- configuration -----------------------------------------------------------

bool Stt::enabled() {
    return !Config::str(QStringLiteral("stt.host")).trimmed().isEmpty() ||
           !Config::str(QStringLiteral("stt.command")).trimmed().isEmpty();
}

bool Stt::available() { return enabled() || !detectedEngine().isEmpty(); }

QString Stt::sttDir() { return Config::homeDir() + QStringLiteral("/stt"); }

QString Stt::detectedEngine() {
    if (!whisperCppBin().isEmpty()) return QStringLiteral("whisper.cpp");
    if (!which(QStringLiteral("faster-whisper")).isEmpty()) return QStringLiteral("faster-whisper");
    if (!which(QStringLiteral("whisper")).isEmpty()) return QStringLiteral("openai-whisper");
    return {};
}

bool Stt::hasEngine() { return !whisperCppBin().isEmpty(); }
bool Stt::hasModel() { return !ggmlModelFile().isEmpty(); }

QString Stt::modelSize() {
    const QString m = Config::str(QStringLiteral("stt.model")).trimmed();
    // 'whisper-1' is the OpenAI HTTP model name; locally it means "the default".
    return (m.isEmpty() || m == QLatin1String("whisper-1")) ? QStringLiteral("base") : m;
}

void Stt::setModelSize(const QString& size) {
    Config::setPref(QStringLiteral("stt.model"), size.trimmed());
}

QStringList Stt::modelSizes() {
    return {QStringLiteral("tiny"),   QStringLiteral("base"),     QStringLiteral("small"),
            QStringLiteral("medium"), QStringLiteral("large-v3"), QStringLiteral("turbo")};
}

// --- transcription -----------------------------------------------------------

QString Stt::transcribe(const QString& audioPath, QString* err) {
    QString local;
    QString& e = err ? *err : local;
    e.clear();

    if (!QFileInfo(audioPath).isFile()) {
        e = QStringLiteral("no such audio file: %1").arg(audioPath);
        return {};
    }

    const QString host = Config::str(QStringLiteral("stt.host")).trimmed();
    const QString cmd = Config::str(QStringLiteral("stt.command")).trimmed();

    QString text;
    QString engine;
    if (!host.isEmpty()) {
        engine = QStringLiteral("http");
        text = viaHttp(host, audioPath, &e);
    } else if (!cmd.isEmpty()) {
        engine = QStringLiteral("command");
        text = viaCommand(cmd, audioPath, &e);
    } else {
        engine = detectedEngine();
        if (engine.isEmpty()) {
            e = QStringLiteral("no speech-to-text engine — run `ollamadev voice --setup`, "
                               "or set stt.host / stt.command");
            return {};
        }
        text = viaAuto(audioPath, engine, &e);
    }

    text = text.trimmed();
    if (text.isEmpty()) {
        if (e.isEmpty()) e = QStringLiteral("%1 returned no text").arg(engine);
        return {};
    }
    e.clear();
    logHistory(text, modelSize(), engine);
    return text;
}

// --- history -----------------------------------------------------------------

QVector<QJsonObject> Stt::history(int n) {
    QVector<QJsonObject> out;
    if (n <= 0) return out;

    QFile f(historyFile());
    if (!f.open(QIODevice::ReadOnly)) return out;

    QVector<QJsonObject> all;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty()) continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.contains(QLatin1String("text"))) all << o;
    }
    const int from = qMax(0, all.size() - n);
    for (int i = from; i < all.size(); ++i) out << all.at(i);
    return out;
}

void Stt::clearHistory() {
    // One file, by exact name. Never a glob, never a directory.
    QFile::remove(historyFile());
}

// --- provisioning ------------------------------------------------------------

bool Stt::provision(const Progress& onProgress, const QString& size, QString* err) {
    QString local;
    QString& e = err ? *err : local;
    e.clear();

    const QString dir = sttDir();
    if (!QDir().mkpath(dir)) {
        e = QStringLiteral("cannot create %1").arg(dir);
        return false;
    }

    if (whisperCppBin().isEmpty()) {
        const QString target = platformTarget();
        const QString bin = dir + u'/' + target;
        const QString url =
            QStringLiteral("https://github.com/kennethyork/OllamaDev/releases/latest/download/%1")
                .arg(target);
        if (!download(url, bin, onProgress, QStringLiteral("engine"), &e)) return false;
        QFile::setPermissions(bin, QFile::permissions(bin) | QFileDevice::ExeOwner |
                                       QFileDevice::ExeGroup | QFileDevice::ExeOther);
    }

    const QString want = size.isEmpty() ? modelSize() : size;
    if (ggmlModelFile(want).isEmpty()) {
        const QString name = ggmlModelName(want);
        const QString url = QStringLiteral(
                                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
                                "%1?download=true")
                                .arg(name);
        if (!download(url, dir + u'/' + name, onProgress, QStringLiteral("model ") + name, &e)) {
            return false;
        }
    }

    if (whisperCppBin().isEmpty() || ggmlModelFile(want).isEmpty()) {
        if (e.isEmpty()) e = QStringLiteral("provisioning finished but the engine is still missing");
        return false;
    }
    return true;
}

// --- recorder ----------------------------------------------------------------

SttRecorder::SttRecorder(QObject* parent) : QObject(parent) {}

SttRecorder::~SttRecorder() {
    // A Recorder going out of scope must not leave a mic process running. The
    // QProcess is a child QObject, so Qt would destroy it anyway — but destroying
    // a running QProcess only blocks; stopping it deliberately is what keeps the
    // wav valid and the mic released.
    if (proc_ && proc_->state() != QProcess::NotRunning) stop();
}

bool SttRecorder::isRecording() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

QString SttRecorder::recorderBin() {
    for (const QString& r :
         {QStringLiteral("arecord"), QStringLiteral("ffmpeg"), QStringLiteral("parecord")}) {
        if (!which(r).isEmpty()) return r;
    }
    return {};
}

bool SttRecorder::canRecord() { return !recorderBin().isEmpty(); }

bool SttRecorder::start(QString* err) {
    if (isRecording()) {
        if (err) *err = QStringLiteral("already recording");
        return false;
    }

    const QString rec = recorderBin();
    if (rec.isEmpty()) {
        if (err) *err = QStringLiteral("no recorder found — install alsa-utils (arecord), "
                                       "ffmpeg, or pulseaudio-utils (parecord)");
        return false;
    }

    wav_ = scratchBase() + QStringLiteral(".wav");
    const int cap = maxSeconds();

    // Every flag here was checked against the tool's own --help, and each of the
    // three was confirmed to finalise the wav header on SIGTERM (which is what
    // QProcess::terminate sends) rather than leaving a truncated RIFF size.
    QStringList args;
    if (rec == QLatin1String("arecord")) {
        // -q quiet, -t wav container, -f S16_LE, -r rate, -c channels,
        // -d duration (the safety cap, enforced by arecord itself).
        args = QStringList{"-q",  "-t", "wav",
                           "-f",  "S16_LE",
                           "-r",  QString::number(kRate),
                           "-c",  QString::number(kChannels),
                           "-d",  QString::number(cap),
                           wav_};
    } else if (rec == QLatin1String("ffmpeg")) {
        // -nostdin matters: ffmpeg reads stdin for its keyboard controls and would
        // otherwise eat the very Enter keypress the CLI uses to stop recording.
        args = QStringList{"-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                           "-f", "alsa", "-i", "default",
                           "-ar", QString::number(kRate),
                           "-ac", QString::number(kChannels),
                           "-t", QString::number(cap),
                           wav_};
    } else {
        // parecord has no duration flag; the PHP wrapped it in coreutils `timeout`,
        // which meant the process being signalled was timeout's child, not the pid
        // we recorded. We own the handle instead, so stop() is the cap.
        args = QStringList{"--rate=" + QString::number(kRate),
                           "--channels=" + QString::number(kChannels),
                           "--format=s16le",
                           "--file-format=wav",
                           wav_};
    }

    proc_ = new QProcess(this);  // parented: our child, our responsibility
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    proc_->start(rec, args);
    if (!proc_->waitForStarted(5000)) {
        if (err) *err = QStringLiteral("could not start %1").arg(rec);
        delete proc_;
        proc_ = nullptr;
        wav_.clear();
        return false;
    }
    return true;
}

QString SttRecorder::stop() {
    if (!proc_) return {};

    if (proc_->state() != QProcess::NotRunning) {
        // SIGTERM to OUR child handle. All three recorders trap it and rewrite the
        // RIFF/data sizes, so the wav is playable. Only if one ignores us do we
        // escalate — still to this pid, never to a name.
        proc_->terminate();
        if (!proc_->waitForFinished(4000)) {
            proc_->kill();
            proc_->waitForFinished(2000);
        }
    }

    delete proc_;
    proc_ = nullptr;

    const QString path = wav_;
    wav_.clear();
    // A recorder that never got a sample still leaves a header-only file; treat
    // that as "nothing captured" so transcribe() is not asked to read silence.
    if (path.isEmpty() || QFileInfo(path).size() < 1024) {
        if (!path.isEmpty()) QFile::remove(path);
        return {};
    }
    return path;
}

}  // namespace odv
