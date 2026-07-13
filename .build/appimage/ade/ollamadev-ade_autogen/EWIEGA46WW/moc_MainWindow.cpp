/****************************************************************************
** Meta object code from reading C++ file 'MainWindow.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.4.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../../../ade/MainWindow.h"
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'MainWindow.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.4.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
namespace {
struct qt_meta_stringdata_odv__MainWindow_t {
    uint offsetsAndSizes[26];
    char stringdata0[16];
    char stringdata1[14];
    char stringdata2[1];
    char stringdata3[5];
    char stringdata4[11];
    char stringdata5[9];
    char stringdata6[13];
    char stringdata7[3];
    char stringdata8[20];
    char stringdata9[10];
    char stringdata10[9];
    char stringdata11[15];
    char stringdata12[5];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_odv__MainWindow_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_odv__MainWindow_t qt_meta_stringdata_odv__MainWindow = {
    {
        QT_MOC_LITERAL(0, 15),  // "odv::MainWindow"
        QT_MOC_LITERAL(16, 13),  // "addPaneOfKind"
        QT_MOC_LITERAL(30, 0),  // ""
        QT_MOC_LITERAL(31, 4),  // "kind"
        QT_MOC_LITERAL(36, 10),  // "pingOllama"
        QT_MOC_LITERAL(47, 8),  // "autosave"
        QT_MOC_LITERAL(56, 12),  // "onPaneClosed"
        QT_MOC_LITERAL(69, 2),  // "id"
        QT_MOC_LITERAL(72, 19),  // "onCanvasContextMenu"
        QT_MOC_LITERAL(92, 9),  // "globalPos"
        QT_MOC_LITERAL(102, 8),  // "worldPos"
        QT_MOC_LITERAL(111, 14),  // "onThemeChanged"
        QT_MOC_LITERAL(126, 4)   // "name"
    },
    "odv::MainWindow",
    "addPaneOfKind",
    "",
    "kind",
    "pingOllama",
    "autosave",
    "onPaneClosed",
    "id",
    "onCanvasContextMenu",
    "globalPos",
    "worldPos",
    "onThemeChanged",
    "name"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_odv__MainWindow[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    1,   50,    2, 0x08,    1 /* Private */,
       4,    0,   53,    2, 0x08,    3 /* Private */,
       5,    0,   54,    2, 0x08,    4 /* Private */,
       6,    1,   55,    2, 0x08,    5 /* Private */,
       8,    2,   58,    2, 0x08,    7 /* Private */,
      11,    1,   63,    2, 0x08,   10 /* Private */,

 // slots: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QPoint, QMetaType::QPointF,    9,   10,
    QMetaType::Void, QMetaType::QString,   12,

       0        // eod
};

Q_CONSTINIT const QMetaObject odv::MainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_odv__MainWindow.offsetsAndSizes,
    qt_meta_data_odv__MainWindow,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_odv__MainWindow_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<MainWindow, std::true_type>,
        // method 'addPaneOfKind'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'pingOllama'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'autosave'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onPaneClosed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'onCanvasContextMenu'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QPoint &, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QPointF &, std::false_type>,
        // method 'onThemeChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>
    >,
    nullptr
} };

void odv::MainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MainWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->addPaneOfKind((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->pingOllama(); break;
        case 2: _t->autosave(); break;
        case 3: _t->onPaneClosed((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 4: _t->onCanvasContextMenu((*reinterpret_cast< std::add_pointer_t<QPoint>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QPointF>>(_a[2]))); break;
        case 5: _t->onThemeChanged((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
}

const QMetaObject *odv::MainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *odv::MainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_odv__MainWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int odv::MainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 6;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
