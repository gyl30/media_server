#include <utility>

#include "media/rtsp/rtsp_server.h"
#include "media/net/tcp_connection.h"
#include "media/rtsp/rtsp_input_session.h"
#include "media/rtsp/rtsp_output_session.h"
#include "media/rtsp/rtsp_server_connection.h"

namespace media_server
{

rtsp_server::rtsp_server(io_context_pool& workers, const config& config)
    : config_(config), listener_(std::make_shared<tcp_listener>(workers, config.rtsp_port))
{
}

boost::system::error_code rtsp_server::startup()
{
    const std::weak_ptr<rtsp_server> weak = shared_from_this();
    return listener_->startup(
        [weak](boost::asio::ip::tcp::socket socket)
        {
            if (const auto self = weak.lock())
            {
                self->on_accept(std::move(socket));
            }
        });
}

void rtsp_server::on_accept(boost::asio::ip::tcp::socket socket)
{
    auto tcp = std::make_shared<tcp_connection>(std::move(socket));
    auto connection = std::make_shared<rtsp_server_connection>(std::move(tcp));

    const std::weak_ptr<rtsp_server> weak_owner = shared_from_this();
    const std::weak_ptr<rtsp_server_connection> weak_connection = connection;
    auto handler = std::make_shared<rtsp_server_connection_handler>();
    handler->on_read = [weak_connection](std::span<const std::uint8_t> data)
    {
        const auto current = weak_connection.lock();
        return current ? current->input(data) : data.size();
    };
    handler->on_describe = [weak_owner, weak_connection](rtsp_server_t* server, const char* uri)
    {
        const auto owner = weak_owner.lock();
        const auto current = weak_connection.lock();
        if (!owner || !current)
        {
            return -1;
        }
        auto session = std::make_shared<rtsp_output_session>(current, owner->config_.rtsp_video);
        current->set_handler(session->make_handler());
        return session->on_describe(server, uri != nullptr ? uri : "");
    };
    handler->on_setup =
        [weak_owner, weak_connection](
            rtsp_server_t* server, const char* uri, const char* session, const rtsp_header_transport_t transports[], std::size_t count)
    {
        const auto owner = weak_owner.lock();
        const auto current = weak_connection.lock();
        if (!owner || !current)
        {
            return -1;
        }
        auto output = std::make_shared<rtsp_output_session>(current, owner->config_.rtsp_video);
        current->set_handler(output->make_handler());
        return output->on_setup(server, uri != nullptr ? uri : "", session != nullptr ? session : "", transports, count);
    };
    handler->on_announce = [weak_owner, weak_connection](rtsp_server_t* server, const char* uri, const char* sdp, int length)
    {
        const auto owner = weak_owner.lock();
        const auto current = weak_connection.lock();
        if (!owner || !current)
        {
            return -1;
        }
        auto input = std::make_shared<rtsp_input_session>(current);
        auto input_handler = std::make_shared<rtsp_server_connection_handler>();
        input_handler->on_read = [input](std::span<const std::uint8_t> data) { return input->on_read(data); };
        input_handler->on_shutdown = [input]() { input->safe_shutdown(); };
        input_handler->on_setup = [input](rtsp_server_t* handler_server,
                                          const char* handler_uri,
                                          const char* handler_session,
                                          const rtsp_header_transport_t handler_transports[],
                                          std::size_t handler_count)
        { return input->on_setup(handler_server, handler_uri, handler_session, handler_transports, handler_count); };
        input_handler->on_teardown = [input](rtsp_server_t* handler_server, const char*, const char* handler_session)
        { return input->on_teardown(handler_server, handler_session); };
        input_handler->on_announce = [input](rtsp_server_t* handler_server, const char* handler_uri, const char* handler_sdp, int handler_length)
        { return input->on_announce(handler_server, handler_uri, handler_sdp, handler_length); };
        input_handler->on_record =
            [input](rtsp_server_t* handler_server, const char*, const char* handler_session, const std::int64_t*, const double*)
        { return input->on_record(handler_server, handler_session); };
        input_handler->on_get_parameter = [](rtsp_server_t* handler_server, const char*, const char*, const void*, int)
        { return rtsp_server_reply_get_parameter(handler_server, 200, nullptr, 0); };
        current->set_handler(std::move(input_handler));
        return input->on_announce(server, uri != nullptr ? uri : "", sdp, length);
    };
    handler->on_play = [](rtsp_server_t*, const char*, const char*, const std::int64_t*, const double*) { return -1; };
    handler->on_teardown = [](rtsp_server_t*, const char*, const char*) { return -1; };
    handler->on_record = [](rtsp_server_t*, const char*, const char*, const std::int64_t*, const double*) { return -1; };
    handler->on_get_parameter = [](rtsp_server_t* server, const char*, const char* session, const void*, int bytes)
    { return bytes == 0 && (session == nullptr || session[0] == '\0') ? rtsp_server_reply_get_parameter(server, 200, nullptr, 0) : -1; };

    if (!connection->startup(std::move(handler)))
    {
        connection->shutdown();
    }
}

}    // namespace media_server
