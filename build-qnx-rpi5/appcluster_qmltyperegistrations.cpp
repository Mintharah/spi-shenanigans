/****************************************************************************
** Generated QML type registration code
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <QtQml/qqml.h>
#include <QtQml/qqmlmoduleregistration.h>

#if __has_include(<cluster.h>)
#  include <cluster.h>
#endif


#if !defined(QT_STATIC)
#define Q_QMLTYPE_EXPORT Q_DECL_EXPORT
#else
#define Q_QMLTYPE_EXPORT
#endif
Q_QMLTYPE_EXPORT void qml_register_types_Cluster()
{
    qmlRegisterModule("Cluster", 254, 0);
    QT_WARNING_PUSH QT_WARNING_DISABLE_DEPRECATED
    qmlRegisterTypesAndRevisions<VehicleBackend>("Cluster", 254);
    QT_WARNING_POP
    qmlRegisterModule("Cluster", 254, 254);
}

static const QQmlModuleRegistration clusterRegistration("Cluster", qml_register_types_Cluster);
