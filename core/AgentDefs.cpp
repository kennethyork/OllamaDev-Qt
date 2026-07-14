// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AgentDefs.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>

#include "Config.h"
#include "Json.h"
#include "Tools.h"

namespace odv {
namespace {

// A leading/trailing run of quote or bracket chars around a frontmatter value.
QString trimValue(QString s) {
    return s.trimmed().remove(QRegularExpression(QStringLiteral("^[\"'\\[]+|[\"'\\]]+$")));
}

// Parse one .md persona: optional `--- … ---` YAML-ish frontmatter, then the body
// (its system prompt). Only the keys we understand are read; everything else in
// the frontmatter is ignored, so a persona file can carry extra notes.
AgentDef parseFile(const QString& file) {
    AgentDef d;
    d.file = file;
    d.name = QFileInfo(file).completeBaseName();

    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return d;
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QString body = content;
    static const QRegularExpression fm(QStringLiteral("^---\\s*\\n(.*?)\\n---\\s*\\n(.*)$"),
                                       QRegularExpression::DotMatchesEverythingOption);
    const auto m = fm.match(content);
    if (m.hasMatch()) {
        body = m.captured(2);
        for (const QString& line : m.captured(1).split(QLatin1Char('\n'))) {
            const int c = line.indexOf(QLatin1Char(':'));
            if (c < 0) continue;
            const QString key = line.left(c).trimmed().toLower();
            const QString val = trimValue(line.mid(c + 1));
            if (key == QLatin1String("name") && !val.isEmpty()) d.name = val;
            else if (key == QLatin1String("description")) d.description = val;
            else if (key == QLatin1String("model")) d.model = val;
            else if (key == QLatin1String("permission")) d.permission = val;
            else if (key == QLatin1String("tools")) {
                for (const QString& t : val.split(QLatin1Char(','), Qt::SkipEmptyParts))
                    if (!t.trimmed().isEmpty()) d.tools << t.trimmed();
            }
        }
    }
    d.prompt = body.trimmed();
    return d;
}

// Project directory first so a repo persona shadows a same-named home one.
QStringList personaDirs() {
    QStringList dirs;
    dirs << QDir::currentPath() + QStringLiteral("/.ollamadev/agents");
    const QString home = QDir::homePath();
    if (!home.isEmpty()) dirs << home + QStringLiteral("/.ollamadev/agents");
    dirs.removeDuplicates();
    return dirs;
}

}  // namespace

QVector<AgentDef> AgentDefs::all() {
    QMap<QString, AgentDef> byName;  // keyed lower-case; sorted by QMap
    for (const QString& dir : personaDirs()) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QFileInfo& fi : d.entryInfoList({QStringLiteral("*.md")}, QDir::Files)) {
            const AgentDef def = parseFile(fi.absoluteFilePath());
            const QString key = def.name.toLower();
            if (!key.isEmpty() && !byName.contains(key)) byName.insert(key, def);
        }
    }
    QVector<AgentDef> out;
    out.reserve(byName.size());
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it) out << it.value();
    return out;
}

QStringList AgentDefs::list() {
    QStringList names;
    for (const AgentDef& d : all()) names << d.name;
    return names;
}

AgentDef AgentDefs::get(const QString& name) {
    const QString want = name.trimmed().toLower();
    if (want.isEmpty()) return {};
    for (const AgentDef& d : all())
        if (d.name.toLower() == want) return d;
    return {};
}

// ---------------------------------------------------------------- OutputStyles

namespace {
struct Style {
    QString desc;
    QString prompt;
};
const QMap<QString, Style>& styles() {
    static const QMap<QString, Style> s{
        {QStringLiteral("default"),
         {QStringLiteral("Balanced — the standard OllamaDev voice."), QString()}},
        {QStringLiteral("concise"),
         {QStringLiteral("Terse — the shortest correct answer, no preamble."),
          QStringLiteral("\n\nOUTPUT STYLE: Be extremely concise. Give the shortest correct "
                         "answer; skip preamble, restating the question, and closing summaries. "
                         "Prefer lists over paragraphs.")}},
        {QStringLiteral("explanatory"),
         {QStringLiteral("Mentor mode — explain the why behind each step."),
          QStringLiteral("\n\nOUTPUT STYLE: Be explanatory. Briefly explain the reasoning and "
                         "trade-offs behind what you do, as a mentor would — without becoming "
                         "verbose.")}},
        {QStringLiteral("formal"),
         {QStringLiteral("Professional and precise — no slang or emoji."),
          QStringLiteral("\n\nOUTPUT STYLE: Write formally and precisely. No slang, no emoji; "
                         "complete sentences and exact terminology.")}},
        {QStringLiteral("bullets"),
         {QStringLiteral("Bullet-first — structure answers as lists."),
          QStringLiteral("\n\nOUTPUT STYLE: Structure every answer as bullet points wherever "
                         "possible; use prose only when a list won't do.")}},
    };
    return s;
}
}  // namespace

QStringList OutputStyles::names() {
    // Stable, meaningful order rather than QMap's alphabetical one.
    return {QStringLiteral("default"), QStringLiteral("concise"), QStringLiteral("explanatory"),
            QStringLiteral("formal"), QStringLiteral("bullets")};
}

QString OutputStyles::current() {
    const QString s = Config::str(QStringLiteral("outputStyle"), QStringLiteral("default"))
                          .trimmed()
                          .toLower();
    return styles().contains(s) ? s : QStringLiteral("default");
}

QString OutputStyles::suffix(const QString& style) {
    const QString s = style.trimmed().toLower();
    return styles().contains(s) ? styles().value(s).prompt : QString();
}

QString OutputStyles::describe(const QString& style) {
    const QString s = style.trimmed().toLower();
    return styles().contains(s) ? styles().value(s).desc : QString();
}

bool OutputStyles::set(const QString& name) {
    const QString s = name.trimmed().toLower();
    if (!styles().contains(s)) return false;
    Config::setPref(QStringLiteral("outputStyle"), s);
    return true;
}

// ------------------------------------------------------------------ StatusLine

namespace {

// SECURITY: the status line may be a SHELL COMMAND. If we honoured a repo-local
// .ollamadev.json here, cloning a hostile repo and starting OllamaDev in it would
// silently run that repo's chosen command on the user's machine. So the status
// line is read from the HOME config (and ade-prefs) ONLY — never from the merged
// config that layers the project file in. This mirrors PHP's Config::trustedGet.
QString homeStatuslineRaw() {
    const QString home = QDir::homePath();
    QStringList files;
    if (!home.isEmpty()) {
        files << home + QStringLiteral("/.ollamadev/config.json")
              << home + QStringLiteral("/.config/ollamadev/config.json")
              << home + QStringLiteral("/.ollamadev/ade-prefs.json");
    }
    for (const QString& path : files) {
        QFile f(path);
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = json::objectFrom(QString::fromUtf8(f.readAll()));
        f.close();
        // ade-prefs stores flat dotted keys; config.json may nest.
        const QJsonValue v = json::at(o, QStringLiteral("statusline"));
        if (v.isString() && !v.toString().trimmed().isEmpty()) return v.toString().trimmed();
        const QJsonValue flat = o.value(QStringLiteral("statusline"));
        if (flat.isString() && !flat.toString().trimmed().isEmpty()) return flat.toString().trimmed();
    }
    return {};
}

QString gitBranch() {
    QProcess p;
    p.start(QStringLiteral("git"),
            {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("HEAD")});
    if (!p.waitForFinished(1500)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

}  // namespace

bool StatusLine::configured() { return !homeStatuslineRaw().isEmpty(); }

QString StatusLine::render(const QString& model, const QString& mode) {
    const QString cfg = homeStatuslineRaw();
    if (cfg.isEmpty()) return {};

    const QString branch = gitBranch();
    const QString branchTok = branch.isEmpty() ? QStringLiteral("-") : branch;

    // Template form: substitute the tokens.
    if (cfg.contains(QLatin1Char('{'))) {
        QString out = cfg;
        out.replace(QStringLiteral("{model}"), model);
        out.replace(QStringLiteral("{cwd}"), QDir::currentPath());
        out.replace(QStringLiteral("{branch}"), branchTok);
        out.replace(QStringLiteral("{mode}"), mode);
        return out;
    }

    // Command form: run it, pass context via env, show the first non-empty line.
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("OLLAMADEV_MODEL"), model);
    env.insert(QStringLiteral("OLLAMADEV_CWD"), QDir::currentPath());
    env.insert(QStringLiteral("OLLAMADEV_BRANCH"), branch);
    env.insert(QStringLiteral("OLLAMADEV_MODE"), mode);
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cfg});
    if (!p.waitForFinished(2000)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    for (const QString& line : QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n')))
        if (!line.trimmed().isEmpty()) return line.trimmed();
    return {};
}

}  // namespace odv
