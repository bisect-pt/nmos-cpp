#include "nmos/video_h265.h"

#include <algorithm>
#include <map>
#include <vector>
#include "nmos/capabilities.h"
#include "nmos/format.h"
#include "nmos/interlace_mode.h"
#include "nmos/json_fields.h"
#include "nmos/resource.h"

namespace nmos
{
    namespace video_h265
    {
        namespace details
        {
            // Bit positions within the 48-bit interop-constraints, most significant bit first
            // See Rec. ITU-T H.265 and https://tools.ietf.org/html/rfc7798#section-7.1
            const uint64_t progressive_source_flag = 1ull << 47; // general_progressive_source_flag
            const uint64_t interlaced_source_flag = 1ull << 46; // general_interlaced_source_flag
            const uint64_t non_packed_constraint_flag = 1ull << 45; // general_non_packed_constraint_flag
            const uint64_t frame_only_constraint_flag = 1ull << 44; // general_frame_only_constraint_flag

            // The remaining 44 bits are the reserved bits, which for general_profile_idc 4, 5 and 9
            // carry the general constraint flags that distinguish the individual profiles:
            //   max_12bit, max_10bit, max_8bit, max_422chroma, max_420chroma, max_monochrome,
            //   intra, one_picture_only, lower_bit_rate, max_14bit, ...
            // Transcribe their positions and values from Rec. ITU-T H.265 Table A.2 and A.3.
            // NB: for some profiles a constraint is asserted by the flag being *clear* rather than
            // set - check the standard rather than assuming.

            struct profile_entry
            {
                uint32_t profile_space;
                uint32_t profile_id;
                uint64_t interop_constraints_mask; // which bits of interop-constraints are significant
                uint64_t interop_constraints_value; // the required value of those bits
                nmos::video_h265::profile profile;
            };

            // See Rec. ITU-T H.265 Annex A.3
            // general_profile_idc alone identifies only Main (1), Main10 (2) and MainStillPicture (3).
            // The format range extensions (4), high throughput (5) and screen content coding (9)
            // families each share a profile_id and are disambiguated purely by the constraint flags,
            // so each needs a mask/value pair. Populate as required; profiles not in the table are
            // rejected rather than silently mapped to something plausible.
            static const std::vector<profile_entry> profile_table
            {
                { 0, 1, 0, 0, profiles::Main },
                { 0, 2, 0, 0, profiles::Main10 },
                { 0, 3, 0, 0, profiles::MainStillPicture }
                // TODO: format range extensions (profile_id 4), high throughput (5),
                // screen content coding (9)
            };

            // "Main-3.1" etc. -> tier_flag, level_id
            std::pair<uint32_t, uint32_t> parse_level_name(const level& level)
            {
                const auto dash = level.name.find(U('-'));
                if (utility::string_t::npos == dash) throw std::invalid_argument("invalid H.265 level");

                const auto tier = level.name.substr(0, dash);
                uint32_t tier_flag;
                if (U("Main") == tier) tier_flag = 0;
                else if (U("High") == tier) tier_flag = 1;
                else throw std::invalid_argument("invalid H.265 tier");

                // the level number is either "n" or "n.m"
                const auto number = level.name.substr(dash + 1);
                const auto dot = number.find(U('.'));
                const auto units = utility::istringstreamed<uint32_t>(utility::string_t::npos == dot ? number : number.substr(0, dot));
                const uint32_t tenths = utility::string_t::npos == dot ? 0 : utility::istringstreamed<uint32_t>(number.substr(dot + 1));

                // general_level_idc == 30 * level
                return{ tier_flag, 3 * (10 * units + tenths) };
            }

            // tier_flag, level_id -> "Main-3.1" etc.
            level make_level_name(uint32_t tier_flag, uint32_t level_id)
            {
                if (0 != level_id % 3) throw std::invalid_argument("invalid H.265 level-id");

                const auto tenths = level_id / 3; // e.g. 93 -> 31, i.e. level 3.1
                utility::ostringstream_t os;
                os << (0 == tier_flag ? U("Main-") : U("High-")) << (tenths / 10);
                if (0 != tenths % 10) os << U(".") << (tenths % 10);
                return level{ os.str() };
            }
        }

        // Map the Flow's profile and level onto the SDP profile-tier-level
        sdp::video_h265::profile_tier_level make_profile_tier_level(const profile& profile, const level& level)
        {
            const auto found = std::find_if(details::profile_table.begin(), details::profile_table.end(), [&](const details::profile_entry& entry)
            {
                return entry.profile == profile;
            });
            if (details::profile_table.end() == found) throw std::invalid_argument("unsupported H.265 profile");

            const auto tier_level = details::parse_level_name(level);

            // general_profile_compatibility_flag[general_profile_idc] must be set
            const uint32_t compatibility = 1u << (31 - found->profile_id);

            return sdp::video_h265::profile_tier_level{
                found->profile_space,
                found->profile_id,
                compatibility,
                found->interop_constraints_value,
                tier_level.first,
                tier_level.second
            };
        }

        // Map the SDP profile-tier-level onto the Flow's profile and level
        std::pair<profile, level> parse_profile_tier_level(const sdp::video_h265::profile_tier_level& ptl)
        {
            const auto found = std::find_if(details::profile_table.begin(), details::profile_table.end(), [&](const details::profile_entry& entry)
            {
                return entry.profile_space == ptl.profile_space
                    && entry.profile_id == ptl.profile_id
                    && (ptl.interop_constraints & entry.interop_constraints_mask) == entry.interop_constraints_value;
            });
            if (details::profile_table.end() == found) throw std::invalid_argument("unsupported H.265 profile-tier-level");

            return{ found->profile, details::make_level_name(ptl.tier_flag, ptl.level_id) };
        }
    }

    // Which packet transmission mode the Sender is using, derived from sprop-max-don-diff
    video_h265::packet_transmission_mode get_video_H265_packet_transmission_mode(const video_H265_parameters& params)
    {
        return 0 == params.sprop_max_don_diff
            ? video_h265::packet_transmission_modes::non_interleaved_nal_units
            : video_h265::packet_transmission_modes::interleaved_nal_units;
    }

    // Which parameter sets transport mode the Sender is using, derived from the sprop-* parameters
    video_h265::parameter_sets_transport_mode get_video_H265_parameter_sets_transport_mode(const video_H265_parameters& params)
    {
        if (params.sprop_vps.empty() && params.sprop_sps.empty() && params.sprop_pps.empty())
            return video_h265::parameter_sets_transport_modes::in_band;

        // a trailing ',' indicates a terminating empty NAL unit, i.e. the parameter sets are also
        // conveyed in-band; a lone "," means nothing is conveyed out-of-band
        const auto trailing_comma = [](const utility::string_t& value)
        {
            return !value.empty() && U(',') == value.back();
        };
        if (trailing_comma(params.sprop_vps) || trailing_comma(params.sprop_sps) || trailing_comma(params.sprop_pps))
            return video_h265::parameter_sets_transport_modes::in_and_out_of_band;

        return video_h265::parameter_sets_transport_modes::out_of_band;
    }

    // Get additional "video/H265" parameters from the SDP parameters
    // Every RFC 7798 fmtp parameter is optional and has a default, so unlike "video/jxsv" there is
    // no MissingRequiredParameter policy; the two public overloads are identical, and provided only
    // for symmetry with the other coded video formats.
    template <typename MissingRequiredParameter>
    video_H265_parameters get_video_H265_parameters(const sdp_parameters& sdp_params, MissingRequiredParameter missing = MissingRequiredParameter{})
    {
        video_H265_parameters params;

        (void)missing;

        // optional
        const auto profile_space = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::profile_space);
        params.ptl.profile_space = sdp_params.fmtp.end() != profile_space
            ? utility::istringstreamed<uint32_t>(profile_space->second)
            : 0;

        // optional; 1 == Main
        const auto profile_id = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::profile_id);
        params.ptl.profile_id = sdp_params.fmtp.end() != profile_id
            ? utility::istringstreamed<uint32_t>(profile_id->second)
            : 1;

        // optional
        const auto profile_compatibility_indicator = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::profile_compatibility_indicator);
        params.ptl.profile_compatibility_indicator = sdp_params.fmtp.end() != profile_compatibility_indicator
            ? (uint32_t)sdp::video_h265::parse_hex_string(profile_compatibility_indicator->second)
            : 0;

        // optional
        const auto interop_constraints = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::interop_constraints);
        params.ptl.interop_constraints = sdp_params.fmtp.end() != interop_constraints
            ? sdp::video_h265::parse_hex_string(interop_constraints->second)
            : 0;

        // optional; 0 == Main tier
        const auto tier_flag = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::tier_flag);
        params.ptl.tier_flag = sdp_params.fmtp.end() != tier_flag
            ? utility::istringstreamed<uint32_t>(tier_flag->second)
            : 0;

        // optional; 93 == level 3.1
        const auto level_id = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::level_id);
        params.ptl.level_id = sdp_params.fmtp.end() != level_id
            ? utility::istringstreamed<uint32_t>(level_id->second)
            : 93;

        // optional; zero indicates the non-interleaved packetization mode
        const auto sprop_max_don_diff = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::sprop_max_don_diff);
        if (sdp_params.fmtp.end() != sprop_max_don_diff) params.sprop_max_don_diff = utility::istringstreamed<uint32_t>(sprop_max_don_diff->second);

        // optional
        const auto sprop_depack_buf_nalus = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::sprop_depack_buf_nalus);
        if (sdp_params.fmtp.end() != sprop_depack_buf_nalus) params.sprop_depack_buf_nalus = utility::istringstreamed<uint32_t>(sprop_depack_buf_nalus->second);

        // optional
        const auto sprop_depack_buf_bytes = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::sprop_depack_buf_bytes);
        if (sdp_params.fmtp.end() != sprop_depack_buf_bytes) params.sprop_depack_buf_bytes = utility::istringstreamed<uint32_t>(sprop_depack_buf_bytes->second);

        // optional; SRST is the only mode permitted by BCP-006-03
        const auto tx_mode = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::tx_mode);
        params.tx_mode = sdp_params.fmtp.end() != tx_mode
            ? tx_mode->second
            : sdp::video_h265::tx_modes::SRST;

        // optional; kept verbatim, including any trailing ','
        const auto sprop_vps = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::sprop_vps);
        if (sdp_params.fmtp.end() != sprop_vps) params.sprop_vps = sprop_vps->second;

        // optional
        const auto sprop_sps = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::sprop_sps);
        if (sdp_params.fmtp.end() != sprop_sps) params.sprop_sps = sprop_sps->second;

        // optional
        const auto sprop_pps = details::find_fmtp(sdp_params.fmtp, sdp::video_h265::fields::sprop_pps);
        if (sdp_params.fmtp.end() != sprop_pps) params.sprop_pps = sprop_pps->second;

        // optional
        if (sdp::bandwidth_types::application_specific == sdp_params.bandwidth.bandwidth_type) params.bit_rate = sdp_params.bandwidth.bandwidth;

        return params;
    }

    // Get additional "video/H265" parameters from the SDP parameters
    video_H265_parameters get_video_H265_parameters(const sdp_parameters& sdp_params)
    {
        return get_video_H265_parameters<details::throw_missing_fmtp>(sdp_params);
    }

    // Get additional "video/H265" parameters from the SDP parameters
    video_H265_parameters get_video_H265_parameters_or_defaults(const sdp_parameters& sdp_params)
    {
        return get_video_H265_parameters<>(sdp_params, [](const utility::string_t&) {});
    }

    namespace details
    {
        const video_H265_parameters* get_h265(const format_parameters* format) { return get<video_H265_parameters>(format); }

        // NMOS Parameter Registers - Capabilities register
        // See https://specs.amwa.tv/nmos-parameter-registers/branches/main/capabilities/
        //
        // NB: unlike "video/jxsv", the RFC 7798 fmtp carries no sampling, depth, width, height,
        // exactframerate, colorimetry, tcs or range, so the corresponding Receiver Capabilities
        // cannot be checked against the transport file. They are deliberately absent from this map,
        // so that nmos::details::validate_sdp_parameters ignores them, i.e. those constraints fail
        // *open*. Do not add matchers for them that return false; that would reject valid streams.
#define CAPS_ARGS const sdp_parameters& sdp, const format_parameters& format, const web::json::value& con
        static const std::map<utility::string_t, std::function<bool(CAPS_ARGS)>> h265_constraints
        {
            { nmos::caps::format::media_type, [](CAPS_ARGS) { return nmos::match_string_constraint(get_media_type(sdp).name, con); } },
            { nmos::caps::format::profile, [](CAPS_ARGS) { auto h265 = get_h265(&format); return h265 && nmos::match_string_constraint(nmos::video_h265::parse_profile_tier_level(h265->ptl).first.name, con); } },
            { nmos::caps::format::level, [](CAPS_ARGS) { auto h265 = get_h265(&format); return h265 && nmos::match_string_constraint(nmos::video_h265::parse_profile_tier_level(h265->ptl).second.name, con); } },
            { nmos::caps::transport::bit_rate, [](CAPS_ARGS) { auto h265 = get_h265(&format); return h265 && (0 == h265->bit_rate || nmos::match_integer_constraint(h265->bit_rate, con)); } },
            { nmos::caps::transport::packet_transmission_mode, [](CAPS_ARGS) { auto h265 = get_h265(&format); return h265 && nmos::match_string_constraint(nmos::get_video_H265_packet_transmission_mode(*h265).name, con); } },
            { nmos::caps::transport::parameter_sets_transport_mode, [](CAPS_ARGS) { auto h265 = get_h265(&format); return h265 && nmos::match_string_constraint(nmos::get_video_H265_parameter_sets_transport_mode(*h265).name, con); } }
        };
#undef CAPS_ARGS
    }

    // Validate SDP parameters for "video/H265" against IS-04 receiver capabilities
    // cf. nmos::validate_sdp_parameters
    void validate_video_H265_sdp_parameters(const web::json::value& receiver, const nmos::sdp_parameters& sdp_params)
    {
        // this function can only be used to validate SDP data for "video/H265"; logic error otherwise
        const auto media_type = get_media_type(sdp_params);
        if (nmos::media_types::video_H265 != media_type) throw std::invalid_argument("unexpected media type/encoding name");

        const auto params = get_video_H265_parameters(sdp_params);

        // BCP-006-03 permits only Single RTP stream transport
        // See https://specs.amwa.tv/bcp-006-03/branches/v1.0-dev/docs/NMOS_With_H.265.html#senders
        if (sdp::video_h265::tx_modes::SRST != params.tx_mode) throw nmos::details::sdp_processing_error("unsupported tx-mode; only SRST is supported");

        nmos::details::validate_sdp_parameters(details::h265_constraints, sdp_params, nmos::formats::video, params, receiver);
    }

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
        uint64_t bit_rate,
        bool constant_bit_rate,
        const nmos::settings& settings)
    {
        using web::json::value;

        // fail early if the profile and level cannot be expressed in an SDP transport file
        if (!profile.empty() && !level.empty()) video_h265::make_profile_tier_level(profile, level);

        auto resource = nmos::make_coded_video_flow(
            id, source_id, device_id,
            grain_rate,
            frame_width, frame_height, interlace_mode,
            colorspace, transfer_characteristic, color_sampling, bit_depth,
            nmos::media_types::video_H265,
            settings
        );
        auto& data = resource.data;

        // additional attributes required by BCP-006-03
        // see https://specs.amwa.tv/bcp-006-03/branches/v1.0-dev/docs/NMOS_With_H.265.html#flows
        if (!profile.empty()) data[nmos::fields::profile] = value(profile.name);
        if (!level.empty()) data[nmos::fields::level] = value(level.name);
        if (0 != bit_rate) data[nmos::fields::bit_rate] = value(bit_rate);
        // constant_bit_rate defaults to false, so only include it when true
        if (constant_bit_rate) data[nmos::fields::constant_bit_rate] = value(true);

        return resource;
    }
}


namespace sdp{
    namespace video_h265{
        // "profile-compatibility-indicator" (32 bits) and "interop-constraints" (48 bits) are
        // hexadecimal strings, groups of two hex digits separated by "..",
        // e.g. "B0.00.00.00.00.00"
        // See https://tools.ietf.org/html/rfc7798#section-7.1
        uint64_t parse_hex_string(const utility::string_t& value)
        {
            uint64_t result = 0;
            for (const auto c : value)
            {
                if (U('.') == c) continue;

                uint64_t nibble;
                if (c >= U('0') && c <= U('9')) nibble = c - U('0');
                else if (c >= U('a') && c <= U('f')) nibble = 10 + (c - U('a'));
                else if (c >= U('A') && c <= U('F')) nibble = 10 + (c - U('A'));
                else throw std::invalid_argument("invalid hexadecimal value");

                result = (result << 4) | nibble;
            }
            return result;
        }

        utility::string_t make_hex_string(uint64_t value, size_t bytes)
        {
            static const utility::char_t* const digits = U("0123456789ABCDEF");
            utility::string_t result;
            for (size_t i = 0; i < bytes; ++i)
            {
                const auto byte = (value >> (8 * (bytes - 1 - i))) & 0xFF;
                if (0 != i) result.push_back(U('.'));
                result.push_back(digits[(byte >> 4) & 0xF]);
                result.push_back(digits[byte & 0xF]);
            }
            return result;
        }
    }
}
