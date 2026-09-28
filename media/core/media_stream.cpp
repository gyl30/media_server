#include <utility>

#include "media/core/media_stream.h"
#include "media/ps/mpeg_ps_output.h"

namespace media_server
{
media_stream::media_stream(std::string name, worker_context& worker) : media_history(std::move(name), worker) {}

worker_context& media_stream::worker() const noexcept { return worker_; }

bool media_stream::set_tracks(std::vector<media_track> tracks)
{
    return media_history::set_tracks(std::move(tracks));
}

void media_stream::publish(media_frame frame)
{
    if (ended_ || !frame.payload || frame.payload->empty() || !tracks_.contains(frame.track))
    {
        return;
    }
    media_history::publish(frame);
}

void media_stream::end()
{
    if (ended_)
    {
        return;
    }
    media_history::end();
}

std::shared_ptr<mpeg_ps_output> media_stream::ps_output()
{
    if (ended_)
    {
        return {};
    }
    if (const auto output = ps_output_.lock())
    {
        return output;
    }
    auto output = std::make_shared<mpeg_ps_output>(name_, worker_);
    if (!output->startup(std::static_pointer_cast<media_stream>(shared_from_this()), tracks()))
    {
        return output;
    }
    ps_output_ = output;
    return output;
}

}    // namespace media_server
