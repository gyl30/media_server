#ifndef DESKTOP_FINITE_HTTP_INPUT_H
#define DESKTOP_FINITE_HTTP_INPUT_H

#include <cstdint>

#include <QUrl>
#include <QThread>
#include <QByteArray>
#include <QSslSocket>
#include <QDeadlineTimer>

#include <boost/beast/http.hpp>

extern "C"
{
#include <libavformat/avio.h>
}

namespace media_server
{

struct finite_http_input
{
    QSslSocket socket;
    boost::beast::http::response_parser<boost::beast::http::buffer_body> parser;
    QByteArray wire;
    QThread& thread;
    AVIOInterruptCB interrupt;
    QDeadlineTimer deadline;

    finite_http_input(QThread& owner, AVIOInterruptCB io_interrupt, QDeadlineTimer transport_deadline);
    ~finite_http_input();

    int context_error() const;
    int wait();
    int open(const QUrl& url);
    static int read(void* opaque, uint8_t* buffer, int size);
};

}    // namespace media_server

#endif
