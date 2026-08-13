#ifndef MEDIA_WEBRTC_WEBRTC_SDP_H
#define MEDIA_WEBRTC_WEBRTC_SDP_H

#include "media/core/media_types.h"

#include <boost/asio/ip/address.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media_server
{

struct webrtc_codec_offer
{
    int payload_type{};
    std::string encoding_name;
    std::uint32_t clock_rate{};
    std::uint16_t channel_count{};
    std::string format_parameters;
};

struct webrtc_media_offer
{
    std::string type;
    int port{};
    std::string protocol;
    std::string mid;
    std::string direction;
    std::string setup;
    std::string ice_ufrag;
    std::string ice_pwd;
    std::string fingerprint;
    bool rtcp_mux{};
    bool bundle_only{};
    std::vector<int> payload_types;
    std::vector<webrtc_codec_offer> codecs;
};

struct webrtc_offer
{
    std::vector<std::string> bundle_mids;
    std::vector<webrtc_media_offer> media;
};

struct webrtc_answer
{
    std::string sdp;
    std::string transport_mid;
    std::optional<codec_id> video_codec;
    std::optional<int> video_payload_type;
    std::optional<int> audio_payload_type;
    std::optional<int> audio_channel_count;
    std::optional<int> audio_bitrate;
    std::optional<int> audio_max_playback_rate;
};

struct webrtc_answer_config
{
    boost::asio::ip::address address;
    std::uint16_t port{};
    std::string ice_ufrag;
    std::string ice_pwd;
    std::string fingerprint;
};

[[nodiscard]] std::optional<webrtc_offer> parse_webrtc_offer(std::string_view sdp);
[[nodiscard]] std::optional<webrtc_answer> make_webrtc_answer(const webrtc_offer& offer,
                                                              const std::vector<media_track>& tracks,
                                                              const webrtc_answer_config& config);

}    // namespace media_server

#endif
