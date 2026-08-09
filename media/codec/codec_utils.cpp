#include "media/codec/codec_utils.h"

extern "C"
{
#include "mpeg4-aac.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
}

#include <array>

namespace media_server
{
namespace
{

constexpr std::int64_t ns_per_second = 1'000'000'000;
constexpr std::int64_t ns_per_millisecond = 1'000'000;

}    // namespace

std::vector<std::uint8_t> h264_avcc_to_annex_b(std::span<const std::uint8_t> avcc)
{
    mpeg4_avc_t configuration{};
    if (mpeg4_avc_decoder_configuration_record_load(
            avcc.data(), avcc.size(), &configuration) <= 0)
            {
        return {};
    }

    std::vector<std::uint8_t> result(sizeof(configuration.data) + 4096U);
    const auto bytes = mpeg4_avc_to_nalu(&configuration, result.data(), result.size());
    if (bytes <= 0)
    {
        return {};
    }
    result.resize(static_cast<std::size_t>(bytes));
    return result;
}

std::vector<std::uint8_t> h264_annex_b_to_avcc(std::span<const std::uint8_t> annex_b)
{
    mpeg4_avc_t configuration{};
    if (mpeg4_avc_from_nalu(annex_b.data(), annex_b.size(), &configuration) < 0 ||
        configuration.nb_sps == 0 || configuration.nb_pps == 0)
        {
        return {};
    }

    std::vector<std::uint8_t> result(sizeof(configuration.data) + 256U);
    const auto bytes = mpeg4_avc_decoder_configuration_record_save(
        &configuration, result.data(), result.size());
    if (bytes <= 0)
    {
        return {};
    }
    result.resize(static_cast<std::size_t>(bytes));
    return result;
}

std::vector<std::uint8_t> h265_hvcc_to_annex_b(std::span<const std::uint8_t> hvcc)
{
    mpeg4_hevc_t configuration{};
    if (mpeg4_hevc_decoder_configuration_record_load(
            hvcc.data(), hvcc.size(), &configuration) <= 0)
    {
        return {};
    }

    std::vector<std::uint8_t> result(sizeof(configuration.data) + 4096U);
    const auto bytes = mpeg4_hevc_to_nalu(&configuration, result.data(), result.size());
    if (bytes <= 0)
    {
        return {};
    }
    result.resize(static_cast<std::size_t>(bytes));
    return result;
}

std::vector<std::uint8_t> h265_annex_b_to_hvcc(std::span<const std::uint8_t> annex_b)
{
    mpeg4_hevc_t configuration{};
    if (mpeg4_hevc_from_nalu(annex_b.data(), annex_b.size(), &configuration) < 0 ||
        configuration.numOfArrays < 3)
    {
        return {};
    }

    std::vector<std::uint8_t> result(sizeof(configuration.data) + 256U);
    const auto bytes = mpeg4_hevc_decoder_configuration_record_save(
        &configuration, result.data(), result.size());
    if (bytes <= 0)
    {
        return {};
    }
    result.resize(static_cast<std::size_t>(bytes));
    return result;
}

std::optional<aac_config> parse_aac_asc(std::span<const std::uint8_t> asc)
{
    mpeg4_aac_t configuration{};
    if (mpeg4_aac_audio_specific_config_load(
            asc.data(), asc.size(), &configuration) < 0)
            {
        return std::nullopt;
    }

    const auto channels = mpeg4_aac_channel_count(configuration.channel_configuration);
    if (configuration.sampling_frequency == 0 || channels == 0)
    {
        return std::nullopt;
    }

    return aac_config{
        .sample_rate = configuration.sampling_frequency,
        .channel_count = channels,
    };
}

std::optional<aac_config> parse_aac_adts(std::span<const std::uint8_t> adts)
{
    mpeg4_aac_t configuration{};
    if (mpeg4_aac_adts_load(adts.data(), adts.size(), &configuration) < 0)
    {
        return std::nullopt;
    }

    const auto channels = mpeg4_aac_channel_count(configuration.channel_configuration);
    if (configuration.sampling_frequency == 0 || channels == 0)
    {
        return std::nullopt;
    }

    return aac_config{
        .sample_rate = configuration.sampling_frequency,
        .channel_count = channels,
    };
}

std::vector<std::uint8_t> make_adts_frame(
    std::span<const std::uint8_t> asc,
    std::span<const std::uint8_t> raw_aac)
    {

    mpeg4_aac_t configuration{};
    if (mpeg4_aac_audio_specific_config_load(
            asc.data(), asc.size(), &configuration) < 0)
            {
        return {};
    }

    std::array<std::uint8_t, 16> header{};
    const auto header_bytes = mpeg4_aac_adts_save(
        &configuration, raw_aac.size(), header.data(), header.size());
    if (header_bytes <= 0)
    {
        return {};
    }

    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(header_bytes) + raw_aac.size());
    result.insert(result.end(), header.begin(), header.begin() + header_bytes);
    result.insert(result.end(), raw_aac.begin(), raw_aac.end());
    return result;
}

std::int64_t milliseconds_to_ns(std::int64_t value) noexcept
{
    return value * ns_per_millisecond;
}

std::int64_t ns_to_milliseconds(std::int64_t value) noexcept
{
    return value / ns_per_millisecond;
}

std::uint32_t ns_to_flv_milliseconds(std::int64_t value) noexcept
{
    return static_cast<std::uint32_t>(ns_to_milliseconds(value));
}

std::int64_t ns_to_90khz(std::int64_t value) noexcept
{
    const auto seconds = value / ns_per_second;
    const auto nanoseconds = value % ns_per_second;
    return seconds * 90'000 + nanoseconds * 90'000 / ns_per_second;
}

}    // namespace media_server
