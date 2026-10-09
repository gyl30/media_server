#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "media/core/session_registry.h"

namespace
{
using namespace media_server;

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class fake_session final : public session
{
   public:
    void shutdown() override { ++shutdowns; }

    int shutdowns{};
};
}    // namespace

int main()
{
    try
    {
        auto& registry = session_registry::instance();
        const auto first = std::make_shared<fake_session>();
        require(registry.add_receiver_session("gb/a/b", "id-1", first), "add failed");
        require(!registry.add_receiver_session("gb/a/b", "id-2", std::make_shared<fake_session>()), "duplicate name accepted");
        int updates{};
        require(registry.update_if_open("gb/a/b", *first, [&updates]() { ++updates; }) && updates == 1, "fresh receiver not updated");

        int closed{};
        // 身份不匹配：不关闭、不登记等待者。
        require(registry.begin_close_receiver("gb/a/b", "id-other", [&closed](receiver_close reason) { closed += reason == receiver_close::completed ? 1 : 100; }) == nullptr, "stale identity closed receiver");
        require(registry.update_if_open("gb/a/b", *first, [&updates]() { ++updates; }) && updates == 2, "stale close changed receiver");

        // 关闭开始后槽位保留，名称仍被占用，直到会话清理完成并自行移除。
        require(registry.begin_close_receiver("gb/a/b", "id-1", [&closed](receiver_close reason) { closed += reason == receiver_close::completed ? 1 : 100; }) == first, "close did not return session");
        // 进入关闭后更新与关闭在同一把锁下判定，不再执行更新。
        require(!registry.update_if_open("gb/a/b", *first, [&updates]() { ++updates; }) && updates == 2, "closing receiver updated");
        require(!registry.add_receiver_session("gb/a/b", "id-2", std::make_shared<fake_session>()), "closing slot released early");
        // 并发关闭共享同一次完成。
        require(registry.begin_close_receiver("gb/a/b", "id-1", [&closed](receiver_close reason) { closed += reason == receiver_close::completed ? 1 : 100; }) == first, "concurrent close rejected");
        require(closed == 0, "close completed before cleanup");

        // 其他会话不能移除不属于自己的槽位。
        const fake_session stranger;
        registry.remove_receiver_session("gb/a/b", stranger);
        require(closed == 0, "foreign removal completed close");
        registry.remove_receiver_session("gb/a/b", *first);
        require(closed == 2, "cleanup did not complete all closes");
        require(registry.begin_close_receiver("gb/a/b", "id-1", [&closed](receiver_close reason) { closed += reason == receiver_close::completed ? 1 : 100; }) == nullptr, "removed receiver closed again");
        require(registry.add_receiver_session("gb/a/b", "id-2", std::make_shared<fake_session>()), "slot not reusable after cleanup");

        // 服务停止：摘除全部槽位，等待中的关闭直接完成。
        const auto pending = std::make_shared<fake_session>();
        require(registry.add_receiver_session("gb/c/d", "id-3", pending), "add pending failed");
        std::optional<receiver_close> stopped;
        require(registry.begin_close_receiver("gb/c/d", "id-3", [&stopped](receiver_close reason) { stopped = reason; }) == pending,
                "pending close failed");
        registry.shutdown_all();
        require(stopped == receiver_close::stopped && pending->shutdowns == 1, "shutdown_all did not end close as stopped");
        std::cout << "session registry: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
