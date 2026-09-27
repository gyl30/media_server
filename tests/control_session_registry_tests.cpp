#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "media/control/control_session_registry.h"

namespace
{

class probe_session final : public media_server::control_session
{
   public:
    probe_session(std::string id, int& shutdowns) : id_(std::move(id)), shutdowns_(shutdowns) {}

    void shutdown() override { ++shutdowns_; }
    [[nodiscard]] std::string_view stream_id() const noexcept override { return id_; }

   private:
    std::string id_;
    int& shutdowns_;
};

void require(bool condition)
{
    if (!condition)
    {
        throw std::runtime_error("control session registry lifecycle");
    }
}

}    // namespace

int main()
{
    auto& registry = media_server::control_session_registry::instance();
    int receiver_shutdowns = 0;
    int sender_shutdowns = 0;
    auto receiver = std::make_shared<probe_session>("receiver", receiver_shutdowns);
    auto sender = std::make_shared<probe_session>("sender", sender_shutdowns);
    require(registry.add_receiver_session("live/camera", receiver));
    require(registry.add_sender_session("live/camera", "sender", sender));

    registry.shutdown_all();
    require(receiver_shutdowns == 1 && sender_shutdowns == 1);
    require(!registry.take_receiver_session_as<probe_session>("live/camera"));
    require(!registry.take_sender_session("live/camera", "sender"));
    require(!registry.add_receiver_session("live/camera", receiver));
    require(!registry.add_sender_session("live/camera", "sender", sender));

    registry.shutdown_all();
    require(receiver_shutdowns == 1 && sender_shutdowns == 1);
}
