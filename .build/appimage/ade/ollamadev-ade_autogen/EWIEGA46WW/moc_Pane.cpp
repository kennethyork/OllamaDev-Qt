/****************************************************************************
** Meta object code from reading C++ file 'Pane.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.4.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../../../ade/Pane.h"
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'Pane.h' doesn't include <QObject>."
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
struct qt_meta_stringdata_odv__Pane_t {
    uint offsetsAndSizes[12];
    char stringdata0[10];
    char stringdata1[15];
    char stringdata2[1];
    char stringdata3[7];
    char stringdata4[21];
    char stringdata5[15];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_odv__Pane_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_odv__Pane_t qt_meta_stringdata_odv__Pane = {
    {
        QT_MOC_LITERAL(0, 9),  // "odv::Pane"
        QT_MOC_LITERAL(10, 14),  // "closeRequested"
        QT_MOC_LITERAL(25, 0),  // ""
        QT_MOC_LITERAL(26, 6),  // "raised"
        QT_MOC_LITERAL(33, 20),  // "focusToggleRequested"
        QT_MOC_LITERAL(54, 14)   // "geometryEdited"
    },
    "odv::Pane",
    "closeRequested",
    "",
    "raised",
    "focusToggleRequested",
    "geometryEdited"
};
#undef QT_MOC_LITERAL
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_odv__Pane[] = {

 // content:
      10,       // revision
       0,       // classname
       0,    0, // classinfo
       4,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,   38,    2, 0x06,    1 /* Public */,
       3,    0,   39,    2, 0x06,    2 /* Public */,
       4,    0,   40,    2, 0x06,    3 /* Public */,
       5,    0,   41,    2, 0x06,    4 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject odv::Pane::staticMetaObject = { {
    QMetaObject::SuperData::link<QGraphicsWidget::staticMetaObject>(),
    qt_meta_stringdata_odv__Pane.offsetsAndSizes,
    qt_meta_data_odv__Pane,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_odv__Pane_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<Pane, std::true_type>,
        // method 'closeRequested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'raised'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'focusToggleRequested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'geometryEdited'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void odv::Pane::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<Pane *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->closeRequested(); break;
        case 1: _t->raised(); break;
        case 2: _t->focusToggleRequested(); break;
        case 3: _t->geometryEdited(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (Pane::*)();
            if (_t _q_method = &Pane::closeRequested; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (Pane::*)();
            if (_t _q_method = &Pane::raised; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (Pane::*)();
            if (_t _q_method = &Pane::focusToggleRequested; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (Pane::*)();
            if (_t _q_method = &Pane::geometryEdited; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
    }
    (void)_a;
}

const QMetaObject *odv::Pane::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *odv::Pane::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_odv__Pane.stringdata0))
        return static_cast<void*>(this);
    return QGraphicsWidget::qt_metacast(_clname);
}

int odv::Pane::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QGraphicsWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 4)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void odv::Pane::closeRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void odv::Pane::raised()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void odv::Pane::focusToggleRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void odv::Pane::geometryEdited()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
