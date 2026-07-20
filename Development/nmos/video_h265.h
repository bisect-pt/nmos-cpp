#ifndef NMOS_VIDEO_H265_H
#define NMOS_VIDEO_H265_H

#include <utility>
#include "nmos/media_type.h"
#include "nmos/node_resources.h"
#include "nmos/sdp_utils.h"
#include "nmos/video_jxsv.h"

// H.265 / HEVC, per AMWA BCP-006-03
// See https://specs.amwa.tv/bcp-006-03/branches/v1.0-dev/docs/NMOS_With_H.265.html
//
// This header provides parsing of SDP transport files and validation against IS-04 Receiver
// Capabilities. SDP transport files are produced by the application, not by nmos-cpp, so there
// are deliberately no make_*_sdp_parameters functions here.

namespace sdp
{
    namespace video_h265
    {
        // hexadecimal strings, ".."-separated groups of two hex digits, per RFC 7798,
        // e.g. "B0.00.00.00.00.00"; used for profile-compatibility-indicator (4 bytes)
        // and interop-constraints (6 bytes)
        utility::string_t make_hex_string(uint64_t value, size_t bytes);
        uint64_t parse_hex_string(const utility::string_t& value);

        namespace fields
        {
            // See https://www.iana.org/assignments/media-types/video/H265
            // and https://tools.ietf.org/html/rfc7798#section-7.1

            // the profile-tier-level, as decimal integers
            const web::json::field_with_default<uint32_t> profile_space{ U("profile-space"), 0 };
            const web::json::field_with_default<uint32_t> profile_id{ U("profile-id"), 1 }; // 1 == Main
            const web::json::field_with_default<uint32_t> tier_flag{ U("tier-flag"), 0 }; // 0 == Main tier
            const web::json::field_with_default<uint32_t> level_id{ U("level-id"), 93 }; // 93 == level 3.1, i.e. 30 * 3.1

            // profile-compatibility-indicator (32 bits) and interop-constraints (48 bits),
            // as hexadecimal strings, groups of two hex digits separated by ".."
            const web::json::field_as_string_or profile_compatibility_indicator{ U("profile-compatibility-indicator"), U("") };
            const web::json::field_as_string_or interop_constraints{ U("interop-constraints"), U("") };

            // out-of-band parameter sets, as comma-separated base64 NAL units
            // NB: a *trailing* comma indicates a terminating empty NAL unit, which per BCP-006-03
            // signals that the parameter sets are also sent in-band, i.e. in_and_out_of_band
            const web::json::field_as_string_or sprop_vps{ U("sprop-vps"), U("") };
            const web::json::field_as_string_or sprop_sps{ U("sprop-sps"), U("") };
            const web::json::field_as_string_or sprop_pps{ U("sprop-pps"), U("") };

            // sprop-max-don-diff greater than zero indicates the interleaved packetization mode
            const web::json::field_with_default<uint32_t> sprop_max_don_diff{ U("sprop-max-don-diff"), 0 };
            const web::json::field_with_default<uint32_t> sprop_depack_buf_nalus{ U("sprop-depack-buf-nalus"), 0 };
            const web::json::field_with_default<uint32_t> sprop_depack_buf_bytes{ U("sprop-depack-buf-bytes"), 0 };

            // BCP-006-03 requires Single RTP stream transport (SRST); parse it in order to reject
            // a Sender using MRST or MRMT
            const web::json::field_as_string_or tx_mode{ U("tx-mode"), U("SRST") };
        }

        // Single RTP stream transport is the only mode permitted by BCP-006-03
        namespace tx_modes
        {
            const utility::string_t SRST{ U("SRST") };
        }

        // The profile-tier-level, i.e. the six fmtp parameters that together encode the Flow's
        // profile and level attributes
        // See Rec. ITU-T H.265 Annex A and https://tools.ietf.org/html/rfc7798#section-7.1
        struct profile_tier_level
        {
            uint32_t profile_space; // general_profile_space, 0..3
            uint32_t profile_id; // general_profile_idc
            uint32_t profile_compatibility_indicator; // general_profile_compatibility_flag[0..31]
            uint64_t interop_constraints; // 48 bits: general_progressive_source_flag,
                                          // general_interlaced_source_flag,
                                          // general_non_packed_constraint_flag,
                                          // general_frame_only_constraint_flag, and the 44 reserved
                                          // bits carrying the RExt / HT / SCC general constraint flags
            uint32_t tier_flag; // general_tier_flag, 0 == Main tier, 1 == High tier
            uint32_t level_id; // general_level_idc == 30 * level

            profile_tier_level()
                : profile_space(0)
                , profile_id(1)
                , profile_compatibility_indicator(0)
                , interop_constraints(0)
                , tier_flag(0)
                , level_id(93)
            {}

            profile_tier_level(uint32_t profile_space, uint32_t profile_id, uint32_t profile_compatibility_indicator, uint64_t interop_constraints, uint32_t tier_flag, uint32_t level_id)
                : profile_space(profile_space)
                , profile_id(profile_id)
                , profile_compatibility_indicator(profile_compatibility_indicator)
                , interop_constraints(interop_constraints)
                , tier_flag(tier_flag)
                , level_id(level_id)
            {}
        };
    }
}

namespace nmos
{
    namespace media_types
    {
        // H.265 / HEVC
        // See https://www.iana.org/assignments/media-types/video/H265
        // and https://tools.ietf.org/html/rfc7798
        const media_type video_H265{ U("video/H265") };
    }

    // NB: the bit_rate, profile, level and packet_transmission_mode *keys* are shared with the
    // other coded video formats and are declared in nmos/video_codec.h. The keys below are used
    // by H.264 and H.265 but not by JPEG XS; if BCP-006-02 (H.264) is added later, move them into
    // nmos/video_codec.h.

    namespace fields
    {
        // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/flow-attributes/#constant-bit-rate
        const web::json::field_as_bool_or constant_bit_rate{ U("constant_bit_rate"), false };

        // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/sender-attributes/#parameter-sets-flow-mode
        const web::json::field_as_string_or parameter_sets_flow_mode{ U("parameter_sets_flow_mode"), U("dynamic") };

        // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/sender-attributes/#parameter-sets-transport-mode
        const web::json::field_as_string_or parameter_sets_transport_mode{ U("parameter_sets_transport_mode"), U("in_band") };
    }

    namespace caps
    {
        namespace format
        {
            // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/capabilities/#constant-bit-rate
            const web::json::field_as_value_or constant_bit_rate{ U("urn:x-nmos:cap:format:constant_bit_rate"), {} }; // boolean
        }

        namespace transport
        {
            // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/capabilities/#parameter-sets-flow-mode
            const web::json::field_as_value_or parameter_sets_flow_mode{ U("urn:x-nmos:cap:transport:parameter_sets_flow_mode"), {} }; // string

            // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/capabilities/#parameter-sets-transport-mode
            const web::json::field_as_value_or parameter_sets_transport_mode{ U("urn:x-nmos:cap:transport:parameter_sets_transport_mode"), {} }; // string
        }
    }

    // The H.265 Profile, Level, Packet Transmission Mode, Parameter Sets Flow Mode and Parameter
    // Sets Transport Mode types and their permitted values. These are distinct types from e.g.
    // nmos::video_jxsv::profile, so that a value from one codec cannot be used with another.
    namespace video_h265
    {
        // Profile
        // "The H.265 profile in use. Any white space Unicode character in the profile name SHALL be omitted."
        // See Rec. ITU-T H.265 Annex A.3
        // and https://specs.amwa.tv/nmos-parameter-registers/branches/main/flow-attributes/#profile
        DEFINE_STRING_ENUM(profile)
        namespace profiles
        {
            // Main, Main 10 and Main Still Picture profiles (H.265 A.3.2, A.3.3, A.3.4)
            const profile Main{ U("Main") }; // default
            const profile Main10{ U("Main10") };
            const profile MainStillPicture{ U("MainStillPicture") };

            // Format range extensions profiles (H.265 A.3.5)
            const profile Monochrome{ U("Monochrome") };
            const profile Monochrome10{ U("Monochrome10") };
            const profile Monochrome12{ U("Monochrome12") };
            const profile Monochrome16{ U("Monochrome16") };
            const profile Main12{ U("Main12") };
            const profile Main10_422{ U("Main10-422") };
            const profile Main12_422{ U("Main12-422") };
            const profile Main_444{ U("Main-444") };
            const profile Main10_444{ U("Main10-444") };
            const profile Main12_444{ U("Main12-444") };
            const profile Main16_444{ U("Main16-444") };
            const profile MainIntra{ U("MainIntra") };
            const profile Main10Intra{ U("Main10Intra") };
            const profile Main12Intra{ U("Main12Intra") };
            const profile Main10Intra_422{ U("Main10Intra-422") };
            const profile Main12Intra_422{ U("Main12Intra-422") };
            const profile MainIntra_444{ U("MainIntra-444") };
            const profile Main10Intra_444{ U("Main10Intra-444") };
            const profile Main12Intra_444{ U("Main12Intra-444") };
            const profile Main16Intra_444{ U("Main16Intra-444") };
            const profile MainStillPicture_444{ U("MainStillPicture-444") };
            const profile Main16StillPicture_444{ U("Main16StillPicture-444") };

            // High throughput profiles (H.265 A.3.6, A.3.7)
            const profile HighThroughput_444{ U("HighThroughput-444") };
            const profile HighThroughput10_444{ U("HighThroughput10-444") };
            const profile HighThroughput14_444{ U("HighThroughput14-444") };
            const profile HighThroughput14Intra_444{ U("HighThroughput14Intra-444") };

            // Screen content coding extensions profiles (H.265 A.3.8)
            const profile ScreenExtendedMain{ U("ScreenExtendedMain") };
            const profile ScreenExtendedMain10{ U("ScreenExtendedMain10") };
            const profile ScreenExtendedMain_444{ U("ScreenExtendedMain-444") };
            const profile ScreenExtendedMain10_444{ U("ScreenExtendedMain10-444") };
            const profile ScreenExtendedHighThroughput_444{ U("ScreenExtendedHighThroughput-444") };
            const profile ScreenExtendedHighThroughput10_444{ U("ScreenExtendedHighThroughput10-444") };
            const profile ScreenExtendedHighThroughput14_444{ U("ScreenExtendedHighThroughput14-444") };
        }

        // Level, i.e. the tier name followed by the level number
        // See Rec. ITU-T H.265 Annex A.4
        // and https://specs.amwa.tv/nmos-parameter-registers/branches/main/flow-attributes/#level
        DEFINE_STRING_ENUM(level)
        namespace levels
        {
            // Main tier
            const level Main_1{ U("Main-1") };
            const level Main_2{ U("Main-2") };
            const level Main_2_1{ U("Main-2.1") };
            const level Main_3{ U("Main-3") };
            const level Main_3_1{ U("Main-3.1") }; // default
            const level Main_4{ U("Main-4") };
            const level Main_4_1{ U("Main-4.1") };
            const level Main_5{ U("Main-5") };
            const level Main_5_1{ U("Main-5.1") };
            const level Main_5_2{ U("Main-5.2") };
            const level Main_6{ U("Main-6") };
            const level Main_6_1{ U("Main-6.1") };
            const level Main_6_2{ U("Main-6.2") };
            const level Main_8_5{ U("Main-8.5") };

            // High tier, defined only from level 4 upwards
            const level High_4{ U("High-4") };
            const level High_4_1{ U("High-4.1") };
            const level High_5{ U("High-5") };
            const level High_5_1{ U("High-5.1") };
            const level High_5_2{ U("High-5.2") };
            const level High_6{ U("High-6") };
            const level High_6_1{ U("High-6.1") };
            const level High_6_2{ U("High-6.2") };
            const level High_8_5{ U("High-8.5") };
        }

        // Packet Transmission Mode, derived from sprop-max-don-diff
        // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/sender-attributes/#packet-transmission-mode
        DEFINE_STRING_ENUM(packet_transmission_mode)
        namespace packet_transmission_modes
        {
            const packet_transmission_mode non_interleaved_nal_units{ U("non_interleaved_nal_units") }; // default
            const packet_transmission_mode interleaved_nal_units{ U("interleaved_nal_units") };
        }

        // Parameter Sets Flow Mode, i.e. whether and how the parameter sets in the bitstream may change
        // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/sender-attributes/#parameter-sets-flow-mode
        DEFINE_STRING_ENUM(parameter_sets_flow_mode)
        namespace parameter_sets_flow_modes
        {
            const parameter_sets_flow_mode strict{ U("strict") };
            const parameter_sets_flow_mode statik{ U("static") }; // 'static' is a keyword
            const parameter_sets_flow_mode dynamic{ U("dynamic") }; // default
        }

        // Parameter Sets Transport Mode, i.e. whether the parameter sets are carried in the
        // bitstream, in the SDP transport file, or both
        // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/sender-attributes/#parameter-sets-transport-mode
        DEFINE_STRING_ENUM(parameter_sets_transport_mode)
        namespace parameter_sets_transport_modes
        {
            const parameter_sets_transport_mode in_band{ U("in_band") }; // default
            const parameter_sets_transport_mode out_of_band{ U("out_of_band") };
            const parameter_sets_transport_mode in_and_out_of_band{ U("in_and_out_of_band") };
        }

        // Map a Flow's profile and level onto the SDP profile-tier-level, and back.
        //
        // The table is derived from Rec. ITU-T H.265 Table A.2 and A.3. general_profile_idc alone
        // is unambiguous only for Main (1), Main10 (2) and MainStillPicture (3); the format range
        // extensions (4), high throughput (5) and screen content coding (9) families each share a
        // profile_id and are disambiguated by the general constraint flags carried in
        // interop-constraints (max_12bit, max_10bit, max_8bit, max_422chroma, max_420chroma,
        // max_monochrome, intra, one_picture_only, lower_bit_rate). Transcribe these from the
        // standard; do not guess. Both functions throw nmos::details::sdp_processing_error for
        // values not in the table.
        sdp::video_h265::profile_tier_level make_profile_tier_level(const profile& profile, const level& level);
        std::pair<profile, level> parse_profile_tier_level(const sdp::video_h265::profile_tier_level& ptl);
    }

    // Additional "video/H265" parameters, as parsed from an SDP transport file
    // See https://www.iana.org/assignments/media-types/video/H265
    // and https://tools.ietf.org/html/rfc7798#section-7.1
    struct video_H265_parameters
    {
        // fmtp indicates format
        sdp::video_h265::profile_tier_level ptl;

        // fmtp indicates packetization
        uint32_t sprop_max_don_diff; // if omitted (zero), the non-interleaved mode is in use
        uint32_t sprop_depack_buf_nalus;
        uint32_t sprop_depack_buf_bytes;
        utility::string_t tx_mode; // if omitted (empty), assume sdp::video_h265::tx_modes::SRST

        // fmtp indicates the out-of-band parameter sets, kept verbatim, including any trailing ','
        utility::string_t sprop_vps;
        utility::string_t sprop_sps;
        utility::string_t sprop_pps;

        // bandwidth
        uint64_t bit_rate; // transport bit rate (kilobits/second); if omitted, zero

        video_H265_parameters()
            : ptl()
            , sprop_max_don_diff()
            , sprop_depack_buf_nalus()
            , sprop_depack_buf_bytes()
            , bit_rate()
        {}

        video_H265_parameters(
            sdp::video_h265::profile_tier_level ptl,
            uint32_t sprop_max_don_diff,
            uint32_t sprop_depack_buf_nalus,
            uint32_t sprop_depack_buf_bytes,
            utility::string_t tx_mode,
            utility::string_t sprop_vps,
            utility::string_t sprop_sps,
            utility::string_t sprop_pps,
            uint64_t bit_rate
        )
            : ptl(ptl)
            , sprop_max_don_diff(sprop_max_don_diff)
            , sprop_depack_buf_nalus(sprop_depack_buf_nalus)
            , sprop_depack_buf_bytes(sprop_depack_buf_bytes)
            , tx_mode(std::move(tx_mode))
            , sprop_vps(std::move(sprop_vps))
            , sprop_sps(std::move(sprop_sps))
            , sprop_pps(std::move(sprop_pps))
            , bit_rate(bit_rate)
        {}
    };

    // Which packet transmission mode the Sender is using, derived from sprop-max-don-diff
    video_h265::packet_transmission_mode get_video_H265_packet_transmission_mode(const video_H265_parameters& params);

    // Which parameter sets transport mode the Sender is using, derived from the sprop-* parameters:
    //   all absent or empty              -> in_band
    //   present, without a trailing ','  -> out_of_band
    //   present, with a trailing ','     -> in_and_out_of_band
    // (a lone "," means in_and_out_of_band, with nothing conveyed out-of-band)
    video_h265::parameter_sets_transport_mode get_video_H265_parameter_sets_transport_mode(const video_H265_parameters& params);

    // Get additional "video/H265" parameters from the SDP parameters
    video_H265_parameters get_video_H265_parameters(const sdp_parameters& sdp_params);
    video_H265_parameters get_video_H265_parameters_or_defaults(const sdp_parameters& sdp_params);

    // Validate SDP parameters for "video/H265" against IS-04 receiver capabilities
    void validate_video_H265_sdp_parameters(const web::json::value& receiver, const nmos::sdp_parameters& sdp_params);

    // See https://specs.amwa.tv/bcp-006-03/branches/v1.0-dev/docs/NMOS_With_H.265.html#flows
    // cf. nmos::make_coded_video_flow
    nmos::resource make_video_H265_flow(
        const nmos::id& id,
        const nmos::id& source_id,
        const nmos::id& device_id,
        const nmos::rational& grain_rate,
        unsigned int frame_width,
        unsigned int frame_height,
        const nmos::interlace_mode& interlace_mode,
        const nmos::colorspace& colorspace,
        const nmos::transfer_characteristic& transfer_characteristic,
        const sdp::sampling& color_sampling,
        unsigned int bit_depth,
        const video_h265::profile& profile,
        const video_h265::level& level,
        uint64_t bit_rate, // format bit rate (kilobits/second)
        bool constant_bit_rate,
        const nmos::settings& settings);
}

#endif
