// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace odv {

// GRAPH MEMORY — a persistent, linked knowledge base that lives next to the code.
// Each note is a markdown file with frontmatter (title, tags) and a body that can
// link to other notes with [[wiki-links]]:
//
//   <project>/.ollamadev/memory/<slug>.md   committable, co-located with the code
//   ~/.ollamadev/memory/<slug>.md           global
//
// The agent gets a SHORT INDEX of titles in its system prompt (index()) and pulls
// whole notes on demand with the `recall` tool; it writes new facts with
// `remember`. The links form a graph the desktop renders (graph()).
struct MemoryNote {
    QString slug;
    QString title;
    QString body;  // frontmatter stripped
    QString path;
    QStringList tags;
    QStringList links;  // raw [[targets]], not yet resolved to slugs

    bool isNull() const { return slug.isEmpty(); }
};

class Memory {
public:
    static QStringList baseDirs();
    static QString projectDir();  // where save() writes

    // Every note, project overriding global, sorted by slug.
    static QVector<MemoryNote> all();

    // By slug, or (case-insensitively) by title. Null note when there is none.
    static MemoryNote get(const QString& slugOrTitle);

    static QString slugify(const QString& s);

    // Write a note into the project memory dir. Returns the slug.
    static QString save(const QString& title, const QString& body, const QStringList& tags = {},
                        const QString& slug = {});

    static bool remove(const QString& slug);

    // Substring match against title, tags, and body.
    static QVector<MemoryNote> search(const QString& query);

    // The bounded "slug — title" list that goes in the system prompt. Bounded on
    // purpose: the prompt gets the INDEX, never the bodies.
    static QString index(int cap = 24);

    // {nodes:[{id,title,tags,degree}], edges:[{from,to}]} — [[links]] resolved to
    // real notes by slug or title; dangling links are dropped.
    static QJsonObject graph();

    // Extract durable, reusable project facts from recent work and save them,
    // deduped against what already exists. Best-effort: any failure (backend
    // down, unparseable reply) returns an empty list. Returns the slugs saved.
    static QStringList autoRemember(const QString& context, const QString& backendId = {},
                                    const QString& model = {});
};

}  // namespace odv
