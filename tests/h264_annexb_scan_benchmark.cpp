#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>

extern "C"
{
#include "mpeg4-avc.h"
}

namespace
{

struct nalu_range
{
    std::size_t offset;
    std::size_t size;
    std::uint8_t type;

    bool operator==(const nalu_range&) const = default;
};

struct reference_capture
{
    const std::uint8_t* begin{};
    std::vector<nalu_range> ranges;
};

struct benchmark_input
{
    std::string_view name;
    std::vector<std::uint8_t> bytes;
};

volatile std::uint64_t benchmark_sink{};

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

std::size_t find_start_code(std::span<const std::uint8_t> bytes, std::size_t begin)
{
    for (std::size_t index = begin + 2U; index + 1U < bytes.size(); ++index)
    {
        if (bytes[index - 2U] == 0U && bytes[index - 1U] == 0U && bytes[index] == 1U)
        {
            return index + 1U;
        }
    }
    return bytes.size();
}

void parse_annexb_into(std::span<const std::uint8_t> bytes, std::vector<nalu_range>& ranges)
{
    ranges.clear();
    ranges.reserve(16U);
    auto payload_begin = find_start_code(bytes, 0U);
    while (payload_begin < bytes.size())
    {
        const auto next_payload_begin = find_start_code(bytes, payload_begin);
        auto payload_end = next_payload_begin < bytes.size() ? next_payload_begin - 3U : bytes.size();
        while (payload_end > payload_begin && bytes[payload_end - 1U] == 0U)
        {
            --payload_end;
        }
        if (payload_end > payload_begin)
        {
            ranges.push_back({payload_begin, payload_end - payload_begin, static_cast<std::uint8_t>(bytes[payload_begin] & 0x1fU)});
        }
        payload_begin = next_payload_begin;
    }
}

std::vector<nalu_range> parse_annexb(std::span<const std::uint8_t> bytes)
{
    std::vector<nalu_range> ranges;
    parse_annexb_into(bytes, ranges);
    return ranges;
}

void capture_reference(void* opaque, const std::uint8_t* nalu, std::size_t bytes)
{
    auto& capture = *static_cast<reference_capture*>(opaque);
    capture.ranges.push_back({static_cast<std::size_t>(nalu - capture.begin), bytes, static_cast<std::uint8_t>(nalu[0] & 0x1fU)});
}

std::vector<nalu_range> reference_ranges(const std::vector<std::uint8_t>& bytes)
{
    reference_capture capture{bytes.data(), {}};
    require(mpeg4_h264_annexb_nalu(bytes.data(), bytes.size(), &capture_reference, &capture) == 0, "reference parser result");
    return capture.ranges;
}

void append_start_code(std::vector<std::uint8_t>& bytes, std::size_t size)
{
    require(size == 3U || size == 4U, "start code size");
    bytes.insert(bytes.end(), size - 1U, 0U);
    bytes.push_back(1U);
}

void append_nalu(std::vector<std::uint8_t>& bytes, std::size_t start_code_size, std::initializer_list<std::uint8_t> payload)
{
    append_start_code(bytes, start_code_size);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

void append_nalu(std::vector<std::uint8_t>& bytes, std::size_t start_code_size, std::span<const std::uint8_t> payload)
{
    append_start_code(bytes, start_code_size);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> make_payload(std::uint8_t type, std::size_t size, std::uint8_t fill)
{
    require(size > 0U, "payload size");
    std::vector<std::uint8_t> payload(size, fill);
    payload.front() = type;
    return payload;
}

std::vector<benchmark_input> make_inputs()
{
    std::vector<benchmark_input> inputs;

    inputs.push_back({"empty", {}});
    inputs.push_back({"no-start-code", {0x67U, 0x42U, 0x00U, 0x1eU}});

    std::vector<std::uint8_t> trailing_start_code;
    append_nalu(trailing_start_code, 3U, {0x67U, 0x42U, 0x00U, 0x1eU});
    append_start_code(trailing_start_code, 4U);
    inputs.push_back({"trailing-start-code", std::move(trailing_start_code)});

    std::vector<std::uint8_t> consecutive;
    append_nalu(consecutive, 3U, {0x67U, 0x42U, 0x00U, 0x1eU});
    append_start_code(consecutive, 4U);
    append_start_code(consecutive, 3U);
    append_nalu(consecutive, 4U, {0x68U, 0xceU, 0x06U, 0xe2U});
    inputs.push_back({"consecutive-start-codes", std::move(consecutive)});

    std::vector<std::uint8_t> mixed;
    append_nalu(mixed, 4U, {0x09U, 0xf0U});
    append_nalu(mixed, 3U, {0x67U, 0x42U, 0x00U, 0x1eU});
    append_nalu(mixed, 4U, {0x68U, 0xceU, 0x06U, 0xe2U});
    append_nalu(mixed, 3U, {0x06U, 0x05U, 0xffU, 0xffU});
    append_nalu(mixed, 4U, {0x65U, 0x88U, 0x84U, 0x00U, 0x01U});
    append_nalu(mixed, 3U, {0x41U, 0x9aU, 0x22U, 0x11U});
    inputs.push_back({"mixed-start-codes", std::move(mixed)});

    std::vector<std::uint8_t> inter;
    append_nalu(inter, 4U, {0x09U, 0xf0U});
    const auto inter_slice = make_payload(0x41U, 1'024U, 0x55U);
    append_nalu(inter, 3U, inter_slice);
    inputs.push_back({"inter", std::move(inter)});

    std::vector<std::uint8_t> key;
    append_nalu(key, 4U, {0x09U, 0xf0U});
    append_nalu(key, 3U, {0x67U, 0x42U, 0x00U, 0x1eU, 0xe9U, 0x01U, 0x40U});
    append_nalu(key, 4U, {0x68U, 0xceU, 0x06U, 0xe2U});
    const auto sei = make_payload(0x06U, 4'096U, 0x33U);
    append_nalu(key, 3U, sei);
    const auto idr = make_payload(0x65U, 128U * 1024U, 0x55U);
    append_nalu(key, 4U, idr);
    inputs.push_back({"key", std::move(key)});

    return inputs;
}

void check_correctness(const benchmark_input& input)
{
    const auto expected = reference_ranges(input.bytes);
    const auto actual = parse_annexb(input.bytes);
    require(actual == expected, std::string(input.name) + " boundary comparison");
    for (const auto& range : actual)
    {
        require(range.offset + range.size <= input.bytes.size(), std::string(input.name) + " range bounds");
        require(range.size > 0U, std::string(input.name) + " non-empty NAL");
    }
    std::cout << "[pass] annexb " << input.name << " nalu_count=" << actual.size() << '\n';
}

void mix_range_checksum(std::uint64_t& checksum, std::span<const nalu_range> ranges)
{
    for (const auto& range : ranges)
    {
        checksum ^= static_cast<std::uint64_t>(range.offset + 0x9e3779b9U);
        checksum = (checksum << 7U) | (checksum >> 57U);
        checksum ^= static_cast<std::uint64_t>(range.size);
        checksum ^= static_cast<std::uint64_t>(range.type);
    }
}

std::uint64_t range_checksum(std::span<const nalu_range> ranges, std::size_t consumers)
{
    std::uint64_t checksum = 0;
    for (std::size_t consumer = 0; consumer < consumers; ++consumer)
    {
        mix_range_checksum(checksum, ranges);
    }
    return checksum;
}

std::uint64_t repeated_scan_checksum(const std::vector<std::uint8_t>& bytes, std::size_t consumers, reference_capture& capture)
{
    std::uint64_t checksum = 0;
    for (std::size_t consumer = 0; consumer < consumers; ++consumer)
    {
        capture.ranges.clear();
        require(mpeg4_h264_annexb_nalu(bytes.data(), bytes.size(), &capture_reference, &capture) == 0, "benchmark reference parser result");
        mix_range_checksum(checksum, capture.ranges);
    }
    return checksum;
}

std::uint64_t parse_once_checksum(const std::vector<std::uint8_t>& bytes, std::size_t consumers, std::vector<nalu_range>& ranges)
{
    parse_annexb_into(bytes, ranges);
    return range_checksum(ranges, consumers);
}

std::uint64_t elapsed_ns(const std::vector<std::uint8_t>& bytes, std::size_t consumers, bool repeated_scan, std::size_t repetitions)
{
    reference_capture capture{bytes.data(), {}};
    capture.ranges.reserve(16U);
    std::vector<nalu_range> ranges;
    ranges.reserve(16U);
    const auto begin = std::chrono::steady_clock::now();
    std::uint64_t checksum = 0;
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
    {
        checksum ^= repeated_scan ? repeated_scan_checksum(bytes, consumers, capture) : parse_once_checksum(bytes, consumers, ranges);
    }
    benchmark_sink = checksum;
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / repetitions;
}

std::uint64_t median(std::vector<std::uint64_t> values)
{
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

void run_benchmark(const benchmark_input& input)
{
    const auto ranges = parse_annexb(input.bytes);
    const std::size_t repetitions = input.bytes.size() < 4'096U ? 200U : 10U;
    std::cout << "\nAU class=" << input.name << " bytes=" << input.bytes.size() << " nalus=" << ranges.size() << '\n';
    std::cout << "consumers\trepeated_scan_ns_per_au\tparse_once_ns_per_au\tratio\n";
    for (const auto consumers : {1U, 4U, 8U, 16U, 32U})
    {
        reference_capture capture{input.bytes.data(), {}};
        capture.ranges.reserve(16U);
        require(repeated_scan_checksum(input.bytes, consumers, capture) == range_checksum(ranges, consumers),
                std::string(input.name) + " checksum comparison");
        for (std::size_t warmup = 0; warmup < 3U; ++warmup)
        {
            elapsed_ns(input.bytes, consumers, true, repetitions);
            elapsed_ns(input.bytes, consumers, false, repetitions);
        }

        std::vector<std::uint64_t> repeated_samples;
        std::vector<std::uint64_t> parse_once_samples;
        repeated_samples.reserve(10U);
        parse_once_samples.reserve(10U);
        for (std::size_t sample = 0; sample < 10U; ++sample)
        {
            repeated_samples.push_back(elapsed_ns(input.bytes, consumers, true, repetitions));
            parse_once_samples.push_back(elapsed_ns(input.bytes, consumers, false, repetitions));
        }
        const auto repeated_median = median(repeated_samples);
        const auto parse_once_median = median(parse_once_samples);
        std::cout << consumers << '\t' << *std::min_element(repeated_samples.begin(), repeated_samples.end()) << '/' << repeated_median << '/'
                  << *std::max_element(repeated_samples.begin(), repeated_samples.end()) << '\t'
                  << *std::min_element(parse_once_samples.begin(), parse_once_samples.end()) << '/' << parse_once_median << '/'
                  << *std::max_element(parse_once_samples.begin(), parse_once_samples.end()) << '\t'
                  << static_cast<double>(repeated_median) / static_cast<double>(parse_once_median) << '\n';
    }
}

void run_correctness()
{
    for (const auto& input : make_inputs())
    {
        check_correctness(input);
    }
}

}    // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2 || (std::string_view(argv[1]) != "--correctness" && std::string_view(argv[1]) != "--benchmark"))
        {
            std::cerr << "usage: h264_annexb_scan_benchmark --correctness|--benchmark\n";
            return 2;
        }

        run_correctness();
        if (std::string_view(argv[1]) == "--benchmark")
        {
            for (const auto& input : make_inputs())
            {
                if (input.name == "inter" || input.name == "key")
                {
                    run_benchmark(input);
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
