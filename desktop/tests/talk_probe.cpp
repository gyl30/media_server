#include <SDL.h>
#include <QTimer>
#include <QVector>
#include <QJsonArray>
#include <QTextStream>
#include <QJsonObject>
#include <QJsonDocument>
#include <QApplication>
#include <QElapsedTimer>

#include "desktop/talk_session.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    bool valid_seconds = true;
    const int seconds = argc == 4 ? QString::fromLocal8Bit(argv[3]).toInt(&valid_seconds) : 2;
    if ((argc != 3 && argc != 4) || !valid_seconds || seconds < 2 || seconds > 30)
    {
        QTextStream(stderr) << "usage: desktop_talk_probe PLATFORM DEVICE_ID [SECONDS:2..30]" << Qt::endl;
        return 2;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        return 2;
    }
    int result;
    {
        media_server::talk_session session(QUrl(QString::fromLocal8Bit(argv[1])), QString::fromLocal8Bit(argv[2]));
        quint64 packets = 0;
        QVector<quint64> second_packets(seconds);
        QElapsedTimer clock;
        QElapsedTimer transmitting;
        qint64 observed_ms = 0;
        qint64 last_progress_ms = 0;
        QJsonArray observations;
        QTimer observe;
        observe.setTimerType(Qt::PreciseTimer);
        clock.start();
        QObject::connect(&session,
                         &media_server::talk_session::progress,
                         &app,
                         [&](quint64 count)
                         {
                             last_progress_ms = transmitting.elapsed();
                             const auto second = last_progress_ms / 1000;
                             if (second < seconds)
                             {
                                 second_packets[second] += count - packets;
                             }
                             packets = count;
                         });
        const auto record_progress = [&]()
        {
            const auto now = transmitting.elapsed();
            observations.append(QJsonObject{{QStringLiteral("elapsed_ms"), clock.elapsed()},
                                            {QStringLiteral("transmitting_ms"), now},
                                            {QStringLiteral("packets"), static_cast<qint64>(packets)}});
        };
        QObject::connect(&observe, &QTimer::timeout, &app, record_progress);
        QObject::connect(&session,
                         &media_server::talk_session::transmitting,
                         &app,
                         [&]()
                         {
                             transmitting.start();
                             observe.start(1000);
                             QTimer::singleShot(seconds * 1000,
                                                Qt::PreciseTimer,
                                                &session,
                                                [&]()
                                                {
                                                    observe.stop();
                                                    observed_ms = transmitting.elapsed();
                                                    record_progress();
                                                    session.stop();
                                                });
                         });
        QObject::connect(&session,
                         &media_server::talk_session::finished,
                         &app,
                         [&](const QString& error)
                         {
                             observe.stop();
                             bool progressing = true;
                             QJsonArray packets_per_second;
                             for (const auto count : second_packets)
                             {
                                 progressing = progressing && count >= 25;
                                 packets_per_second.append(static_cast<qint64>(count));
                             }
                             const bool complete = observed_ms >= seconds * 1000 && packets >= static_cast<quint64>(seconds * 25) && progressing &&
                                                   observed_ms - last_progress_ms < 1000;
                             const QJsonObject report{{QStringLiteral("packets"), static_cast<qint64>(packets)},
                                                      {QStringLiteral("progress_scope"), QStringLiteral("local_ws_enqueue")},
                                                      {QStringLiteral("requested_seconds"), seconds},
                                                      {QStringLiteral("observed_ms"), observed_ms},
                                                      {QStringLiteral("last_progress_ms"), last_progress_ms},
                                                      {QStringLiteral("packets_per_second"), packets_per_second},
                                                      {QStringLiteral("elapsed_ms"), clock.elapsed()},
                                                      {QStringLiteral("observations"), observations},
                                                      {QStringLiteral("complete"), complete},
                                                      {QStringLiteral("error"), error}};
                             QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << Qt::endl;
                             app.exit(error.isEmpty() && complete ? 0 : 1);
                         });
        QTimer::singleShot(27'000 + seconds * 1000, &session, [&]() { session.stop(QStringLiteral("probe deadline")); });
        QTimer::singleShot(0, &session, &media_server::talk_session::start);
        result = app.exec();
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return result;
}
