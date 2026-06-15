/****************************************************************************
** Meta object code from reading C++ file 'cluster.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../Cluster/cluster.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'cluster.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.3. It"
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
struct qt_meta_tag_ZN14VehicleBackendE_t {};
} // unnamed namespace

template <> constexpr inline auto VehicleBackend::qt_create_metaobjectdata<qt_meta_tag_ZN14VehicleBackendE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "VehicleBackend",
        "QML.Element",
        "auto",
        "speedChanged",
        "",
        "powerChanged",
        "batteryChanged",
        "tempChanged",
        "vibTotalChanged",
        "currentChanged",
        "voltageChanged",
        "speedWarningChanged",
        "tempWarningChanged",
        "vibWarningChanged",
        "voltageWarningChanged",
        "criticalAlertChanged",
        "onSpiData",
        "stm32_data_t",
        "data",
        "speed",
        "power",
        "battery",
        "temp",
        "vibTotal",
        "current",
        "voltage",
        "speedWarning",
        "tempWarning",
        "vibWarning",
        "voltageWarning",
        "criticalAlert"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'speedChanged'
        QtMocHelpers::SignalData<void()>(3, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'powerChanged'
        QtMocHelpers::SignalData<void()>(5, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'batteryChanged'
        QtMocHelpers::SignalData<void()>(6, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'tempChanged'
        QtMocHelpers::SignalData<void()>(7, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'vibTotalChanged'
        QtMocHelpers::SignalData<void()>(8, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'currentChanged'
        QtMocHelpers::SignalData<void()>(9, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'voltageChanged'
        QtMocHelpers::SignalData<void()>(10, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'speedWarningChanged'
        QtMocHelpers::SignalData<void()>(11, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'tempWarningChanged'
        QtMocHelpers::SignalData<void()>(12, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'vibWarningChanged'
        QtMocHelpers::SignalData<void()>(13, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'voltageWarningChanged'
        QtMocHelpers::SignalData<void()>(14, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'criticalAlertChanged'
        QtMocHelpers::SignalData<void()>(15, 4, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onSpiData'
        QtMocHelpers::SlotData<void(stm32_data_t)>(16, 4, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 17, 18 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
        // property 'speed'
        QtMocHelpers::PropertyData<float>(19, QMetaType::Float, QMC::DefaultPropertyFlags, 0),
        // property 'power'
        QtMocHelpers::PropertyData<float>(20, QMetaType::Float, QMC::DefaultPropertyFlags, 1),
        // property 'battery'
        QtMocHelpers::PropertyData<float>(21, QMetaType::Float, QMC::DefaultPropertyFlags, 2),
        // property 'temp'
        QtMocHelpers::PropertyData<float>(22, QMetaType::Float, QMC::DefaultPropertyFlags, 3),
        // property 'vibTotal'
        QtMocHelpers::PropertyData<float>(23, QMetaType::Float, QMC::DefaultPropertyFlags, 4),
        // property 'current'
        QtMocHelpers::PropertyData<float>(24, QMetaType::Float, QMC::DefaultPropertyFlags, 5),
        // property 'voltage'
        QtMocHelpers::PropertyData<float>(25, QMetaType::Float, QMC::DefaultPropertyFlags, 6),
        // property 'speedWarning'
        QtMocHelpers::PropertyData<bool>(26, QMetaType::Bool, QMC::DefaultPropertyFlags, 7),
        // property 'tempWarning'
        QtMocHelpers::PropertyData<bool>(27, QMetaType::Bool, QMC::DefaultPropertyFlags, 8),
        // property 'vibWarning'
        QtMocHelpers::PropertyData<bool>(28, QMetaType::Bool, QMC::DefaultPropertyFlags, 9),
        // property 'voltageWarning'
        QtMocHelpers::PropertyData<bool>(29, QMetaType::Bool, QMC::DefaultPropertyFlags, 10),
        // property 'criticalAlert'
        QtMocHelpers::PropertyData<bool>(30, QMetaType::Bool, QMC::DefaultPropertyFlags, 11),
    };
    QtMocHelpers::UintData qt_enums {
    };
    QtMocHelpers::UintData qt_constructors {};
    QtMocHelpers::ClassInfos qt_classinfo({
            {    1,    2 },
    });
    return QtMocHelpers::metaObjectData<VehicleBackend, void>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums, qt_constructors, qt_classinfo);
}
Q_CONSTINIT const QMetaObject VehicleBackend::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14VehicleBackendE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14VehicleBackendE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14VehicleBackendE_t>.metaTypes,
    nullptr
} };

void VehicleBackend::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<VehicleBackend *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->speedChanged(); break;
        case 1: _t->powerChanged(); break;
        case 2: _t->batteryChanged(); break;
        case 3: _t->tempChanged(); break;
        case 4: _t->vibTotalChanged(); break;
        case 5: _t->currentChanged(); break;
        case 6: _t->voltageChanged(); break;
        case 7: _t->speedWarningChanged(); break;
        case 8: _t->tempWarningChanged(); break;
        case 9: _t->vibWarningChanged(); break;
        case 10: _t->voltageWarningChanged(); break;
        case 11: _t->criticalAlertChanged(); break;
        case 12: _t->onSpiData((*reinterpret_cast< std::add_pointer_t<stm32_data_t>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::speedChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::powerChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::batteryChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::tempChanged, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::vibTotalChanged, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::currentChanged, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::voltageChanged, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::speedWarningChanged, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::tempWarningChanged, 8))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::vibWarningChanged, 9))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::voltageWarningChanged, 10))
            return;
        if (QtMocHelpers::indexOfMethod<void (VehicleBackend::*)()>(_a, &VehicleBackend::criticalAlertChanged, 11))
            return;
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast<float*>(_v) = _t->speed(); break;
        case 1: *reinterpret_cast<float*>(_v) = _t->power(); break;
        case 2: *reinterpret_cast<float*>(_v) = _t->battery(); break;
        case 3: *reinterpret_cast<float*>(_v) = _t->temp(); break;
        case 4: *reinterpret_cast<float*>(_v) = _t->vibTotal(); break;
        case 5: *reinterpret_cast<float*>(_v) = _t->current(); break;
        case 6: *reinterpret_cast<float*>(_v) = _t->voltage(); break;
        case 7: *reinterpret_cast<bool*>(_v) = _t->speedWarning(); break;
        case 8: *reinterpret_cast<bool*>(_v) = _t->tempWarning(); break;
        case 9: *reinterpret_cast<bool*>(_v) = _t->vibWarning(); break;
        case 10: *reinterpret_cast<bool*>(_v) = _t->voltageWarning(); break;
        case 11: *reinterpret_cast<bool*>(_v) = _t->criticalAlert(); break;
        default: break;
        }
    }
}

const QMetaObject *VehicleBackend::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *VehicleBackend::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14VehicleBackendE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int VehicleBackend::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 12;
    }
    return _id;
}

// SIGNAL 0
void VehicleBackend::speedChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void VehicleBackend::powerChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void VehicleBackend::batteryChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void VehicleBackend::tempChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void VehicleBackend::vibTotalChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void VehicleBackend::currentChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void VehicleBackend::voltageChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void VehicleBackend::speedWarningChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void VehicleBackend::tempWarningChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}

// SIGNAL 9
void VehicleBackend::vibWarningChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 9, nullptr);
}

// SIGNAL 10
void VehicleBackend::voltageWarningChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void VehicleBackend::criticalAlertChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 11, nullptr);
}
QT_WARNING_POP
