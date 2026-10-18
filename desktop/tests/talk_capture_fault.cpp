#include <chrono>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

namespace
{

// 仅链接到故障探针；真实SDL设备与线程仍运行，不改变产品采集实现。
struct capture_fault
{
    SDL_AudioCallback callback{};
    void* userdata{};
    bool no_first{};
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_sample{};
};

capture_fault fault;

void SDLCALL capture(void* context, Uint8* bytes, int length)
{
    auto& state = *static_cast<capture_fault*>(context);
    const auto now = std::chrono::steady_clock::now();
    if (state.started == std::chrono::steady_clock::time_point{})
    {
        state.started = now;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.started).count();
    if (!state.no_first && (elapsed < 2000 || elapsed >= 5500))
    {
        state.callback(state.userdata, bytes, length);
        return;
    }
    // 每100ms只给一个真实样本：保持3秒输入活性，但不足以持续生成20ms包。
    // 中途缺供3.5秒，可耗尽生产最多200ms的PCM队列，并在观察结束前恢复。
    if (length >= 2 && (state.last_sample == std::chrono::steady_clock::time_point{} || now - state.last_sample >= std::chrono::milliseconds(100)))
    {
        state.last_sample = now;
        state.callback(state.userdata, bytes, 2);
    }
}

}    // namespace

extern "C" SDL_AudioDeviceID SDLCALL
__real_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes);

extern "C" SDL_AudioDeviceID SDLCALL
__wrap_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes)
{
    const char* mode = std::getenv("QT_TEST_CAPTURE_FAULT");
    if (!iscapture || !desired || !desired->callback || !mode || (std::strcmp(mode, "no-first") != 0 && std::strcmp(mode, "mid-stall") != 0))
    {
        return __real_SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
    }
    fault = {};
    fault.callback = desired->callback;
    fault.userdata = desired->userdata;
    fault.no_first = std::strcmp(mode, "no-first") == 0;
    auto spec = *desired;
    spec.callback = capture;
    spec.userdata = &fault;
    return __real_SDL_OpenAudioDevice(device, iscapture, &spec, obtained, allowed_changes);
}
