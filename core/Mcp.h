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
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

#include "Tools.h"

namespace odv {

// One entry of the `mcpServers` config object.
//
// CONFIG KEY — the PHP had a real bug here: src/20-mcp.php:167 loaded servers
// from `mcpServers` while src/99-main.php:336 (`mcp add`) wrote them to `mcp`, so
// every server the CLI added was invisible to the loader. This port uses
// `mcpServers` for BOTH, and adopts a legacy `mcp` entry on read so a config
// written by the broken CLI starts working instead of silently doing nothing.
struct McpServerCfg {
    QString name;
    QString type;  // "stdio" (command) | "http" (url)
    QString command;
    QStringList args;
    QString url;
    QJsonObject headers;
    QJsonObject env;
    bool disabled = false;
};

// A connection to ONE MCP server.
//
// stdio transport is a child process speaking LSP-style Content-Length-framed
// JSON-RPC 2.0. The process and every call on it are confined to a dedicated
// thread, because QProcess is thread-affine and any crew coder thread may reach
// a discovered tool. Calls are therefore serialised per server, which is also
// what the transport requires (one framed reply per request, in order).
class McpClient {
public:
    explicit McpClient(McpServerCfg cfg);
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    // Raw `tools` array as the server reported it. Empty if it never came up.
    QJsonArray listTools();

    // Returns the tool's text content. `ok` is false for a transport failure or a
    // JSON-RPC error, so the agent can tell "the tool said no" from "the tool
    // never ran".
    QString callTool(const QString& name, const QJsonObject& args, bool* ok);

    const McpServerCfg& cfg() const { return cfg_; }

    // Terminate the child. Called for our OWN children only — never a global kill.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    McpServerCfg cfg_;
};

class Mcp {
public:
    static QVector<McpServerCfg> servers();

    // Persist to config.json (which is MCP-only by convention — Config::setPref
    // deliberately writes prefs, not this).
    static bool addServer(const QString& name, const QString& command, const QStringList& args,
                          QString* err);
    static bool removeServer(const QString& name, QString* err);

    // Connect to every enabled server and turn its tools into ToolDefs for OUR
    // registry, so the model calls an MCP tool exactly like a native one.
    // Returns empty (and spawns nothing) when no servers are configured.
    static QVector<ToolDef> discoverTools();
};

// The other half: expose OUR registry to any MCP client over stdio.
class McpServer {
public:
    // Newline-delimited JSON-RPC 2.0 on stdin/stdout. READ-ONLY unless
    // allowWrites (or config mcp.allowWrites) opts in: an MCP client is a remote
    // caller and must not get bash/write/rm on the user's machine for free.
    static int serve(bool allowWrites);
};

}  // namespace odv
