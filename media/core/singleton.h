#ifndef MEDIA_CORE_SINGLETON_H
#define MEDIA_CORE_SINGLETON_H

#include <exception>
#include <memory>
#include <utility>

namespace media_server
{
template <typename T>
class singleton
{
   public:
    singleton() = delete;
    ~singleton() = delete;

    template <typename... Args>
    static void init(Args&&... args)
    {
        auto& value = storage();
        if (value)
        {
            std::terminate();
        }
        value.reset(new T(std::forward<Args>(args)...));
    }

    static T& instance()
    {
        auto& value = storage();
        if (!value)
        {
            std::terminate();
        }
        return *value;
    }

    static void destroy() noexcept
    {
        storage().reset();
    }

   private:
    static std::unique_ptr<T>& storage()
    {
        static std::unique_ptr<T> value;
        return value;
    }
};

}    // namespace media_server

#endif
