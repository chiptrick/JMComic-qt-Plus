/****************************************************************************
** Meta object code from reading C++ file 'jm_api.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.12.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../jm_api.h"
#include <QtCore/qmetatype.h>
#include <QtCore/QList>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'jm_api.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.12.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN5JmApiE_t {};
} // unnamed namespace

template <> constexpr inline auto JmApi::qt_create_metaobjectdata<qt_meta_tag_ZN5JmApiE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "JmApi",
        "SettingReady",
        "",
        "ok",
        "version",
        "error",
        "SearchReady",
        "QList<JmComicItem>",
        "items",
        "total",
        "page",
        "AlbumReady",
        "JmAlbum",
        "album",
        "ChapterReady",
        "JmChapterDetail",
        "chapter",
        "LoginReady",
        "JmUserInfo",
        "user",
        "FavoriteReady",
        "FavoriteAddReady",
        "SessionExpired",
        "message",
        "DailyReady",
        "signedToday",
        "dailyId",
        "SignReady",
        "PromoteReady",
        "QList<JmHomeBlock>",
        "blocks",
        "LatestReady",
        "Log",
        "line"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'SettingReady'
        QtMocHelpers::SignalData<void(bool, const QString &, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { QMetaType::QString, 4 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'SearchReady'
        QtMocHelpers::SignalData<void(bool, const QVector<JmComicItem> &, int, int, const QString &)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { 0x80000000 | 7, 8 }, { QMetaType::Int, 9 }, { QMetaType::Int, 10 },
            { QMetaType::QString, 5 },
        }}),
        // Signal 'AlbumReady'
        QtMocHelpers::SignalData<void(bool, const JmAlbum &, const QString &)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { 0x80000000 | 12, 13 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'ChapterReady'
        QtMocHelpers::SignalData<void(bool, const JmChapterDetail &, const QString &)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { 0x80000000 | 15, 16 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'LoginReady'
        QtMocHelpers::SignalData<void(bool, const JmUserInfo &, const QString &)>(17, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { 0x80000000 | 18, 19 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'FavoriteReady'
        QtMocHelpers::SignalData<void(bool, const QVector<JmComicItem> &, int, const QString &)>(20, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { 0x80000000 | 7, 8 }, { QMetaType::Int, 9 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'FavoriteAddReady'
        QtMocHelpers::SignalData<void(bool, const QString &)>(21, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'SessionExpired'
        QtMocHelpers::SignalData<void(const QString &)>(22, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 23 },
        }}),
        // Signal 'DailyReady'
        QtMocHelpers::SignalData<void(bool, bool, int, const QString &, const QString &)>(24, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { QMetaType::Bool, 25 }, { QMetaType::Int, 26 }, { QMetaType::QString, 23 },
            { QMetaType::QString, 5 },
        }}),
        // Signal 'SignReady'
        QtMocHelpers::SignalData<void(bool, const QString &, const QString &)>(27, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { QMetaType::QString, 23 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'PromoteReady'
        QtMocHelpers::SignalData<void(bool, const QVector<JmHomeBlock> &, const QString &)>(28, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { 0x80000000 | 29, 30 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'LatestReady'
        QtMocHelpers::SignalData<void(bool, const QVector<JmComicItem> &, int, const QString &)>(31, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 }, { 0x80000000 | 7, 8 }, { QMetaType::Int, 10 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'Log'
        QtMocHelpers::SignalData<void(const QString &)>(32, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 33 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<JmApi, qt_meta_tag_ZN5JmApiE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject JmApi::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN5JmApiE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN5JmApiE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN5JmApiE_t>.metaTypes,
    nullptr
} };

void JmApi::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<JmApi *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->SettingReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 1: _t->SearchReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QList<JmComicItem>>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[4])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[5]))); break;
        case 2: _t->AlbumReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<JmAlbum>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 3: _t->ChapterReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<JmChapterDetail>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 4: _t->LoginReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<JmUserInfo>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 5: _t->FavoriteReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QList<JmComicItem>>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[4]))); break;
        case 6: _t->FavoriteAddReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 7: _t->SessionExpired((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 8: _t->DailyReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<bool>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[4])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[5]))); break;
        case 9: _t->SignReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 10: _t->PromoteReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QList<JmHomeBlock>>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 11: _t->LatestReady((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QList<JmComicItem>>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[4]))); break;
        case 12: _t->Log((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const QString & , const QString & )>(_a, &JmApi::SettingReady, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const QVector<JmComicItem> & , int , int , const QString & )>(_a, &JmApi::SearchReady, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const JmAlbum & , const QString & )>(_a, &JmApi::AlbumReady, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const JmChapterDetail & , const QString & )>(_a, &JmApi::ChapterReady, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const JmUserInfo & , const QString & )>(_a, &JmApi::LoginReady, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const QVector<JmComicItem> & , int , const QString & )>(_a, &JmApi::FavoriteReady, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const QString & )>(_a, &JmApi::FavoriteAddReady, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(const QString & )>(_a, &JmApi::SessionExpired, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , bool , int , const QString & , const QString & )>(_a, &JmApi::DailyReady, 8))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const QString & , const QString & )>(_a, &JmApi::SignReady, 9))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const QVector<JmHomeBlock> & , const QString & )>(_a, &JmApi::PromoteReady, 10))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(bool , const QVector<JmComicItem> & , int , const QString & )>(_a, &JmApi::LatestReady, 11))
            return;
        if (QtMocHelpers::indexOfMethod<void (JmApi::*)(const QString & )>(_a, &JmApi::Log, 12))
            return;
    }
}

const QMetaObject *JmApi::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *JmApi::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN5JmApiE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int JmApi::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 13)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 13;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 13)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 13;
    }
    return _id;
}

// SIGNAL 0
void JmApi::SettingReady(bool _t1, const QString & _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}

// SIGNAL 1
void JmApi::SearchReady(bool _t1, const QVector<JmComicItem> & _t2, int _t3, int _t4, const QString & _t5)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2, _t3, _t4, _t5);
}

// SIGNAL 2
void JmApi::AlbumReady(bool _t1, const JmAlbum & _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2, _t3);
}

// SIGNAL 3
void JmApi::ChapterReady(bool _t1, const JmChapterDetail & _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2, _t3);
}

// SIGNAL 4
void JmApi::LoginReady(bool _t1, const JmUserInfo & _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1, _t2, _t3);
}

// SIGNAL 5
void JmApi::FavoriteReady(bool _t1, const QVector<JmComicItem> & _t2, int _t3, const QString & _t4)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1, _t2, _t3, _t4);
}

// SIGNAL 6
void JmApi::FavoriteAddReady(bool _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1, _t2);
}

// SIGNAL 7
void JmApi::SessionExpired(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void JmApi::DailyReady(bool _t1, bool _t2, int _t3, const QString & _t4, const QString & _t5)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1, _t2, _t3, _t4, _t5);
}

// SIGNAL 9
void JmApi::SignReady(bool _t1, const QString & _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 9, nullptr, _t1, _t2, _t3);
}

// SIGNAL 10
void JmApi::PromoteReady(bool _t1, const QVector<JmHomeBlock> & _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 10, nullptr, _t1, _t2, _t3);
}

// SIGNAL 11
void JmApi::LatestReady(bool _t1, const QVector<JmComicItem> & _t2, int _t3, const QString & _t4)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 11, nullptr, _t1, _t2, _t3, _t4);
}

// SIGNAL 12
void JmApi::Log(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 12, nullptr, _t1);
}
QT_WARNING_POP
