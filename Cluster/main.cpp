#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "cluster.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    VehicleBackend backend;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("Vehicle", &backend);

    const QUrl url(QStringLiteral("qrc:/qt/qml/Cluster/Main.qml"));
    engine.load(url);
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
