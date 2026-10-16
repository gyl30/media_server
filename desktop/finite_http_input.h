#ifndef DESKTOP_FINITE_HTTP_INPUT_H
#define DESKTOP_FINITE_HTTP_INPUT_H

#include <cstdint>

#include <QUrl>
#include <QThread>
#include <QByteArray>
#include <QSslSocket>

#include <boost/beast/http.hpp>

extern "C"
{
#include <libavformat/avio.h>
}

namespace media_server
{

// 有限录像 HTTP chunked 输入：Qt socket 提供 TCP/TLS，Beast 解析分帧；只有解析完整结束才返回 EOF。
// 全部调用在所属工作线程内进行，作为 FFmpeg 自定义 AVIO 的读取端。
struct finite_http_input
{
    QSslSocket socket;
    boost::beast::http::response_parser<boost::beast::http::buffer_body> parser;
    QByteArray wire;
    QThread& thread;
    AVIOInterruptCB interrupt;

    finite_http_input(QThread& owner, AVIOInterruptCB io_interrupt);
    ~finite_http_input();

    int context_error() const;
    int wait();
    int open(const QUrl& url);
    static int read(void* opaque, uint8_t* buffer, int size);
};

}    // namespace media_server

#endif
