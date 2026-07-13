#pragma once
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include "Board.h"

class QHBoxLayout;
class QLineEdit;
class QTimer;
class QVBoxLayout;

namespace odv {

// The crew kanban. Polls the two files the engine publishes — Crew::boardState()
// (~/.ollamadev/crew/current.json) and Board::pending() — every 1.5s, exactly
// like startCrewPoll() in app.js. There is no push channel by design: the CLI and
// the GUI are separate processes sharing a directory, so polling IS the protocol.
class BoardPane : public QWidget {
    Q_OBJECT

public:
    explicit BoardPane(QWidget* parent = nullptr);

    // The cards carry explicit theme colours in their stylesheets, so a theme
    // switch has to rebuild them, not just repaint.
    void refreshTheme();

signals:
    void statusMessage(const QString& msg);

protected:
    // Polling stops while the pane is not on the canvas — a closed board must not
    // keep hitting the disk.
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private slots:
    void poll();
    void sendSteer();

private:
    struct Held {
        QString decisionId;
        QString diff;
        int files = 0;
        QString reason;
    };

    void rebuild();
    QWidget* makeColumn(const QString& key, const QString& label, int count);
    QWidget* makeCard(const QJsonObject& subtask);
    void acceptHeld(int n);
    void discardHeld(int n);
    static QString colFor(const QString& state);
    static QString diffHtml(const QString& diff);

    QJsonObject board_;             // Crew::boardState()
    QHash<int, Held> held_;         // subtask n -> its pending decision
    QString fingerprint_;           // rebuild only when something actually changed
    QSet<int> expanded_;            // which held cards have their diff open

    QLineEdit* steer_ = nullptr;
    QWidget* directorBar_ = nullptr;
    QWidget* columns_ = nullptr;
    QHBoxLayout* colsLayout_ = nullptr;
    QTimer* timer_ = nullptr;
};

}  // namespace odv
