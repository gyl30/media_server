#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>

#include <QDir>
#include <SDL.h>
#include <QFile>
#include <QTest>
#include <QTimer>
#include <QThread>
#include <QSignalSpy>
#include <QEventLoop>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QCoreApplication>

#include "desktop/decode_thread.h"
#include "desktop/tests/finite_playback_fixture.h"

extern "C"
{
#include <libswresample/swresample.h>
}

namespace
{

struct tone_point
{
    quint64 frame;
    quint64 cycles[2];
};

struct pcm_statistics
{
    quint64 frames{};
    quint64 cycles[2]{};
    quint64 square_sum[2]{};
    quint64 nonempty_clears{};
    quint64 clear_observed_frames{};
    int16_t previous[2]{};
    bool has_previous{};
    bool aligned{true};
    QList<tone_point> phase{{0, {0, 0}}};

    void append(const void* data, Uint32 bytes)
    {
        aligned = aligned && bytes % 4 == 0;
        const auto* input = static_cast<const unsigned char*>(data);
        for (Uint32 offset = 0; offset + 4 <= bytes; offset += 4)
        {
            int16_t stereo[2];
            std::memcpy(stereo, input + offset, sizeof(stereo));
            for (int channel = 0; channel < 2; ++channel)
            {
                if (has_previous && previous[channel] <= 0 && stereo[channel] > 0)
                {
                    ++cycles[channel];
                }
                const qint64 value = stereo[channel];
                square_sum[channel] += static_cast<quint64>(value * value);
                previous[channel] = stereo[channel];
            }
            has_previous = true;
            ++frames;
            // 只保存每100ms输出样本的累计phase，不保存全PCM；cursor插值是数字估计，不是可闻位置。
            if (frames % 4800 == 0)
            {
                phase.append(tone_point{frames, {cycles[0], cycles[1]}});
            }
        }
    }

    double rms(int channel) const { return frames == 0 ? 0 : std::sqrt(static_cast<double>(square_sum[channel]) / frames); }

    double phase_at(quint64 frame, int channel) const
    {
        frame = std::min(frame, frames);
        const auto upper =
            std::lower_bound(phase.cbegin(), phase.cend(), frame, [](const auto& point, quint64 value) { return point.frame < value; });
        if (upper == phase.cbegin())
        {
            return static_cast<double>(upper->cycles[channel]);
        }
        const auto& lower = upper == phase.cend() ? phase.back() : *(upper - 1);
        const quint64 end_frame = upper == phase.cend() ? frames : upper->frame;
        const quint64 end_cycles = upper == phase.cend() ? cycles[channel] : upper->cycles[channel];
        if (end_frame == lower.frame)
        {
            return static_cast<double>(lower.cycles[channel]);
        }
        return lower.cycles[channel] + static_cast<double>(end_cycles - lower.cycles[channel]) * (frame - lower.frame) / (end_frame - lower.frame);
    }
};

// QueueAudio和以下统计均在GUI线程调用；不碰SDL后端线程，不替换实际音频输出。
pcm_statistics* queued_pcm{};
SDL_AudioDeviceID tracked_audio{};

struct swr_statistics
{
    quint64 normal_calls{};
    quint64 input_frames{};
    quint64 normal_output_frames{};
    quint64 flush_calls{};
    quint64 flush_output_frames{};
};

// Only the GUI thread reads this pointer; decoder-thread conversions remain unobserved.
swr_statistics* gui_swr{};

}    // namespace

extern "C" int SDLCALL __real_SDL_QueueAudio(SDL_AudioDeviceID device, const void* data, Uint32 bytes);

extern "C" int SDLCALL __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void* data, Uint32 bytes)
{
    const int result = __real_SDL_QueueAudio(device, data, bytes);
    if (result == 0 && queued_pcm && device == tracked_audio)
    {
        queued_pcm->append(data, bytes);
    }
    return result;
}

extern "C" void SDLCALL __real_SDL_ClearQueuedAudio(SDL_AudioDeviceID device);

extern "C" void SDLCALL __wrap_SDL_ClearQueuedAudio(SDL_AudioDeviceID device)
{
    auto* const statistics = queued_pcm && device == tracked_audio ? queued_pcm : nullptr;
    const auto before = statistics ? SDL_GetQueuedAudioSize(device) : 0;
    __real_SDL_ClearQueuedAudio(device);
    if (before != 0)
    {
        ++statistics->nonempty_clears;
        // SDL后端可并行取走数据；这是调用前观察到的排队帧数，不冒充精确丢弃量。
        statistics->clear_observed_frames += before / 4;
    }
}

extern "C" int __real_swr_convert(SwrContext* context, uint8_t** output, int output_count, const uint8_t** input, int input_count);

extern "C" int __wrap_swr_convert(SwrContext* context, uint8_t** output, int output_count, const uint8_t** input, int input_count)
{
    const int result = __real_swr_convert(context, output, output_count, input, input_count);
    auto* const application = QCoreApplication::instance();
    if (application && QThread::currentThread() == application->thread() && gui_swr && result >= 0)
    {
        if (input == nullptr && input_count == 0)
        {
            ++gui_swr->flush_calls;
            gui_swr->flush_output_frames += static_cast<quint64>(result);
        }
        else
        {
            ++gui_swr->normal_calls;
            gui_swr->input_frames += static_cast<quint64>(input_count);
            gui_swr->normal_output_frames += static_cast<quint64>(result);
        }
    }
    return result;
}

namespace media_server
{

class scale_playback_view_test final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase()
    {
        had_audio_driver_ = qEnvironmentVariableIsSet("SDL_AUDIODRIVER");
        old_audio_driver_ = qgetenv("SDL_AUDIODRIVER");
        QVERIFY(qputenv("SDL_AUDIODRIVER", "dummy"));
        QVERIFY2(SDL_Init(SDL_INIT_AUDIO) == 0, SDL_GetError());
    }

    void cleanupTestCase()
    {
        SDL_Quit();
        if (had_audio_driver_)
        {
            qputenv("SDL_AUDIODRIVER", old_audio_driver_);
        }
        else
        {
            qunsetenv("SDL_AUDIODRIVER");
        }
    }

    void init() { QVERIFY(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty()); }

    void cleanup()
    {
        // 只用于失败回收；成功必须先通过用例自己的 EOF/资源析构屏障。
        for (auto* decoder : QCoreApplication::instance()->findChildren<decode_thread*>())
        {
            decoder->requestInterruption();
            if (!decoder->wait(4000))
            {
                qFatal("倍速测试失败回收时 decoder 未退出");
            }
            decoder->deleteLater();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY_WITH_TIMEOUT(QCoreApplication::instance()->findChildren<decode_thread*>().isEmpty(), 3000);
    }

    void short_audio_tail_only()
    {
        const auto folder = qEnvironmentVariable("SCALE_SHORT_AUDIO_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定独立短尾MKV素材；不代表短尾呈现验收");
        }
        const auto path = QDir(folder).filePath(QStringLiteral("tail-32.mkv"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto payload = file.readAll();
        QCOMPARE(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex(),
                 QByteArray("f6d31381d01d02e9418999a84e0b734ee2eca7173e9e6c626e3bb41bee9d5d7e"));
        QList<QByteArray> expected;
        qsizetype reference_pcm = 0;
        pcm_statistics reference_tone;
        decode_reference(path, expected, reference_pcm, reference_tone);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(expected.size(), qsizetype(50));
        QCOMPARE(reference_pcm, qsizetype(32 * 4));
        QCOMPARE(reference_tone.frames, quint64(32));

        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.setFixedSize(320, 240);
        painted_frames painted(view);
        view.show();
        pcm_statistics output_tone;
        swr_statistics conversion;
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        const auto audio_id = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        bool lost_audio_while_active = false;
        const auto close_audio = qScopeGuard(
            [&]()
            {
                view.stop();
                gui_swr = nullptr;
                queued_pcm = nullptr;
                tracked_audio = 0;
                if (audio_id != 0)
                {
                    SDL_CloseAudioDevice(audio_id);
                }
            });
        QVERIFY2(audio_id != 0, SDL_GetError());
        QCOMPARE(have.freq, 48000);
        QCOMPARE(have.format, Uint16(AUDIO_S16SYS));
        QCOMPARE(have.channels, Uint8(2));
        QSignalSpy errors(&view, &playback_view::playback_error);
        connect(&view,
                &playback_view::audible_changed,
                &view,
                [&](bool active)
                {
                    if (!active && view.rendered_frames() != static_cast<quint64>(expected.size()))
                    {
                        lost_audio_while_active = true;
                    }
                });
        view.play_record(client, historical_target(), 1760000000, 1760000002);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QCOMPARE(api.requests[0].line, create_line);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        const auto media_socket = media.requests[0].socket;
        QCOMPARE(media.requests[0].line, "GET /play/gb28181/" + first_id.toUtf8() + "/fixture-token.ps HTTP/1.1");
        const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(decoders.size(), qsizetype(1));
        QPointer<decode_thread> decoder = decoders.front();
        QVERIFY(media.stream(0, payload));
        QTRY_VERIFY_WITH_TIMEOUT(view.rendered_frames() >= 3 && view.record_seek_ready(), 4000);
        QVERIFY(!decoder->isFinished());
        QVERIFY(!view.audible());
        view.scale_record(1);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(2), 2000);
        QCOMPARE(api.requests[1].line, control_line("scale"));
        QCOMPARE(QJsonDocument::fromJson(api.requests[1].body).object(), (QJsonObject{{"scale", 1}}));
        QVERIFY(api.respond(1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(view.record_seek_ready(), 2000);
        QVERIFY(!decoder->isFinished());
        gui_swr = &conversion;
        queued_pcm = &output_tone;
        tracked_audio = audio_id;
        view.set_audible(audio_id, have.samples * 4);
        QTRY_VERIFY_WITH_TIMEOUT(view.status().startsWith(QStringLiteral("录像传输及呈现已结束")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(3), 2000);
        QCOMPARE(api.requests[2].line, delete_line());
        QVERIFY(api.respond(2, 204));
        QTRY_VERIFY_WITH_TIMEOUT(operations.front().isNull() && media_socket.isNull(), 3000);
        QCOMPARE(media.requests.size(), qsizetype(1));
        QCOMPARE(api.requests.size(), qsizetype(3));
        QVERIFY(!api.parse_error && !media.parse_error);
        QVERIFY(errors.isEmpty());
        QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
        QCOMPARE(view.snapshot().video_frames, quint64(50));
        QCOMPARE(view.rendered_frames(), quint64(50));
        QCOMPARE(view.snapshot().audio_samples, quint64(32));
        QCOMPARE(view.snapshot().pcm_samples, quint64(32));
        QCOMPARE(view.snapshot().video_dropped, quint64(0));
        QCOMPARE(view.snapshot().audio_dropped, quint64(0));
        QVERIFY(!lost_audio_while_active);
        QVERIFY(!view.audible());
        QCOMPARE(SDL_GetQueuedAudioSize(audio_id), Uint32(0));
        QCOMPARE(SDL_GetAudioDeviceStatus(audio_id), SDL_AUDIO_PAUSED);
        QVERIFY(output_tone.aligned && output_tone.frames != 0);
        QCOMPARE(output_tone.nonempty_clears, quint64(0));
        QCOMPARE(output_tone.clear_observed_frames, quint64(0));
        QVERIFY(conversion.normal_calls != 0);
        QCOMPARE(conversion.input_frames, quint64(32));
        QCOMPARE(conversion.normal_output_frames, quint64(0));
        QVERIFY(conversion.flush_calls >= 2);
        QVERIFY(conversion.flush_output_frames != 0);
        QCOMPARE(output_tone.frames, conversion.flush_output_frames);
        QTRY_COMPARE_WITH_TIMEOUT(painted.hashes.size(), expected.size(), 1000);
        QCOMPARE(painted.hashes, expected);
        for (qsizetype index = 0; index < painted.indices.size(); ++index)
        {
            QCOMPARE(painted.indices[index], quint64(index + 1));
        }
        qInfo() << "scale_short_tail source_frames=32 normal_calls=" << conversion.normal_calls << "normal_output=" << conversion.normal_output_frames
                << "flush_calls=" << conversion.flush_calls << "flush_output=" << conversion.flush_output_frames
                << "queued_output=" << output_tone.frames << "painted=50 GET=1 scale=1 DELETE=1; MKV公共API短尾回归，非PS/相机/声学验收";
    }

    void timed_scale_pacing_data()
    {
        QTest::addColumn<double>("requested_scale");
        QTest::addColumn<int>("actual_numerator");
        QTest::addColumn<int>("actual_denominator");
        QTest::addColumn<int>("repeat_count");
        QTest::addColumn<bool>("audible");
        QTest::addColumn<int>("finish_deadline_ms");
        QTest::addColumn<bool>("pause_after_scale");
        QTest::newRow("request-2-actual-2-return-1-muted") << 2.0 << 2 << 1 << 8 << false << 74000 << false;
        QTest::newRow("request-2-actual-1-muted") << 2.0 << 1 << 1 << 8 << false << 74000 << false;
        QTest::newRow("request-2-actual-2-return-1-audible") << 2.0 << 2 << 1 << 8 << true << 74000 << false;
        QTest::newRow("request-2-actual-1-audible") << 2.0 << 1 << 1 << 8 << true << 74000 << false;
        QTest::newRow("request-0.25-actual-0.25-return-1-audible") << 0.25 << 1 << 4 << 8 << true << 86000 << false;
        QTest::newRow("request-0.5-actual-0.5-return-1-audible") << 0.5 << 1 << 2 << 8 << true << 82000 << false;
        QTest::newRow("request-4-actual-4-return-1-audible") << 4.0 << 4 << 1 << 12 << true << 106000 << false;
        QTest::newRow("request-4-actual-3.43-return-1-audible") << 4.0 << 343 << 100 << 12 << true << 106000 << false;
        QTest::newRow("request-2-actual-2-pause-resume-audible") << 2.0 << 2 << 1 << 8 << true << 74000 << true;
    }

    void timed_scale_pacing()
    {
        QFETCH(double, requested_scale);
        QFETCH(int, actual_numerator);
        QFETCH(int, actual_denominator);
        QFETCH(int, repeat_count);
        QFETCH(bool, audible);
        QFETCH(int, finish_deadline_ms);
        QFETCH(bool, pause_after_scale);
        const double actual_rate = static_cast<double>(actual_numerator) / actual_denominator;
        const qint64 source_duration_us = qint64(repeat_count) * 8000000;
        const qint64 changed_source_end_us =
            pause_after_scale ? source_duration_us : 8000000 + qint64(16000000) * actual_numerator / actual_denominator;
        const qint64 changed_wall_end_us = 8000000 + (changed_source_end_us - 8000000) * actual_denominator / actual_numerator;
        const auto folder = qEnvironmentVariable("FINITE_PS_TEST_FIXTURE_DIR");
        if (folder.isEmpty())
        {
            QSKIP("未指定私有PS素材；不代表倍速呈现验收");
        }
        const auto path = QDir(folder).filePath(QStringLiteral("pcma-long-noend.ps"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll();
        QCOMPARE(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex(),
                 QByteArray("19b8925fe5031cbf35f08c10131b0edf9b00ca466b62937958fb6f8cc3d2dcdd"));
        QList<QByteArray> reference;
        qsizetype reference_pcm = 0;
        pcm_statistics reference_tone;
        decode_reference(path, reference, reference_pcm, reference_tone);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(reference.size(), qsizetype(200));
        QCOMPARE(reference_pcm, qsizetype(384000 * 4));
        QCOMPARE(reference_tone.frames, quint64(384000));
        QVERIFY(reference_tone.aligned);
        for (int channel = 0; channel < 2; ++channel)
        {
            QVERIFY(reference_tone.rms(channel) > 100);
        }
        QByteArray source;
        QList<QByteArray> expected;
        for (int repeat = 0; repeat < repeat_count; ++repeat)
        {
            source += shifted_pes_timestamps(original, repeat * 8 * 90000);
            expected += reference;
        }

        // 仅接受上面固定的双轨 PS fixture；不是任意 PS/损坏 PES 的通用解析器。
        struct timed_pack
        {
            QByteArray bytes;
            qint64 pts_us;
        };
        QList<timed_pack> packs;
        const QByteArray pack_prefix("\0\0\1\xba", 4);
        qsizetype position = 0;
        while (position < source.size())
        {
            QCOMPARE(source.mid(position, 4), pack_prefix);
            const auto next = source.indexOf(pack_prefix, position + 4);
            const auto end = next < 0 ? source.size() : next;
            const auto bytes = source.mid(position, end - position);
            auto pes = bytes.indexOf(QByteArray("\0\0\1\xe0", 4));
            if (pes < 0)
            {
                pes = bytes.indexOf(QByteArray("\0\0\1\xc0", 4));
            }
            QVERIFY(pes >= 0 && pes + 14 <= bytes.size());
            QVERIFY((static_cast<unsigned char>(bytes[pes + 7]) & 0x80) != 0);
            QVERIFY(static_cast<unsigned char>(bytes[pes + 8]) >= 5);
            const auto* p = reinterpret_cast<const unsigned char*>(bytes.constData() + pes + 9);
            QVERIFY((p[0] & 1) && (p[2] & 1) && (p[4] & 1));
            const quint64 pts = (quint64(p[0] & 14) << 29) | (quint64(p[1]) << 22) | (quint64(p[2] & 254) << 14) | (quint64(p[3]) << 7) | (p[4] >> 1);
            const qint64 pts_us = static_cast<qint64>(pts * 1000000 / 90000);
            QCOMPARE(pts_us, qint64(packs.size() / 2) * 40000);
            packs.append({bytes, pts_us});
            position = end;
        }
        QCOMPARE(packs.size(), qsizetype(400 * repeat_count));

        http_fixture api, media;
        QVERIFY(api.start());
        QVERIFY(media.start());
        platform_client client;
        QVERIFY(client.set_base_url(api.url()));
        playback_view view;
        view.setFixedSize(320, 240);
        painted_frames painted(view);
        view.show();
        pcm_statistics output_tone;
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        const auto audio_id = audible ? SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0) : 0;
        bool lost_audio_while_active = false;
        const auto close_audio = qScopeGuard(
            [&]()
            {
                view.stop();
                queued_pcm = nullptr;
                tracked_audio = 0;
                if (audio_id != 0)
                {
                    SDL_CloseAudioDevice(audio_id);
                }
            });
        if (audible)
        {
            QVERIFY2(audio_id != 0, SDL_GetError());
            QCOMPARE(have.freq, 48000);
            QCOMPARE(have.format, Uint16(AUDIO_S16SYS));
            QCOMPARE(have.channels, Uint8(2));
            queued_pcm = &output_tone;
            tracked_audio = audio_id;
        }
        connect(&view,
                &playback_view::audible_changed,
                &view,
                [&](bool active)
                {
                    if (!active && view.rendered_frames() != static_cast<quint64>(expected.size()))
                    {
                        lost_audio_while_active = true;
                    }
                });
        QSignalSpy errors(&view, &playback_view::playback_error);
        view.play_record(client, historical_target(), 1760000000, 1760000000 + source_duration_us / 1000000);
        const auto operations = operation_clients(client);
        QCOMPARE(operations.size(), qsizetype(1));
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), qsizetype(1), 2000);
        QCOMPARE(api.requests[0].line, create_line);
        QVERIFY(api.respond(0, 201, ticket(media)));
        QTRY_COMPARE_WITH_TIMEOUT(media.requests.size(), qsizetype(1), 2000);
        QCOMPARE(media.requests[0].line, "GET /play/gb28181/" + first_id.toUtf8() + "/fixture-token.ps HTTP/1.1");
        const auto decoders = QCoreApplication::instance()->findChildren<decode_thread*>();
        QCOMPARE(decoders.size(), qsizetype(1));
        QPointer<decode_thread> decoder = decoders.front();
        if (audible)
        {
            view.set_audible(audio_id, have.samples * 4);
        }
        const auto media_socket = media.requests[0].socket;
        qint64 bytes_written = 0;
        QObject telemetry;
        connect(media_socket, &QTcpSocket::bytesWritten, &telemetry, [&](qint64 bytes) { bytes_written += bytes; });

        // QTRY/qWait每轮休眠最多10ms，会把5ms呈现timer节流到不足100fps；运行媒体时使用真实事件循环。
        const auto await_media = [](const auto& predicate, int timeout_ms)
        {
            if (predicate())
            {
                return true;
            }
            if (timeout_ms <= 0)
            {
                return false;
            }
            QEventLoop loop;
            QTimer condition, deadline;
            condition.setTimerType(Qt::PreciseTimer);
            condition.setInterval(5);
            deadline.setTimerType(Qt::PreciseTimer);
            deadline.setSingleShot(true);
            QObject::connect(&condition, &QTimer::timeout, &loop, [&]()
            {
                if (predicate())
                {
                    loop.quit();
                }
            });
            QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            condition.start();
            deadline.start(timeout_ms);
            loop.exec();
            return predicate();
        };

        QList<pacing_sample> samples;
        QElapsedTimer elapsed;
        elapsed.start();
        qsizetype sent = 0;
        QByteArray sent_bytes;
        qint64 write_bytes = stream_header.size();
        QList<qint64> dispatch_lateness;
        bool send_ok = media.send(0, stream_header);
        bool eof_sent = false;
        // 唯一新增可变时序事实：夹具真正停止供给的墙钟时长；恢复后整体平移，不补发欠账突发。
        qint64 dispatch_offset_us = 0;
        const qint64 wire_end_ms = (changed_wall_end_us + source_duration_us - changed_source_end_us) / 1000;
        QTimer sender, sampler;
        sender.setTimerType(Qt::PreciseTimer);
        sender.setInterval(1);
        sampler.setTimerType(Qt::PreciseTimer);
        sampler.setInterval(100);
        const auto stop_timers = qScopeGuard(
            [&]()
            {
                sender.stop();
                sampler.stop();
                view.stop();
            });
        connect(&sender,
                &QTimer::timeout,
                this,
                [&]()
                {
                    while (sent < packs.size())
                    {
                        const auto pts_us = packs[sent].pts_us;
                        // 有理数整数时刻独立于请求值；3.43倍也不累计浮点步进误差。
                        const qint64 due_us = dispatch_offset_us + (pts_us < 8000000 ? pts_us
                                                                    : pts_us < changed_source_end_us
                                                                        ? 8000000 + (pts_us - 8000000) * actual_denominator / actual_numerator
                                                                        : changed_wall_end_us + pts_us - changed_source_end_us);
                        const auto now_us = elapsed.nsecsElapsed() / 1000;
                        if (due_us > now_us)
                        {
                            return;
                        }
                        dispatch_lateness.append((now_us - due_us) / 1000);
                        const auto wire = chunk(packs[sent].bytes);
                        send_ok = media.send(0, wire) && send_ok;
                        write_bytes += wire.size();
                        sent_bytes += packs[sent].bytes;
                        ++sent;
                    }
                    if (elapsed.nsecsElapsed() / 1000 >= wire_end_ms * 1000 + dispatch_offset_us)
                    {
                        send_ok = media.send(0, "0\r\n\r\n") && send_ok;
                        write_bytes += 5;
                        eof_sent = true;
                        if (media_socket)
                        {
                            media_socket->disconnectFromHost();
                        }
                        sender.stop();
                    }
                });
        connect(&sampler,
                &QTimer::timeout,
                this,
                [&]()
                {
                    const qint64 dispatch_us = sent == 0 ? 0 : packs[sent - 1].pts_us + 40000;
                    // 最终逐帧 hash/索引连续性独立校验后，索引才可解释为固定25fps素材的源时间。
                    const qint64 paint_us = painted.indices.isEmpty() ? 0 : static_cast<qint64>(painted.indices.back()) * 40000;
                    const auto queued_bytes = audio_id == 0 ? Uint32(0) : SDL_GetQueuedAudioSize(audio_id);
                    const auto audio_status = audio_id == 0 ? SDL_AUDIO_STOPPED : SDL_GetAudioDeviceStatus(audio_id);
                    const quint64 outstanding = queued_bytes / 4 + 2 * have.samples;
                    const quint64 output_cursor = output_tone.frames > outstanding ? output_tone.frames - outstanding : 0;
                    samples.append({elapsed.elapsed(),
                                    dispatch_us,
                                    paint_us,
                                    output_cursor,
                                    {output_tone.phase_at(output_cursor, 0), output_tone.phase_at(output_cursor, 1)},
                                    queued_bytes,
                                    audio_status});
                    const auto stats = view.snapshot();
                    qInfo().noquote() << "scale_sample wall_ms=" << samples.back().wall_ms << "dispatch_source_us=" << dispatch_us
                                      << "write_calls=" << sent + 1 + (eof_sent ? 1 : 0) << "write_bytes=" << write_bytes
                                      << "dispatch_last_late_ms=" << (dispatch_lateness.isEmpty() ? 0 : dispatch_lateness.back())
                                      << "bytesWritten=" << bytes_written << "bytesToWrite=" << (media_socket ? media_socket->bytesToWrite() : 0)
                                      << "paint_source_us=" << paint_us << "painted=" << painted.indices.size()
                                      << "rendered=" << view.rendered_frames() << "video_decoded=" << stats.video_frames
                                      << "audio_source_samples=" << stats.audio_samples << "pcm_generated=" << stats.pcm_samples
                                      << "video_pts_us=" << stats.last_video_pts_us << "audio_pts_us=" << stats.last_audio_pts_us
                                      << "audible=" << audible << "sdl_queued_bytes=" << queued_bytes << "sdl_status=" << audio_status
                                      << "output_frames=" << output_tone.frames << "output_cursor_estimate=" << output_cursor
                                      << "output_phase_l=" << samples.back().audio_phase[0] << "output_phase_r=" << samples.back().audio_phase[1]
                                      << "output_cycles_l=" << output_tone.cycles[0] << "output_cycles_r=" << output_tone.cycles[1]
                                      << "output_rms_l=" << output_tone.rms(0) << "output_rms_r=" << output_tone.rms(1)
                                      << "nonempty_clear_calls=" << output_tone.nonempty_clears;
                });
        sender.start();
        sampler.start();
        QVERIFY(await_media([&]() { return view.record_seek_ready(); }, 7000));
        qInfo() << "scale_first_ready_ms=" << elapsed.elapsed() << "convergence_deadline_ms=18000; 固定窗口，不搜索通过片段";
        QVERIFY(await_media([&]() { return elapsed.elapsed() >= 7000; }, 7500));
        view.scale_record(requested_scale);
        QVERIFY(await_media([&]() { return api.requests.size() == 2; }, 2000));
        QCOMPARE(api.requests[1].line, control_line("scale"));
        QCOMPARE(QJsonDocument::fromJson(api.requests[1].body).object(), (QJsonObject{{"scale", requested_scale}}));
        QVERIFY(api.respond(1, 204));
        QVERIFY(await_media([&]() { return view.record_seek_ready(); }, 2000));
        if (pause_after_scale)
        {
            // 等22秒样本已采到再暂停，不把PAUSED样本混入原18..22秒稳态窗口。
            QVERIFY(await_media([&]() { return !samples.isEmpty() && samples.back().wall_ms >= 22000; }, 18000));
            QVERIFY(elapsed.elapsed() <= 22250);
            assert_window(samples, 16000, 18000, actual_rate, audible, reference_tone);
            assert_window(samples, 18000, 22000, actual_rate, audible, reference_tone);
            QVERIFY(!QTest::currentTestFailed());
            const auto held_at_us = elapsed.nsecsElapsed() / 1000;
            sender.stop();
            view.set_record_paused(true);
            QVERIFY(await_media([&]() { return api.requests.size() == 3; }, 2000));
            QCOMPARE(api.requests[2].line, control_line("pause"));
            QVERIFY(api.respond(2, 204));
            QVERIFY(await_media([&]() { return view.record_paused(); }, 2000));
            QVERIFY(elapsed.nsecsElapsed() / 1000 - held_at_us <= 250000);
            QVERIFY(await_media([&]() { return !painted.indices.isEmpty() && painted.indices.back() == view.rendered_frames(); }, 250));
            const auto frozen_at_ms = elapsed.elapsed();
            const auto frozen_frames = view.rendered_frames();
            const auto frozen_image = view.image();
            const auto frozen_hashes = painted.hashes;
            const auto frozen_indices = painted.indices;
            const auto frozen_queue = SDL_GetQueuedAudioSize(audio_id);
            QVERIFY(!view.record_seek_ready());
            view.scale_record(4);
            QVERIFY(await_media([&]() { return elapsed.elapsed() >= frozen_at_ms + 2000; }, 2250));
            QCOMPARE(api.requests.size(), qsizetype(3));
            QVERIFY(view.record_paused() && view.audible() && decoder && !decoder->isFinished());
            QCOMPARE(view.rendered_frames(), frozen_frames);
            QCOMPARE(view.image(), frozen_image);
            QCOMPARE(painted.hashes, frozen_hashes);
            QCOMPARE(painted.indices, frozen_indices);
            QCOMPARE(SDL_GetAudioDeviceStatus(audio_id), SDL_AUDIO_PAUSED);
            // update_media可预填有界SDL队列；不要求PCM提交数或queue完全不变。
            QVERIFY(SDL_GetQueuedAudioSize(audio_id) >= frozen_queue);
            QVERIFY(std::all_of(samples.cbegin(),
                                samples.cend(),
                                [&](const auto& sample)
                                {
                                    return sample.wall_ms <= frozen_at_ms ||
                                           (sample.audio_status == SDL_AUDIO_PAUSED && sample.painted_source_us == qint64(frozen_frames) * 40000);
                                }));
            const auto resume_at_ms = elapsed.elapsed();
            view.set_record_paused(false);
            QVERIFY(await_media([&]() { return api.requests.size() == 4; }, 2000));
            QCOMPARE(api.requests[3].line, control_line("resume"));
            QVERIFY(api.respond(3, 204));
            QVERIFY(await_media([&]() { return !view.record_paused() && view.record_seek_ready(); }, 2000));
            QVERIFY(elapsed.elapsed() - resume_at_ms <= 250);
            dispatch_offset_us = elapsed.nsecsElapsed() / 1000 - held_at_us;
            QVERIFY(dispatch_offset_us <= 3000000);
            sender.start();
            qInfo() << "scale_pause_resume source_hold_wall_us=" << held_at_us << "frozen_at_ms=" << frozen_at_ms
                    << "resume_request_ms=" << resume_at_ms << "dispatch_offset_us=" << dispatch_offset_us << "frozen_frames=" << frozen_frames
                    << "; 恢复仍独立供给2倍，不再发Scale";
        }
        else
        {
            QVERIFY(await_media([&]() { return elapsed.elapsed() >= 24000; }, 18000));
            view.scale_record(1);
            QVERIFY(await_media([&]() { return api.requests.size() == 3; }, 2000));
            QCOMPARE(api.requests[2].line, control_line("scale"));
            QCOMPARE(QJsonDocument::fromJson(api.requests[2].body).object(), (QJsonObject{{"scale", 1}}));
            QVERIFY(api.respond(2, 204));
            QVERIFY(await_media([&]() { return view.record_seek_ready(); }, 2000));
        }
        QCOMPARE(QCoreApplication::instance()->findChildren<decode_thread*>().size(), qsizetype(1));
        QCOMPARE(QCoreApplication::instance()->findChildren<decode_thread*>().front(), decoder.data());
        QCOMPARE(operation_clients(client).size(), qsizetype(1));
        QCOMPARE(operation_clients(client).front(), operations.front());
        QCOMPARE(view.audible(), audible);
        // 预设绝对期限：原四行74秒，慢速86/82秒，高速106秒；不随运行结果放宽。
        const int finish_budget_ms = static_cast<int>(finish_deadline_ms - elapsed.elapsed());
        QVERIFY(await_media([&]() { return view.status().startsWith(QStringLiteral("录像传输及呈现已结束")); }, finish_budget_ms));
        sampler.stop();
        QTRY_VERIFY_WITH_TIMEOUT(decoder.isNull(), 3000);
        const qsizetype expected_requests = pause_after_scale ? 5 : 4;
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), expected_requests, 2000);
        QCOMPARE(api.requests.back().line, delete_line());
        QVERIFY(api.respond(expected_requests - 1, 204));
        QTRY_VERIFY_WITH_TIMEOUT(operations.front().isNull() && media_socket.isNull(), 3000);
        QVERIFY(eof_sent && send_ok);
        QCOMPARE(sent, packs.size());
        QCOMPARE(sent_bytes, source);
        QCOMPARE(bytes_written, write_bytes);
        QCOMPARE(api.requests.size(), expected_requests);
        QCOMPARE(media.requests.size(), qsizetype(1));
        QVERIFY(!api.parse_error && !media.parse_error);
        QVERIFY(errors.isEmpty());
        QVERIFY2(view.snapshot().error.isEmpty(), qPrintable(view.snapshot().error));
        QCOMPARE(view.snapshot().video_frames, quint64(reference.size() * repeat_count));
        QCOMPARE(view.rendered_frames(), quint64(reference.size() * repeat_count));
        QCOMPARE(view.snapshot().audio_samples, quint64(64000 * repeat_count));
        QCOMPARE(view.snapshot().pcm_samples, reference_tone.frames * repeat_count);
        QCOMPARE(view.snapshot().video_dropped, quint64(0));
        QCOMPARE(view.snapshot().audio_dropped, quint64(0));
        if (audible)
        {
            QVERIFY(!lost_audio_while_active);
            QVERIFY(!view.audible());
            QCOMPARE(SDL_GetQueuedAudioSize(audio_id), Uint32(0));
            QCOMPARE(SDL_GetAudioDeviceStatus(audio_id), SDL_AUDIO_PAUSED);
            QVERIFY(output_tone.aligned && output_tone.frames != 0);
            QCOMPARE(output_tone.nonempty_clears, quint64(0));
            QCOMPARE(output_tone.clear_observed_frames, quint64(0));
            for (int channel = 0; channel < 2; ++channel)
            {
                // 独立8秒参考含自己的滤波尾；拼接及EOF相位误差预设每份2周期，8/12份最多16/24周期。
                const auto expected_cycles = static_cast<qint64>(reference_tone.cycles[channel] * repeat_count);
                const auto actual_cycles = static_cast<qint64>(output_tone.cycles[channel]);
                QVERIFY2(std::abs(actual_cycles - expected_cycles) <= 2 * repeat_count,
                         "累计排入PCM的正向过零与独立解码参考不符；并非逐样本内容等价检查");
                QVERIFY(output_tone.rms(channel) >= reference_tone.rms(channel) * 0.8);
                QVERIFY(output_tone.rms(channel) <= reference_tone.rms(channel) * 1.2);
                qInfo() << "scale_pcm_complete channel=" << channel << "actual_output_frames=" << output_tone.frames
                        << "reference_input_frames=" << reference_tone.frames * repeat_count << "cycles=" << actual_cycles
                        << "reference_cycles=" << expected_cycles << "rms=" << output_tone.rms(channel)
                        << "reference_rms=" << reference_tone.rms(channel) << "; 仅提交PCM及dummy排空，不是声学验收";
            }
        }
        else
        {
            QCOMPARE(output_tone.frames, quint64(0));
        }
        QTRY_COMPARE_WITH_TIMEOUT(painted.hashes.size(), expected.size(), 1000);
        QCOMPARE(painted.hashes, expected);
        for (qsizetype index = 0; index < painted.indices.size(); ++index)
        {
            QCOMPARE(painted.indices[index], quint64(index + 1));
        }
        QVERIFY(!dispatch_lateness.isEmpty());
        std::sort(dispatch_lateness.begin(), dispatch_lateness.end());
        const auto p95_lateness = dispatch_lateness[dispatch_lateness.size() * 95 / 100];
        qInfo() << "scale_input_complete source_bytes=" << source.size()
                << "sha256=" << QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex() << "dispatch_p95_ms=" << p95_lateness
                << "dispatch_max_ms=" << dispatch_lateness.back() << "elapsed_ms=" << elapsed.elapsed() << "audible(dummy-only)=" << audible
                << "; 非物理屏幕/声音验收";
        QVERIFY2(dispatch_lateness.back() <= 100, "发送事件循环迟到超过100ms；夹具不满足预设实时输入条件");
        // 首个固定窗口在18秒前完成收敛检查；不是把首帧约5秒后的缓存当作稳态。
        assert_window(samples, 16000, 18000, actual_rate, audible, reference_tone);
        assert_window(samples, 18000, 22000, actual_rate, audible, reference_tone);
        const qint64 recovery_begin_ms = pause_after_scale ? 28000 : 34000;
        const qint64 recovery_end_ms = pause_after_scale ? 32000 : 38000;
        const double recovery_rate = pause_after_scale ? actual_rate : 1.0;
        assert_window(samples, recovery_begin_ms, recovery_end_ms, recovery_rate, audible, reference_tone);
        // 均值达标不能掩盖先突发后停顿；仅0.25倍用2秒子窗：25fps的一秒6/7帧量化可偏12%，两秒12/13帧仅偏4%。
        // 其他行保留逐秒检查，所有固定窗口及子窗仍为相同的±10%阈值。
        const qint64 subwindow_ms = actual_numerator == 1 && actual_denominator == 4 ? 2000 : 1000;
        for (qint64 begin_ms = 16000; begin_ms < 22000; begin_ms += subwindow_ms)
        {
            assert_window(samples, begin_ms, begin_ms + subwindow_ms, actual_rate, audible, reference_tone);
        }
        for (qint64 begin_ms = recovery_begin_ms; begin_ms < recovery_end_ms; begin_ms += 1000)
        {
            assert_window(samples, begin_ms, begin_ms + 1000, recovery_rate, audible, reference_tone);
        }
    }

   private:
    struct pacing_sample
    {
        qint64 wall_ms;
        qint64 dispatch_source_us;
        qint64 painted_source_us;
        quint64 audio_cursor;
        double audio_phase[2];
        Uint32 queued_audio_bytes;
        SDL_AudioStatus audio_status;
    };

    void assert_window(
        const QList<pacing_sample>& samples, qint64 begin_ms, qint64 end_ms, double expected_rate, bool audible, const pcm_statistics& reference_tone)
    {
        const auto first = std::find_if(samples.cbegin(), samples.cend(), [=](const auto& value) { return value.wall_ms >= begin_ms; });
        const auto last = std::find_if(samples.cbegin(), samples.cend(), [=](const auto& value) { return value.wall_ms >= end_ms; });
        QVERIFY(first != samples.cend() && last != samples.cend());
        QVERIFY(first->wall_ms - begin_ms <= 150 && last->wall_ms - end_ms <= 150);
        const double seconds = static_cast<double>(last->wall_ms - first->wall_ms) / 1000;
        const double input_rate = static_cast<double>(last->dispatch_source_us - first->dispatch_source_us) / 1000000 / seconds;
        const double paint_rate = static_cast<double>(last->painted_source_us - first->painted_source_us) / 1000000 / seconds;
        qInfo() << "scale_fixed_window begin_ms=" << first->wall_ms << "end_ms=" << last->wall_ms << "target=" << expected_rate
                << "dispatch_rate=" << input_rate << "paint_rate=" << paint_rate;
        QVERIFY(input_rate >= expected_rate * 0.9 && input_rate <= expected_rate * 1.1);
        QVERIFY2(paint_rate >= expected_rate * 0.9 && paint_rate <= expected_rate * 1.1, "固定窗口实际paint源跨度未达到预设倍率±10%；不得放宽阈值");
        if (audible)
        {
            QVERIFY(last->audio_cursor > first->audio_cursor);
            const double output_seconds = static_cast<double>(last->audio_cursor - first->audio_cursor) / 48000;
            qsizetype empty_samples = 0;
            qsizetype empty_run = 0;
            qsizetype longest_empty_run = 0;
            for (auto current = first; current <= last; ++current)
            {
                QCOMPARE(current->audio_status, SDL_AUDIO_PLAYING);
                if (current->queued_audio_bytes == 0)
                {
                    ++empty_samples;
                    longest_empty_run = std::max(longest_empty_run, ++empty_run);
                }
                else
                {
                    empty_run = 0;
                }
            }
            qInfo() << "scale_audio_queue_window begin_ms=" << first->wall_ms << "end_ms=" << last->wall_ms << "zero_queue_samples=" << empty_samples
                    << "longest_zero_queue_samples=" << longest_empty_run << "output_cursor_seconds=" << output_seconds << "wall_seconds=" << seconds
                    << "; queue为0不等于后端静音，此处仅100ms采样监控";
            QVERIFY2(output_seconds >= seconds * 0.9 && output_seconds <= seconds * 1.1, "固定窗口估算输出样本游标推进速率偏离48kHz±10%");
            for (int channel = 0; channel < 2; ++channel)
            {
                const double reference_hz = static_cast<double>(reference_tone.cycles[channel]) * 48000 / reference_tone.frames;
                const double actual_hz = (last->audio_phase[channel] - first->audio_phase[channel]) / output_seconds;
                const double target_hz = reference_hz * expected_rate;
                qInfo() << "scale_audio_tone_window channel=" << channel << "begin_ms=" << first->wall_ms << "end_ms=" << last->wall_ms
                        << "estimated_pcm_hz=" << actual_hz << "target_hz=" << target_hz << "reference_hz=" << reference_hz;
                QVERIFY2(actual_hz >= target_hz * 0.9 && actual_hz <= target_hz * 1.1,
                         "实际转换后PCM音调未达到fixture实际倍率±10%；不是requested推算");
            }
        }
    }

    void decode_reference(const QString& path, QList<QByteArray>& hashes, qsizetype& pcm, pcm_statistics& tone)
    {
        decode_thread reference(QUrl::fromLocalFile(path), QSize(320, 240), playback_mode::finite);
        const auto stop_reference = qScopeGuard(
            [&]()
            {
                reference.requestInterruption();
                if (!reference.wait(4000))
                {
                    qFatal("倍速参考解码线程没有退出");
                }
            });
        const auto consume = [&]()
        {
            for (const auto& frame : reference.take_video_frames())
            {
                QCOMPARE(frame.pts_us, static_cast<qint64>(hashes.size()) * 40000);
                const auto& image = frame.image;
                hashes.append(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
                                                       QCryptographicHash::Sha256));
            }
            for (const auto& block : reference.take_audio_blocks())
            {
                pcm += block.pcm.size();
                tone.append(block.pcm.constData(), static_cast<Uint32>(block.pcm.size()));
            }
        };
        QTimer consumer;
        connect(&consumer, &QTimer::timeout, this, consume);
        consumer.start(1);
        reference.start();
        QTRY_VERIFY_WITH_TIMEOUT(reference.isFinished(), 4000);
        consumer.stop();
        consume();
        QVERIFY2(reference.snapshot().error.isEmpty(), qPrintable(reference.snapshot().error));
    }

   private:
    bool had_audio_driver_{};
    QByteArray old_audio_driver_;
};

}    // namespace media_server

QTEST_MAIN(media_server::scale_playback_view_test)
#include "scale_playback_view_test.moc"
