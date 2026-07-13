#pragma once
#include <QString>
#include <QStringList>

namespace odv {
// @image / /image parsing → base64 into an Ollama message's images[]. STUB.
class Vision {
public:
    static QStringList extractImagePaths(const QString& prompt);   // @img.png, /image path
    static QString stripImageTokens(const QString& prompt);
    static QString encodeBase64(const QString& path, QString* err = nullptr);  // "" on failure
};
}  // namespace odv
