#include <cerrno>
#include <sstream>

#include <QTimer>
#include <QEventLoop>

#include <boost/asio/buffer.hpp>

#include "desktop/finite_http_input.h"

extern "C"
{
#include <libavutil/error.h>
}

namespace media_server
{

finite_http_input::finite_http_input(QThread& owner, AVIOInterruptCB io_interrupt) : thread(owner), interrupt(io_interrupt)
{
    socket.setReadBufferSize(65536);
    parser.header_limit(65536);
    parser.body_limit(boost::none);
    parser.eager(true);
}

finite_http_input::~finite_http_input() { socket.abort(); }

// 与 FFmpeg 共用同一中断回调，期限按所属线程的进展时基判断。
int finite_http_input::context_error() const
{
    return thread.isInterruptionRequested() ? AVERROR_EXIT : interrupt.callback(interrupt.opaque) ? AVERROR(ETIMEDOUT) : 0;
}

int finite_http_input::wait()
{
    const int error = context_error();
    if (error < 0)
    {
        return error;
    }
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QIODevice::readyRead, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QAbstractSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QAbstractSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QAbstractSocket::bytesWritten, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QAbstractSocket::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QSslSocket::encrypted, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QSslSocket::sslErrors, &loop, &QEventLoop::quit);
    timer.start(50);
    loop.exec();
    return context_error();
}

int finite_http_input::open(const QUrl& url)
{
    if (const int error = context_error(); error < 0)
    {
        return error;
    }
    const bool encrypted = url.scheme() == QStringLiteral("https");
    if (encrypted)
    {
        socket.connectToHostEncrypted(url.host(), static_cast<quint16>(url.port(443)));
    }
    else
    {
        socket.connectToHost(url.host(), static_cast<quint16>(url.port(80)));
    }
    while (socket.state() != QAbstractSocket::ConnectedState || (encrypted && !socket.isEncrypted()))
    {
        if (socket.state() == QAbstractSocket::UnconnectedState)
        {
            return AVERROR(EIO);
        }
        if (const int error = wait(); error < 0)
        {
            return error;
        }
    }
    auto target = url.path(QUrl::FullyEncoded).toUtf8();
    if (target.isEmpty())
    {
        target = "/";
    }
    if (url.hasQuery())
    {
        target += '?' + url.query(QUrl::FullyEncoded).toUtf8();
    }
    auto host = url.host().contains(':') ? '[' + url.host().toUtf8() + ']' : QUrl::toAce(url.host());
    if (url.port() >= 0)
    {
        host += ':' + QByteArray::number(url.port());
    }
    boost::beast::http::request<boost::beast::http::empty_body> request(boost::beast::http::verb::get, target.toStdString(), 11);
    request.set(boost::beast::http::field::host, host.toStdString());
    request.keep_alive(false);
    std::ostringstream encoded;
    encoded << request;
    const auto bytes = QByteArray::fromStdString(encoded.str());
    if (socket.write(bytes) != bytes.size())
    {
        return AVERROR(EIO);
    }
    while (socket.bytesToWrite() > 0)
    {
        if (socket.state() != QAbstractSocket::ConnectedState)
        {
            return AVERROR(EIO);
        }
        if (const int error = wait(); error < 0)
        {
            return error;
        }
    }
    return 0;
}

int finite_http_input::read(void* opaque, uint8_t* buffer, int size)
{
    auto& input = *static_cast<finite_http_input*>(opaque);
    while (true)
    {
        if (const int error = input.context_error(); error < 0)
        {
            return error;
        }
        if (input.socket.error() != QAbstractSocket::UnknownSocketError && input.socket.error() != QAbstractSocket::RemoteHostClosedError)
        {
            return AVERROR(EIO);
        }
        if (input.parser.is_done())
        {
            return AVERROR_EOF;
        }
        if (!input.wire.isEmpty())
        {
            auto& body = input.parser.get().body();
            body.data = buffer;
            body.size = static_cast<std::size_t>(size);
            boost::system::error_code error;
            const auto consumed = input.parser.put(boost::asio::buffer(input.wire.constData(), static_cast<std::size_t>(input.wire.size())), error);
            input.wire.remove(0, static_cast<qsizetype>(consumed));
            if (error && error != boost::beast::http::error::need_more && error != boost::beast::http::error::need_buffer)
            {
                return AVERROR(EIO);
            }
            if (input.parser.is_header_done())
            {
                const auto type = input.parser.get()[boost::beast::http::field::content_type];
                if (input.parser.get().result_int() != 200 || !input.parser.chunked() ||
                    QString::fromLatin1(type.data(), static_cast<qsizetype>(type.size()))
                            .section(';', 0, 0)
                            .trimmed()
                            .compare(QStringLiteral("video/mpeg"), Qt::CaseInsensitive) != 0)
                {
                    return AVERROR(EIO);
                }
            }
            const auto produced = static_cast<std::size_t>(size) - body.size;
            if (produced > 0)
            {
                return static_cast<int>(produced);
            }
        }
        if (input.parser.is_done())
        {
            return AVERROR_EOF;
        }
        constexpr qsizetype capacity = 65536;
        if (input.wire.size() == capacity)
        {
            return AVERROR(EIO);
        }
        if (input.socket.bytesAvailable() > 0)
        {
            input.wire += input.socket.read(capacity - input.wire.size());
            continue;
        }
        if (input.socket.state() == QAbstractSocket::UnconnectedState)
        {
            // Beast 要求 put_eof 前已收到响应字节；未响应即断开本就不是完整传输。
            if (!input.parser.got_some())
            {
                return AVERROR(EIO);
            }
            boost::system::error_code error;
            input.parser.put_eof(error);
            return !error && input.parser.is_done() ? AVERROR_EOF : AVERROR(EIO);
        }
        if (const int error = input.wait(); error < 0)
        {
            return error;
        }
    }
}

}    // namespace media_server
