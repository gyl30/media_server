#include <vector>
#include <cstring>

#include <QFile>
#include <SDL.h>
#include <QTimer>
#include <QThread>
#include <QPointer>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QTextStream>
#include <QGridLayout>
#include <QScopeGuard>
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QCommandLineParser>
#include <QCommandLineOption>

#include "desktop/playback_view.h"
#include "desktop/platform_client.h"

namespace media_server
{
namespace
{

QJsonValue timestamp(qint64 value)
{
    // 使用整数字符串，避免 JSON 消费方损失时间戳精度。
    return value == no_media_timestamp ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::number(value));
}

QJsonObject snapshot_json(const playback_view& view)
{
    const auto stats = view.snapshot();
    return {{"video_frames", QString::number(stats.video_frames)},
            {"audio_frames", QString::number(stats.audio_frames)},
            {"audio_samples", QString::number(stats.audio_samples)},
            {"pcm_samples", QString::number(stats.pcm_samples)},
            {"rendered_frames", QString::number(view.rendered_frames())},
            {"video_dropped", QString::number(stats.video_dropped)},
            {"audio_dropped", QString::number(stats.audio_dropped)},
            {"first_video_pts_us", timestamp(stats.first_video_pts_us)},
            {"last_video_pts_us", timestamp(stats.last_video_pts_us)},
            {"first_audio_pts_us", timestamp(stats.first_audio_pts_us)},
            {"last_audio_pts_us", timestamp(stats.last_audio_pts_us)},
            {"video_width", stats.video_width},
            {"video_height", stats.video_height},
            {"video_fps", stats.video_fps},
            {"audio_sample_rate", stats.audio_sample_rate},
            {"audio_channels", stats.audio_channels},
            {"video_codec", stats.video_codec},
            {"audio_codec", stats.audio_codec},
            {"decoder_error", !stats.error.isEmpty()},
            {"ended", stats.ended},
            {"audible", view.audible()}};
}

}    // namespace
}    // namespace media_server

int main(int argc, char** argv)
{
    using namespace media_server;
    QStringList arguments;
    std::vector<char*> qt_arguments;
    for (int index = 0; index < argc; ++index)
    {
        arguments.append(QString::fromLocal8Bit(argv[index]));
        // --platform 属于本探针，不传给 Qt 的 QPA 平台选择器。
        if (index > 0 && std::strcmp(argv[index], "--platform") == 0)
        {
            if (index + 1 < argc)
            {
                arguments.append(QString::fromLocal8Bit(argv[++index]));
            }
            continue;
        }
        if (index > 0 && std::strncmp(argv[index], "--platform=", 11) == 0)
        {
            continue;
        }
        qt_arguments.push_back(argv[index]);
    }
    int qt_argc = static_cast<int>(qt_arguments.size());
    qt_arguments.push_back(nullptr);
    QApplication app(qt_argc, qt_arguments.data());
    app.setApplicationName(QStringLiteral("playback_probe"));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Real platform RTSP playback probe; each view requests its own one-shot ticket. "
                       "No URL or token is written to the JSON report. Set QT_QPA_PLATFORM=offscreen for headless use."));
    parser.addHelpOption();
    const QCommandLineOption platform_option(
        QStringLiteral("platform"), QStringLiteral("Platform HTTP(S) base URL, including an optional gateway prefix."), QStringLiteral("URL"));
    const QCommandLineOption push_option(
        QStringLiteral("push-device"), QStringLiteral("Existing ready push-device ID (not a publish/play token)."), QStringLiteral("ID"));
    const QCommandLineOption source_option(QStringLiteral("source-id"), QStringLiteral("Configured active source ID."), QStringLiteral("ID"));
    const QCommandLineOption gb_device_option(
        QStringLiteral("device"), QStringLiteral("Registered GB28181 device ID; requires --channel."), QStringLiteral("ID"));
    const QCommandLineOption gb_channel_option(
        QStringLiteral("channel"), QStringLiteral("GB28181 channel ID; requires --device."), QStringLiteral("ID"));
    const QCommandLineOption views_option(
        QStringLiteral("views"), QStringLiteral("Independent views, 1..16."), QStringLiteral("N"), QStringLiteral("1"));
    const QCommandLineOption seconds_option(QStringLiteral("seconds"),
                                            QStringLiteral("Observation seconds after every view has decoded its video/audio tracks, 1..3600."),
                                            QStringLiteral("N"),
                                            QStringLiteral("5"));
    const QCommandLineOption output_option(
        QStringLiteral("output"), QStringLiteral("New JSON result file; an existing file is never overwritten."), QStringLiteral("PATH"));
    const QCommandLineOption audible_option(QStringLiteral("audible"),
                                            QStringLiteral("Enable audio output on the first view only; default is muted decoding."));
    const QCommandLineOption toggle_audio_option(
        QStringLiteral("toggle-audio"),
        QStringLiteral("With --audible, toggle the first view every two observation seconds to exercise device cleanup."));
    parser.addOptions({platform_option,
                       push_option,
                       source_option,
                       gb_device_option,
                       gb_channel_option,
                       views_option,
                       seconds_option,
                       output_option,
                       audible_option,
                       toggle_audio_option});
    parser.process(arguments);
    bool views_valid = false;
    bool seconds_valid = false;
    const int view_count = parser.value(views_option).toInt(&views_valid);
    const int seconds = parser.value(seconds_option).toInt(&seconds_valid);
    const QUrl platform_url(parser.value(platform_option), QUrl::StrictMode);
    const bool push_target = !parser.value(push_option).isEmpty();
    const bool source_target = !parser.value(source_option).isEmpty();
    const bool gb_target = !parser.value(gb_device_option).isEmpty();
    if (!parser.positionalArguments().isEmpty() || !platform_client::valid_base_url(platform_url) || push_target + source_target + gb_target != 1 ||
        gb_target != !parser.value(gb_channel_option).isEmpty() || (parser.isSet(toggle_audio_option) && !parser.isSet(audible_option)) ||
        parser.value(output_option).isEmpty() || !views_valid || view_count < 1 || view_count > 16 || !seconds_valid || seconds < 1 || seconds > 3600)
    {
        QTextStream(stderr) << "Invalid or missing arguments; use --help.\n";
        return 2;
    }
    QFile output(parser.value(output_option));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        QTextStream(stderr) << "Cannot create a new output file.\n";
        return 2;
    }
    if (!output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    {
        QTextStream(stderr) << "Cannot restrict output file permissions.\n";
        return 2;
    }

    platform_client client;
    if (!client.set_base_url(platform_url))
    {
        return 2;
    }
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 && parser.isSet(audible_option))
    {
        QTextStream(stderr) << "Cannot initialize the requested audio output.\n";
        SDL_Quit();
        return 2;
    }
    const auto audio_cleanup = qScopeGuard([]() { SDL_Quit(); });
    QWidget window;
    window.setWindowTitle(QStringLiteral("Real RTSP playback probe"));
    auto* layout = new QGridLayout(&window);
    std::vector<playback_view*> views;
    std::vector<decoder_snapshot> previous;
    std::vector<quint64> previous_rendered(view_count);
    std::vector<qint64> first_frame_ms(view_count, -1);
    for (int index = 0; index < view_count; ++index)
    {
        auto* view = new playback_view(&window);
        view->set_audible(false);
        views.push_back(view);
        layout->addWidget(view, index / 4, index % 4);
    }
    window.resize(view_count == 1 ? 640 : 1280, view_count <= 4 ? 400 : 800);
    window.show();

    QElapsedTimer elapsed;
    elapsed.start();
    QJsonArray samples;
    QJsonArray assertions;
    QJsonArray shutdown_samples;
    QJsonArray audio_switches;
    const auto started_utc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    bool failed = false;
    bool stopping = false;
    bool shutdown_timeout = false;
    bool expected_audio_output = parser.isSet(audible_option);
    qint64 observation_start_ms = -1;
    qint64 last_sample_ms = -1;
    qint64 shutdown_start_ms = -1;
    qint64 destruction_start_ms = -1;
    qint64 last_shutdown_sample_ms = -1;
    std::vector<QPointer<decode_thread>> threads;
    std::vector<decoder_snapshot> initial;
    std::vector<quint64> initial_rendered;
    auto check = [&](const QString& name, bool passed, int view = -1)
    {
        assertions.append(QJsonObject{{"name", name}, {"view", view}, {"passed", passed}, {"elapsed_ms", elapsed.elapsed()}});
        failed = failed || !passed;
    };
    auto sample = [&](const QString& phase)
    {
        QJsonArray values;
        for (int index = 0; index < view_count; ++index)
        {
            auto value = snapshot_json(*views[index]);
            value.insert(QStringLiteral("view"), index);
            values.append(value);
        }
        samples.append(QJsonObject{{"elapsed_ms", elapsed.elapsed()}, {"phase", phase}, {"views", values}});
    };
    auto stop_all = [&]()
    {
        if (stopping)
        {
            return;
        }
        sample(QStringLiteral("before_stop"));
        stopping = true;
        shutdown_start_ms = elapsed.elapsed();
        for (auto* thread : app.findChildren<decode_thread*>())
        {
            threads.emplace_back(thread);
        }
        for (auto* view : views)
        {
            view->stop();
        }
    };
    for (int index = 0; index < view_count; ++index)
    {
        views[index]->set_audible(parser.isSet(audible_option) && index == 0);
        play_target target;
        target.type = gb_target ? play_target::kind::gb_channel : source_target ? play_target::kind::source : play_target::kind::push_device;
        target.id = parser.value(gb_target ? gb_device_option : source_target ? source_option : push_option);
        target.channel_id = parser.value(gb_channel_option);
        target.name = QStringLiteral("probe %1").arg(index);
        views[index]->play(client, target);
    }

    QTimer timer;
    timer.setInterval(100);
    QObject::connect(
        &timer,
        &QTimer::timeout,
        &app,
        [&]()
        {
            const auto now = elapsed.elapsed();
            if (stopping)
            {
                // 同时检查当前 QObject 树和 stop 时保存的 QPointer，不能漏掉迟到的解码线程。
                for (auto* thread : app.findChildren<decode_thread*>())
                {
                    bool known = false;
                    for (const auto& observed : threads)
                    {
                        known = known || observed == thread;
                    }
                    if (!known)
                    {
                        threads.emplace_back(thread);
                        thread->requestInterruption();
                    }
                }
                int running = 0;
                QJsonArray states;
                for (std::size_t index = 0; index < threads.size(); ++index)
                {
                    const auto& thread = threads[index];
                    const bool active = thread && thread->isRunning();
                    running += active ? 1 : 0;
                    states.append(QJsonObject{{"thread", static_cast<int>(index)},
                                              {"running", active},
                                              {"finished", !thread || thread->isFinished()},
                                              {"destroyed", !thread}});
                }
                if (last_shutdown_sample_ms < 0 || now - last_shutdown_sample_ms >= 1000 || running == 0)
                {
                    shutdown_samples.append(QJsonObject{{"elapsed_ms", now}, {"threads", states}});
                    last_shutdown_sample_ms = now;
                }
                if (running != 0 && now - shutdown_start_ms > 12'000 && !shutdown_timeout)
                {
                    shutdown_timeout = true;
                    check(QStringLiteral("decoder_shutdown_within_12_seconds"), false);
                    // 线程仍运行时不可析构 QApplication；继续事件循环，等待有限网络期限结束。
                }
                if (running == 0)
                {
                    if (destruction_start_ms < 0)
                    {
                        destruction_start_ms = now;
                        if (!shutdown_timeout)
                        {
                            shutdown_timeout = now - shutdown_start_ms > 12'000;
                            check(QStringLiteral("decoder_shutdown_within_12_seconds"), !shutdown_timeout);
                        }
                    }
                    bool destroyed = true;
                    for (const auto& thread : threads)
                    {
                        destroyed = destroyed && thread.isNull();
                    }
                    // finished 不等于已释放；给 deleteLater 最多两秒事件循环时间。
                    if (!destroyed && now - destruction_start_ms < 2000)
                    {
                        return;
                    }
                    check(QStringLiteral("decoder_objects_destroyed_within_2_seconds"), destroyed);
                    bool descendants_finished = true;
                    for (auto* thread : app.findChildren<decode_thread*>())
                    {
                        descendants_finished = descendants_finished && !thread->isRunning();
                    }
                    check(QStringLiteral("no_running_decoder_descendants"), descendants_finished);
                    int passed = 0;
                    for (const auto& value : assertions)
                    {
                        passed += value.toObject().value(QStringLiteral("passed")).toBool() ? 1 : 0;
                    }
                    QJsonArray first_frames;
                    for (const auto value : first_frame_ms)
                    {
                        first_frames.append(value < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(value));
                    }
                    QJsonObject result{{"schema", 1},
                                       {"started_utc", started_utc},
                                       {"qt_version", QString::fromLatin1(qVersion())},
                                       {"process_id", QString::number(QCoreApplication::applicationPid())},
                                       {"qpa_platform", QGuiApplication::platformName()},
                                       {"first_frame_ms", first_frames},
                                       {"views", view_count},
                                       {"requested_seconds", seconds},
                                       {"first_frame_deadline_ms", 15'000},
                                       {"observation_start_ms", observation_start_ms},
                                       {"audible_first_requested", parser.isSet(audible_option)},
                                       {"audio_switches", audio_switches},
                                       {"audio_output_evidence", QStringLiteral("audible flag and decoded PCM only; not acoustic measurement")},
                                       {"elapsed_ms", now},
                                       {"shutdown_wait_ms", now - shutdown_start_ms},
                                       {"destruction_wait_ms", now - destruction_start_ms},
                                       {"shutdown_timeout", shutdown_timeout},
                                       {"passed_assertions", passed},
                                       {"failed_assertions", assertions.size() - passed},
                                       {"exit_status", failed ? 1 : 0},
                                       {"result", failed ? QStringLiteral("FAIL") : QStringLiteral("PASS")},
                                       {"assertions", assertions},
                                       {"samples", samples},
                                       {"shutdown_samples", shutdown_samples}};
                    const auto data = QJsonDocument(result).toJson(QJsonDocument::Indented);
                    const bool saved = output.write(data) == data.size() && output.flush();
                    output.close();
                    timer.stop();
                    app.exit(saved ? (failed ? 1 : 0) : 2);
                }
                return;
            }

            bool all_ready = true;
            for (int index = 0; index < view_count; ++index)
            {
                const auto stats = views[index]->snapshot();
                if (stats.video_frames > 0 && first_frame_ms[index] < 0)
                {
                    first_frame_ms[index] = now;
                    check(QStringLiteral("first_video_frame_within_15_seconds"), now <= 15'000, index);
                }
                const bool ready =
                    stats.video_frames > 0 && views[index]->rendered_frames() > 0 && (stats.audio_codec.isEmpty() || stats.audio_frames > 0);
                all_ready = all_ready && ready;
                if (!stats.error.isEmpty() || stats.ended)
                {
                    check(QStringLiteral("decoder_live_without_error"), false, index);
                }
            }
            if (failed)
            {
                stop_all();
                return;
            }
            if (observation_start_ms < 0)
            {
                if (all_ready)
                {
                    check(QStringLiteral("all_views_dual_track_or_video_only_ready"), true);
                    observation_start_ms = now;
                    last_sample_ms = now;
                    for (int index = 0; index < view_count; ++index)
                    {
                        previous.push_back(views[index]->snapshot());
                        previous_rendered[index] = views[index]->rendered_frames();
                        initial.push_back(previous[index]);
                        initial_rendered.push_back(previous_rendered[index]);
                        check(QStringLiteral("h264_with_dimensions_and_pts"),
                              previous[index].video_codec == QStringLiteral("h264") && previous[index].video_width > 0 &&
                                  previous[index].video_height > 0 && previous[index].last_video_pts_us != no_media_timestamp,
                              index);
                    }
                    sample(QStringLiteral("observation_start"));
                }
                else if (now >= 15'000)
                {
                    check(QStringLiteral("all_views_ready_within_15_seconds"), false);
                    stop_all();
                }
                else if (last_sample_ms < 0 || now - last_sample_ms >= 1000)
                {
                    sample(QStringLiteral("waiting_ready"));
                    last_sample_ms = now;
                }
                return;
            }
            const bool expected_audible =
                parser.isSet(audible_option) && (!parser.isSet(toggle_audio_option) || ((now - observation_start_ms) / 2000) % 2 == 0);
            if (parser.isSet(toggle_audio_option) && expected_audio_output != expected_audible)
            {
                check(QStringLiteral("audio_output_before_explicit_toggle"), views.front()->audible() == expected_audio_output, 0);
                expected_audio_output = expected_audible;
                QElapsedTimer change;
                change.start();
                views.front()->set_audible(expected_audible);
                audio_switches.append(
                    QJsonObject{{"elapsed_ms", now}, {"audible", expected_audible}, {"call_us", QString::number(change.nsecsElapsed() / 1000)}});
            }
            if (now - last_sample_ms >= 1000 || now - observation_start_ms >= seconds * 1000)
            {
                sample(QStringLiteral("observing"));
                for (int index = 0; index < view_count; ++index)
                {
                    const auto stats = views[index]->snapshot();
                    const auto& before = previous[index];
                    check(QStringLiteral("video_decode_and_pts_growing"),
                          stats.video_frames > before.video_frames && stats.last_video_pts_us > before.last_video_pts_us,
                          index);
                    check(QStringLiteral("rendered_frames_growing"), views[index]->rendered_frames() > previous_rendered[index], index);
                    check(QStringLiteral("audio_track_unchanged"), stats.audio_codec == before.audio_codec, index);
                    if (!stats.audio_codec.isEmpty())
                    {
                        check(QStringLiteral("audio_decode_pcm_and_pts_growing"),
                              stats.audio_frames > before.audio_frames && stats.audio_samples > before.audio_samples &&
                                  stats.pcm_samples > before.pcm_samples && before.last_audio_pts_us != no_media_timestamp &&
                                  stats.last_audio_pts_us > before.last_audio_pts_us,
                              index);
                        if (index == 0 && parser.isSet(audible_option))
                        {
                            check(QStringLiteral("requested_audio_output_state_matches"), views[index]->audible() == expected_audible, index);
                        }
                    }
                    previous[index] = stats;
                    previous_rendered[index] = views[index]->rendered_frames();
                }
                last_sample_ms = now;
                if (failed || now - observation_start_ms >= seconds * 1000)
                {
                    check(QStringLiteral("requested_observation_completed"), now - observation_start_ms >= seconds * 1000);
                    for (int index = 0; index < view_count; ++index)
                    {
                        const auto stats = views[index]->snapshot();
                        const auto decoded = stats.video_frames - initial[index].video_frames;
                        const auto rendered = views[index]->rendered_frames() - initial_rendered[index];
                        check(QStringLiteral("at_least_95_percent_decoded_video_rendered"), decoded > 0 && rendered * 100 >= decoded * 95, index);
                        if (stats.video_fps > 0)
                        {
                            const auto expected = stats.video_fps * (now - observation_start_ms) / 1000.0;
                            check(QStringLiteral("at_least_95_percent_nominal_video_rate"), decoded >= expected * 0.95, index);
                        }
                        if (!stats.audio_codec.isEmpty())
                        {
                            const auto expected = 48.0 * (now - observation_start_ms);
                            check(QStringLiteral("at_least_95_percent_resampled_audio_rate"),
                                  stats.pcm_samples - initial[index].pcm_samples >= expected * 0.95,
                                  index);
                        }
                    }
                    stop_all();
                }
            }
        });
    timer.start();
    return app.exec();
}
