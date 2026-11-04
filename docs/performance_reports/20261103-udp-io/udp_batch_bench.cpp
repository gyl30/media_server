#include <poll.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <linux/perf_event.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

namespace
{

enum class send_mode
{
    sendto,
    connected,
    sendmmsg,
    gso,
};

struct options
{
    send_mode mode{send_mode::sendto};
    int size{1200};
    int last_size{0};
    int burst{4};
    int sockets{256};
    int endpoints_per_socket{1};
    int rounds_per_second{30};
    int seconds{10};
    int sender_cpu{2};
    int receiver_cpu{4};
    int receivers{2};
    bool copy{false};
};

struct header
{
    std::uint32_t endpoint;
    std::uint32_t size;
    std::uint64_t sequence;
    std::int64_t produced_ns;
};

struct endpoint_state
{
    std::uint64_t next_sequence{};
    std::uint64_t received{};
    std::uint64_t bytes{};
    std::uint64_t reordered{};
    std::uint64_t corrupt{};
};

std::int64_t now_ns()
{
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000LL + value.tv_nsec;
}

int open_counter(std::uint64_t config, bool exclude_kernel)
{
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = exclude_kernel ? 1 : 0;
    attr.exclude_hv = 1;
    return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
}

std::uint64_t read_counter(int fd)
{
    std::uint64_t value{};
    if (fd < 0 || read(fd, &value, sizeof(value)) != sizeof(value))
    {
        return 0;
    }
    return value;
}

void pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

std::uint8_t pattern(std::uint64_t sequence, std::size_t index) { return static_cast<std::uint8_t>((sequence * 131U + index * 7U) & 0xFFU); }

void fill(std::uint8_t* data, int size, std::uint32_t endpoint, std::uint64_t sequence)
{
    header value{endpoint, static_cast<std::uint32_t>(size), sequence, now_ns()};
    std::memcpy(data, &value, sizeof(value));
    for (int index = static_cast<int>(sizeof(header)); index < size; index += 61)
    {
        data[index] = pattern(sequence, static_cast<std::size_t>(index));
    }
}

bool parse(int argc, char** argv, options& config)
{
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string key = argv[index];
        const std::string value = argv[index + 1];
        if (key == "--mode")
        {
            if (value == "sendto")
            {
                config.mode = send_mode::sendto;
            }
            else if (value == "connected")
            {
                config.mode = send_mode::connected;
            }
            else if (value == "sendmmsg")
            {
                config.mode = send_mode::sendmmsg;
            }
            else if (value == "gso")
            {
                config.mode = send_mode::gso;
            }
            else
            {
                return false;
            }
        }
        else if (key == "--size")
        {
            config.size = std::stoi(value);
        }
        else if (key == "--last-size")
        {
            config.last_size = std::stoi(value);
        }
        else if (key == "--burst")
        {
            config.burst = std::stoi(value);
        }
        else if (key == "--sockets")
        {
            config.sockets = std::stoi(value);
        }
        else if (key == "--endpoints-per-socket")
        {
            config.endpoints_per_socket = std::stoi(value);
        }
        else if (key == "--rounds-per-second")
        {
            config.rounds_per_second = std::stoi(value);
        }
        else if (key == "--seconds")
        {
            config.seconds = std::stoi(value);
        }
        else if (key == "--sender-cpu")
        {
            config.sender_cpu = std::stoi(value);
        }
        else if (key == "--receiver-cpu")
        {
            config.receiver_cpu = std::stoi(value);
        }
        else if (key == "--receivers")
        {
            config.receivers = std::stoi(value);
        }
        else if (key == "--copy")
        {
            config.copy = value == "1";
        }
        else
        {
            return false;
        }
    }
    if (config.size < static_cast<int>(sizeof(header)) || config.size > 1472 || config.burst < 1 || config.burst > 64 || config.sockets < 1 ||
        config.endpoints_per_socket < 1 || config.receivers < 1)
    {
        return false;
    }
    if (config.last_size == 0)
    {
        config.last_size = config.size;
    }
    if (config.last_size < static_cast<int>(sizeof(header)) || config.last_size > config.size)
    {
        return false;
    }
    if (config.mode == send_mode::gso && config.endpoints_per_socket != 1)
    {
        return false;
    }
    if (config.mode == send_mode::connected && config.endpoints_per_socket != 1)
    {
        return false;
    }
    return true;
}

sockaddr_in loopback(std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

}    // namespace

int main(int argc, char** argv)
{
    options config;
    if (!parse(argc, argv, config))
    {
        std::fprintf(stderr,
                     "usage: udp_batch_bench --mode sendto|connected|sendmmsg|gso --size N [--last-size N] --burst K --sockets S "
                     "[--endpoints-per-socket E] [--rounds-per-second R] [--seconds T] [--sender-cpu C] [--receiver-cpu C] "
                     "[--receivers N] [--copy 0|1]\n");
        return 2;
    }

    const int endpoints = config.sockets * config.endpoints_per_socket;
    std::vector<int> receive_sockets(static_cast<std::size_t>(endpoints));
    std::vector<sockaddr_in> receive_addresses(static_cast<std::size_t>(endpoints));
    for (int index = 0; index < endpoints; ++index)
    {
        const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        int buffer = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        auto address = loopback(0);
        if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            std::perror("receiver bind");
            return 1;
        }
        socklen_t length = sizeof(address);
        getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
        receive_sockets[static_cast<std::size_t>(index)] = fd;
        receive_addresses[static_cast<std::size_t>(index)] = address;
    }

    std::vector<int> send_sockets(static_cast<std::size_t>(config.sockets));
    for (int index = 0; index < config.sockets; ++index)
    {
        const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        auto address = loopback(0);
        if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            std::perror("sender bind");
            return 1;
        }
        if (config.mode == send_mode::connected)
        {
            const auto& target = receive_addresses[static_cast<std::size_t>(index)];
            if (connect(fd, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0)
            {
                std::perror("connect");
                return 1;
            }
        }
        send_sockets[static_cast<std::size_t>(index)] = fd;
    }

    std::vector<endpoint_state> states(static_cast<std::size_t>(endpoints));
    std::atomic<bool> stop{false};
    std::vector<std::vector<std::int64_t>> latencies(static_cast<std::size_t>(config.receivers));
    std::vector<std::uint64_t> wrong_size(static_cast<std::size_t>(config.receivers));
    std::vector<std::thread> receivers;
    for (int receiver = 0; receiver < config.receivers; ++receiver)
    {
        receivers.emplace_back(
            [&, receiver]()
            {
                pin(config.receiver_cpu + receiver);
                const int epoll = epoll_create1(0);
                for (int index = receiver; index < endpoints; index += config.receivers)
                {
                    epoll_event event{};
                    event.events = EPOLLIN;
                    event.data.u32 = static_cast<std::uint32_t>(index);
                    epoll_ctl(epoll, EPOLL_CTL_ADD, receive_sockets[static_cast<std::size_t>(index)], &event);
                }
                constexpr int batch = 64;
                std::vector<std::uint8_t> storage(batch * 2048);
                std::vector<iovec> vectors(batch);
                std::vector<mmsghdr> messages(batch);
                auto& latency = latencies[static_cast<std::size_t>(receiver)];
                std::uint64_t counter{};
                std::vector<epoll_event> events(256);
                while (!stop.load(std::memory_order_relaxed))
                {
                    const int ready = epoll_wait(epoll, events.data(), static_cast<int>(events.size()), 50);
                    for (int item = 0; item < ready; ++item)
                    {
                        const auto index = events[static_cast<std::size_t>(item)].data.u32;
                        for (;;)
                        {
                            for (int slot = 0; slot < batch; ++slot)
                            {
                                vectors[static_cast<std::size_t>(slot)] = {storage.data() + slot * 2048, 2048};
                                messages[static_cast<std::size_t>(slot)] = {};
                                messages[static_cast<std::size_t>(slot)].msg_hdr.msg_iov = &vectors[static_cast<std::size_t>(slot)];
                                messages[static_cast<std::size_t>(slot)].msg_hdr.msg_iovlen = 1;
                            }
                            const int count = recvmmsg(receive_sockets[index], messages.data(), batch, MSG_DONTWAIT, nullptr);
                            if (count <= 0)
                            {
                                break;
                            }
                            const auto received_at = now_ns();
                            for (int slot = 0; slot < count; ++slot)
                            {
                                const auto* data = storage.data() + slot * 2048;
                                const auto bytes = messages[static_cast<std::size_t>(slot)].msg_len;
                                header value{};
                                std::memcpy(&value, data, sizeof(value));
                                auto& state = states[index];
                                if (value.endpoint != index || value.size != bytes)
                                {
                                    ++wrong_size[static_cast<std::size_t>(receiver)];
                                    continue;
                                }
                                bool good = true;
                                for (std::size_t offset = sizeof(header); offset < bytes; offset += 61)
                                {
                                    good = good && data[offset] == pattern(value.sequence, offset);
                                }
                                state.corrupt += good ? 0 : 1;
                                state.reordered += value.sequence < state.next_sequence ? 1 : 0;
                                state.next_sequence = value.sequence + 1;
                                ++state.received;
                                state.bytes += bytes;
                                if ((counter++ & 15U) == 0U)
                                {
                                    latency.push_back(received_at - value.produced_ns);
                                }
                            }
                        }
                    }
                }
                close(epoll);
            });
    }

    pin(config.sender_cpu);
    const int cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES, false);
    const int instructions_fd = open_counter(PERF_COUNT_HW_INSTRUCTIONS, false);
    const int user_cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES, true);

    const int burst = config.burst;
    const auto slots = static_cast<std::size_t>(burst * config.endpoints_per_socket);
    std::vector<std::vector<std::uint8_t>> packets(slots, std::vector<std::uint8_t>(1500));
    std::vector<std::uint8_t> staging(slots * 1500U);
    std::vector<iovec> vectors(static_cast<std::size_t>(burst * config.endpoints_per_socket));
    std::vector<mmsghdr> messages(vectors.size());
    std::vector<std::uint64_t> sequences(static_cast<std::size_t>(endpoints));
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(std::uint16_t))]{};

    std::uint64_t sent{};
    std::uint64_t calls{};
    std::uint64_t again{};
    std::uint64_t short_sends{};
    std::uint64_t errors{};
    std::vector<std::uint64_t> batch_histogram(65);

    const auto packet_size = [&](int position) { return position == burst - 1 ? config.last_size : config.size; };
    const auto wait_writable = [&](int fd)
    {
        ++again;
        pollfd item{fd, POLLOUT, 0};
        poll(&item, 1, 100);
    };

    for (const int fd : {cycles_fd, instructions_fd, user_cycles_fd})
    {
        if (fd >= 0)
        {
            ioctl(fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        }
    }
    timespec cpu_start{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start);
    const auto start = now_ns();
    const auto round_ns = 1'000'000'000LL / config.rounds_per_second;
    const auto total_rounds = static_cast<std::int64_t>(config.rounds_per_second) * config.seconds;
    for (std::int64_t round = 0; round < total_rounds; ++round)
    {
        const auto deadline = start + round * round_ns;
        while (now_ns() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        for (int socket_index = 0; socket_index < config.sockets; ++socket_index)
        {
            const int fd = send_sockets[static_cast<std::size_t>(socket_index)];
            int message_count = 0;
            for (int endpoint_offset = 0; endpoint_offset < config.endpoints_per_socket; ++endpoint_offset)
            {
                const int endpoint = socket_index * config.endpoints_per_socket + endpoint_offset;
                auto& target = receive_addresses[static_cast<std::size_t>(endpoint)];
                for (int position = 0; position < burst; ++position)
                {
                    const int size = packet_size(position);
                    const auto slot = static_cast<std::size_t>(endpoint_offset * burst + position);
                    auto* data = packets[slot].data();
                    fill(data, size, static_cast<std::uint32_t>(endpoint), sequences[static_cast<std::size_t>(endpoint)]++);
                    if (config.copy)
                    {
                        std::memcpy(staging.data() + slot * 1500U, data, static_cast<std::size_t>(size));
                        data = staging.data() + slot * 1500U;
                    }
                    if (config.mode == send_mode::sendto || config.mode == send_mode::connected)
                    {
                        for (;;)
                        {
                            ++calls;
                            const auto result =
                                config.mode == send_mode::connected
                                    ? send(fd, data, static_cast<std::size_t>(size), 0)
                                    : sendto(fd, data, static_cast<std::size_t>(size), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
                            if (result >= 0)
                            {
                                ++sent;
                                ++batch_histogram[1];
                                break;
                            }
                            if (errno != EAGAIN)
                            {
                                ++errors;
                                break;
                            }
                            wait_writable(fd);
                        }
                        continue;
                    }
                    auto& vector = vectors[static_cast<std::size_t>(message_count)];
                    vector = {data, static_cast<std::size_t>(size)};
                    if (config.mode == send_mode::sendmmsg)
                    {
                        auto& message = messages[static_cast<std::size_t>(message_count)];
                        message = {};
                        message.msg_hdr.msg_name = &target;
                        message.msg_hdr.msg_namelen = sizeof(target);
                        message.msg_hdr.msg_iov = &vector;
                        message.msg_hdr.msg_iovlen = 1;
                    }
                    ++message_count;
                }
            }
            if (config.mode == send_mode::sendmmsg)
            {
                int offset = 0;
                while (offset < message_count)
                {
                    ++calls;
                    const int result = sendmmsg(fd, messages.data() + offset, static_cast<unsigned int>(message_count - offset), 0);
                    if (result > 0)
                    {
                        ++batch_histogram[static_cast<std::size_t>(std::min(result, 64))];
                        short_sends += result < message_count - offset ? 1 : 0;
                        sent += static_cast<std::uint64_t>(result);
                        offset += result;
                        continue;
                    }
                    if (errno != EAGAIN)
                    {
                        ++errors;
                        break;
                    }
                    wait_writable(fd);
                }
            }
            if (config.mode == send_mode::gso)
            {
                msghdr message{};
                auto& target = receive_addresses[static_cast<std::size_t>(socket_index)];
                message.msg_name = &target;
                message.msg_namelen = sizeof(target);
                message.msg_iov = vectors.data();
                message.msg_iovlen = static_cast<std::size_t>(message_count);
                if (message_count > 1)
                {
                    message.msg_control = control;
                    message.msg_controllen = sizeof(control);
                    auto* header_item = CMSG_FIRSTHDR(&message);
                    header_item->cmsg_level = SOL_UDP;
                    header_item->cmsg_type = UDP_SEGMENT;
                    header_item->cmsg_len = CMSG_LEN(sizeof(std::uint16_t));
                    const auto segment = static_cast<std::uint16_t>(config.size);
                    std::memcpy(CMSG_DATA(header_item), &segment, sizeof(segment));
                }
                for (;;)
                {
                    ++calls;
                    const auto result = sendmsg(fd, &message, 0);
                    if (result >= 0)
                    {
                        sent += static_cast<std::uint64_t>(message_count);
                        ++batch_histogram[static_cast<std::size_t>(message_count)];
                        break;
                    }
                    if (errno != EAGAIN)
                    {
                        ++errors;
                        break;
                    }
                    wait_writable(fd);
                }
            }
        }
    }
    timespec cpu_end{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
    const auto elapsed_ns = now_ns() - start;
    const auto cycles = read_counter(cycles_fd);
    const auto instructions = read_counter(instructions_fd);
    const auto user_cycles = read_counter(user_cycles_fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    for (auto& thread : receivers)
    {
        thread.join();
    }

    std::uint64_t received{};
    std::uint64_t bytes{};
    std::uint64_t reordered{};
    std::uint64_t corrupt{};
    for (const auto& state : states)
    {
        received += state.received;
        bytes += state.bytes;
        reordered += state.reordered;
        corrupt += state.corrupt;
    }
    std::uint64_t mismatched{};
    std::vector<std::int64_t> all_latency;
    for (int receiver = 0; receiver < config.receivers; ++receiver)
    {
        mismatched += wrong_size[static_cast<std::size_t>(receiver)];
        all_latency.insert(
            all_latency.end(), latencies[static_cast<std::size_t>(receiver)].begin(), latencies[static_cast<std::size_t>(receiver)].end());
    }
    std::sort(all_latency.begin(), all_latency.end());
    const auto percentile = [&](double value) -> double
    {
        return all_latency.empty()
                   ? 0.0
                   : static_cast<double>(all_latency[static_cast<std::size_t>(value * static_cast<double>(all_latency.size() - 1))]) / 1000.0;
    };
    const double cpu_seconds =
        static_cast<double>(cpu_end.tv_sec - cpu_start.tv_sec) + static_cast<double>(cpu_end.tv_nsec - cpu_start.tv_nsec) / 1e9;
    std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(total_rounds) * static_cast<std::uint64_t>(endpoints) *
        (static_cast<std::uint64_t>(burst - 1) * static_cast<std::uint64_t>(config.size) + static_cast<std::uint64_t>(config.last_size));
    std::string histogram;
    for (std::size_t size = 1; size < batch_histogram.size(); ++size)
    {
        if (batch_histogram[size] != 0)
        {
            histogram += (histogram.empty() ? "" : ",") + std::to_string(size) + ":" + std::to_string(batch_histogram[size]);
        }
    }
    const char* names[] = {"sendto", "connected", "sendmmsg", "gso"};
    std::printf(
        "{\"mode\": \"%s\", \"size\": %d, \"last_size\": %d, \"burst\": %d, \"sockets\": %d, \"endpoints_per_socket\": %d, \"copy\": %d, "
        "\"rounds\": %lld, \"elapsed_s\": %.3f, \"sent\": %llu, \"calls\": %llu, \"again\": %llu, \"short_sends\": %llu, \"errors\": %llu, "
        "\"batches\": \"%s\", \"received\": %llu, \"received_bytes\": %llu, \"expected_bytes\": %llu, \"reordered\": %llu, \"corrupt\": %llu, "
        "\"mismatched\": %llu, \"cpu_s\": %.4f, \"cycles_per_packet\": %.0f, \"user_cycles_per_packet\": %.0f, "
        "\"instructions_per_packet\": %.0f, \"latency_us_p50\": %.1f, \"latency_us_p99\": %.1f, \"latency_us_max\": %.1f}\n",
        names[static_cast<int>(config.mode)],
        config.size,
        config.last_size,
        burst,
        config.sockets,
        config.endpoints_per_socket,
        config.copy ? 1 : 0,
        static_cast<long long>(total_rounds),
        static_cast<double>(elapsed_ns) / 1e9,
        static_cast<unsigned long long>(sent),
        static_cast<unsigned long long>(calls),
        static_cast<unsigned long long>(again),
        static_cast<unsigned long long>(short_sends),
        static_cast<unsigned long long>(errors),
        histogram.c_str(),
        static_cast<unsigned long long>(received),
        static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(expected_bytes),
        static_cast<unsigned long long>(reordered),
        static_cast<unsigned long long>(corrupt),
        static_cast<unsigned long long>(mismatched),
        cpu_seconds,
        static_cast<double>(cycles) / static_cast<double>(std::max<std::uint64_t>(sent, 1)),
        static_cast<double>(user_cycles) / static_cast<double>(std::max<std::uint64_t>(sent, 1)),
        static_cast<double>(instructions) / static_cast<double>(std::max<std::uint64_t>(sent, 1)),
        percentile(0.5),
        percentile(0.99),
        percentile(1.0));
    return 0;
}
