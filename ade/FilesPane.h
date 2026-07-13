#pragma once
#include <QModelIndex>
#include <QString>
#include <QWidget>

class QFileSystemModel;
class QTreeView;
class QLineEdit;

namespace odv {

// The project file browser.
//
// The PHP FileBrowser served a FLAT single-level listing with a ".." row, because
// a webview page had to round-trip to PHP for every directory. QFileSystemModel
// is lazy, watched (external edits show up on their own) and gives us a real
// tree for free, so the flat listing is not worth reproducing.
class FilesPane : public QWidget {
    Q_OBJECT

public:
    explicit FilesPane(const QString& root, QWidget* parent = nullptr);

    void setRoot(const QString& root);
    QString root() const { return root_; }

signals:
    void fileActivated(const QString& path);  // MainWindow routes this to the editor
    void statusMessage(const QString& msg);

private slots:
    void onActivated(const QModelIndex& idx);
    void onContextMenu(const QPoint& pos);

private:
    void renameAt(const QModelIndex& idx);
    void deleteAt(const QModelIndex& idx);

    QFileSystemModel* model_ = nullptr;
    QTreeView* tree_ = nullptr;
    QString root_;
};

}  // namespace odv
