#ifndef MEDIA_CORE_SCOPE_EXIT_H
#define MEDIA_CORE_SCOPE_EXIT_H

#include <utility>

namespace media_server
{

template <typename function_type>
class scope_exit
{
   public:
    explicit scope_exit(function_type function) : function_(std::move(function)) {}

    ~scope_exit()
    {
        if (active_)
        {
            function_();
        }
    }

    scope_exit(const scope_exit&) = delete;
    scope_exit& operator=(const scope_exit&) = delete;

    void release() noexcept { active_ = false; }

   private:
    function_type function_;
    bool active_ = true;
};

}    // namespace media_server

#endif
