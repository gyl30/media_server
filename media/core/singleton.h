#ifndef MEDIA_CORE_SINGLETON_H
#define MEDIA_CORE_SINGLETON_H

#include <exception>
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
    static T& init(Args&&... args)
    {
        if (value_ != nullptr)
        {
            std::terminate();
        }
        value_ = new T(std::forward<Args>(args)...);
        return *value_;
    }

    static T& instance()
    {
        if (value_ == nullptr)
        {
            std::terminate();
        }
        return *value_;
    }

    static void destroy() { delete std::exchange(value_, nullptr); }

   private:
    static T* value_;
};

template <typename T>
T* singleton<T>::value_ = nullptr;

}    // namespace media_server

#endif
