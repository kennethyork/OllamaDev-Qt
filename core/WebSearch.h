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
#include <QString>
#include <QVector>

namespace odv {

// One web result, provider-agnostic.
struct SearchHit {
    QString title;
    QString url;
    QString snippet;
};

struct SearchResult {
    bool ok = false;
    QString provider;  // the provider actually used, after fallback
    QVector<SearchHit> hits;
    QString error;
};

struct FetchedPage {
    bool ok = false;
    int status = 0;
    QString text;  // readable text, tags/scripts stripped
    QString error;
};

// The two network tools (`search`, `fetch`), behind one gate.
//
// Providers are search engines, never AI providers: only the query string leaves
// the machine.
//   duckduckgo — default, no key, scrapes the html endpoint
//   searxng    — self-hosted, the most local-first option (search.host)
//   brave      — needs an API key (search.key or BRAVE_API_KEY)
//
// TLS: every request verifies the peer certificate. The PHP original set
// CURLOPT_SSL_VERIFYPEER => false, which silently accepts a MITM on every search
// and fetch; QNetworkAccessManager verifies by default and we never lower that.
class WebSearch {
public:
    // The web kill switch. Config `web.enabled` (default true) with a session
    // override for the CLI's --no-web, so a user can take the agent off the
    // network for one run without editing config.
    static bool webEnabled();
    static void setWebEnabled(bool on);

    // Blocking. Safe to call from any thread: the network stack is created on,
    // and dies with, the calling frame.
    static SearchResult search(const QString& query, int limit,
                               const QString& providerOverride = QString());

    static FetchedPage fetch(const QString& url, int timeoutSec = 30);

    // HTML → readable text. Public because `fetch` is not the only caller that
    // wants a page's prose without its markup.
    static QString htmlToText(const QString& html);
};

}  // namespace odv
