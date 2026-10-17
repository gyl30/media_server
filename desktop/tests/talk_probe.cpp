#include <SDL.h>
#include <QTimer>
#include <QTextStream>
#include <QJsonObject>
#include <QJsonDocument>
#include <QApplication>

#include "desktop/talk_session.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc != 3 || SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        return 2;
    }
    int result;
    {
        media_server::talk_session session(QUrl(QString::fromLocal8Bit(argv[1])), QString::fromLocal8Bit(argv[2]));
        quint64 packets = 0;
        QObject::connect(&session, &media_server::talk_session::progress, &app, [&](quint64 count) { packets = count; });
        QObject::connect(
            &session, &media_server::talk_session::transmitting, &app, [&]() { QTimer::singleShot(2000, &session, [&]() { session.stop(); }); });
        QObject::connect(&session,
                         &media_server::talk_session::finished,
                         &app,
                         [&](const QString& error)
                         {
                             const QJsonObject report{{QStringLiteral("packets"), static_cast<qint64>(packets)}, {QStringLiteral("error"), error}};
                             QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << Qt::endl;
                             app.exit(error.isEmpty() && packets >= 50 ? 0 : 1);
                         });
        QTimer::singleShot(25'000, &session, [&]() { session.stop(QStringLiteral("probe deadline")); });
        QTimer::singleShot(0, &session, &media_server::talk_session::start);
        result = app.exec();
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return result;
}
