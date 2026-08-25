#ifndef MEDIA_CORE_SINGLETON_H
#define MEDIA_CORE_SINGLETON_H

namespace media_server
{
template <typename T>
class singleton
{
   public:
    singleton() = delete;
    ~singleton() = delete;

    static T& instance()
    {
        static T value;
        return value;
    }
};

}    // namespace media_server

#endif
