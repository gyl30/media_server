#ifndef MEDIA_GB28181_GB28181_SESSION_H
#define MEDIA_GB28181_GB28181_SESSION_H

namespace media_server
{

class gb28181_session
{
   public:
    virtual ~gb28181_session() = default;

    virtual void shutdown() = 0;
};

}    // namespace media_server

#endif
