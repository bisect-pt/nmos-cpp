#include "nmos/video_h265.h"

#include "bst/test/test.h"
#include "nmos/capabilities.h"
#include "nmos/json_fields.h"
#include "nmos/node_resources.h"
#include "nmos/transport.h"
#include "nmos/resource.h"
#include "nmos/format.h"
#include "sdp/sdp.h"

namespace
{
    // Typical SDP data for H.265, per BCP-006-03 and RFC 7798.
    // Main10, level 4 (level-id 120), Main tier, parameter sets in-band, non-interleaved, SRST.
    const std::string test_sdp_in_band = R"(v=0
o=- 1443716955 1443716955 IN IP4 192.168.1.2
s=Example H.265 Sender
t=0 0
m=video 30000 RTP/AVP 96
c=IN IP4 224.1.1.1/64
b=AS:20000
a=ts-refclk:localmac=40-a3-6b-a0-2b-d2
a=mediaclk:direct=0
a=source-filter: incl IN IP4 224.1.1.1 192.168.1.2
a=rtpmap:96 H265/90000
a=fmtp:96 profile-id=2; profile-compatibility-indicator=40.00.00.00; interop-constraints=B0.00.00.00.00.00; level-id=120
)";

    // The same Sender, but conveying the parameter sets out-of-band as well as in-band,
    // which BCP-006-03 signals with a *trailing* comma, i.e. a terminating empty NAL unit
    const std::string test_sdp_in_and_out_of_band = R"(v=0
o=- 1443716955 1443716955 IN IP4 192.168.1.2
s=Example H.265 Sender
t=0 0
m=video 30000 RTP/AVP 96
c=IN IP4 224.1.1.1/64
b=AS:20000
a=ts-refclk:localmac=40-a3-6b-a0-2b-d2
a=mediaclk:direct=0
a=rtpmap:96 H265/90000
a=fmtp:96 profile-id=2; level-id=120; sprop-vps=QAEMAf//AWAAAAMAsAAAAwAAAwBdrAk=,; sprop-sps=QgEBAWAAAAMAsAAAAwAAAwBdoAKAgC0WNrkky/AIAAADAAgAAAMBlQ==,; sprop-pps=RAHBcrRiQA==,
)";

    nmos::sdp_parameters parse(const std::string& test_sdp)
    {
        auto test_description = sdp::parse_session_description(test_sdp);
        return nmos::parse_session_description(test_description).first;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////
BST_TEST_CASE(testSdpParametersVideoH265)
{
    const auto s = parse(test_sdp_in_band);

    BST_REQUIRE_EQUAL(nmos::media_types::video_H265, nmos::get_media_type(s));

    const auto params = nmos::get_video_H265_parameters(s);

    BST_REQUIRE_EQUAL(0, params.ptl.profile_space);
    BST_REQUIRE_EQUAL(2, params.ptl.profile_id); // Main10
    BST_REQUIRE_EQUAL(0x40000000, params.ptl.profile_compatibility_indicator);
    BST_REQUIRE_EQUAL(0xB00000000000, params.ptl.interop_constraints);
    BST_REQUIRE_EQUAL(0, params.ptl.tier_flag); // Main tier
    BST_REQUIRE_EQUAL(120, params.ptl.level_id); // level 4, i.e. 30 * 4

    // the defaults, since these are absent from the fmtp
    BST_REQUIRE_EQUAL(0, params.sprop_max_don_diff);
    BST_REQUIRE_EQUAL(sdp::video_h265::tx_modes::SRST, params.tx_mode);
    BST_REQUIRE(params.sprop_vps.empty());
    BST_REQUIRE(params.sprop_sps.empty());
    BST_REQUIRE(params.sprop_pps.empty());

    // b=AS:20000
    BST_REQUIRE_EQUAL(20000, params.bit_rate);

    // derived attributes
    BST_REQUIRE_EQUAL(nmos::video_h265::packet_transmission_modes::non_interleaved_nal_units, nmos::get_video_H265_packet_transmission_mode(params));
    BST_REQUIRE_EQUAL(nmos::video_h265::parameter_sets_transport_modes::in_band, nmos::get_video_H265_parameter_sets_transport_mode(params));

    const auto profile_level = nmos::video_h265::parse_profile_tier_level(params.ptl);
    BST_REQUIRE_EQUAL(nmos::video_h265::profiles::Main10, profile_level.first);
    BST_REQUIRE_EQUAL(nmos::video_h265::levels::Main_4, profile_level.second);
}

////////////////////////////////////////////////////////////////////////////////////////////
BST_TEST_CASE(testSdpParametersVideoH265Defaults)
{
    // an fmtp with no profile-tier-level at all must default to Main, Main tier, level 3.1
    // See https://tools.ietf.org/html/rfc7798#section-7.1
    const std::string test_sdp = R"(v=0
o=- 1443716955 1443716955 IN IP4 192.168.1.2
s=Example H.265 Sender
t=0 0
m=video 30000 RTP/AVP 96
c=IN IP4 224.1.1.1/64
a=rtpmap:96 H265/90000
)";
    const auto params = nmos::get_video_H265_parameters(parse(test_sdp));

    BST_REQUIRE_EQUAL(1, params.ptl.profile_id);
    BST_REQUIRE_EQUAL(0, params.ptl.tier_flag);
    BST_REQUIRE_EQUAL(93, params.ptl.level_id);
    BST_REQUIRE_EQUAL(0, params.bit_rate); // no b= line

    const auto profile_level = nmos::video_h265::parse_profile_tier_level(params.ptl);
    BST_REQUIRE_EQUAL(nmos::video_h265::profiles::Main, profile_level.first);
    BST_REQUIRE_EQUAL(nmos::video_h265::levels::Main_3_1, profile_level.second);
}

////////////////////////////////////////////////////////////////////////////////////////////
BST_TEST_CASE(testParameterSetsTransportMode)
{
    // the trailing comma is what distinguishes out_of_band from in_and_out_of_band
    const auto params = nmos::get_video_H265_parameters(parse(test_sdp_in_and_out_of_band));
    BST_REQUIRE_EQUAL(nmos::video_h265::parameter_sets_transport_modes::in_and_out_of_band, nmos::get_video_H265_parameter_sets_transport_mode(params));

    // no trailing comma => out_of_band
    nmos::video_H265_parameters out_of_band;
    out_of_band.sprop_sps = U("QgEBAWAAAAMAsAAAAwAAAwBdoAKAgC0WNrkky/AIAAADAAgAAAMBlQ==");
    BST_REQUIRE_EQUAL(nmos::video_h265::parameter_sets_transport_modes::out_of_band, nmos::get_video_H265_parameter_sets_transport_mode(out_of_band));

    // a lone "," means in_and_out_of_band, with nothing conveyed out-of-band
    nmos::video_H265_parameters lone_comma;
    lone_comma.sprop_sps = U(",");
    BST_REQUIRE_EQUAL(nmos::video_h265::parameter_sets_transport_modes::in_and_out_of_band, nmos::get_video_H265_parameter_sets_transport_mode(lone_comma));

    // absent entirely => in_band
    nmos::video_H265_parameters in_band;
    BST_REQUIRE_EQUAL(nmos::video_h265::parameter_sets_transport_modes::in_band, nmos::get_video_H265_parameter_sets_transport_mode(in_band));
}

////////////////////////////////////////////////////////////////////////////////////////////
BST_TEST_CASE(testProfileTierLevelRoundtrip)
{
    // every profile in the table must survive a round trip, for both tiers and a range of levels
    const std::vector<nmos::video_h265::profile> profiles{
        nmos::video_h265::profiles::Main,
        nmos::video_h265::profiles::Main10,
        nmos::video_h265::profiles::MainStillPicture
    };
    const std::vector<nmos::video_h265::level> levels{
        nmos::video_h265::levels::Main_1,
        nmos::video_h265::levels::Main_3_1,
        nmos::video_h265::levels::Main_4,
        nmos::video_h265::levels::Main_6_2,
        nmos::video_h265::levels::High_4,
        nmos::video_h265::levels::High_5_1,
        nmos::video_h265::levels::High_8_5
    };

    for (const auto& profile : profiles)
    {
        for (const auto& level : levels)
        {
            const auto ptl = nmos::video_h265::make_profile_tier_level(profile, level);
            const auto profile_level = nmos::video_h265::parse_profile_tier_level(ptl);
            BST_REQUIRE_EQUAL(profile, profile_level.first);
            BST_REQUIRE_EQUAL(level, profile_level.second);
        }
    }

    // level-id encoding, i.e. general_level_idc == 30 * level
    BST_REQUIRE_EQUAL(93, nmos::video_h265::make_profile_tier_level(nmos::video_h265::profiles::Main, nmos::video_h265::levels::Main_3_1).level_id);
    BST_REQUIRE_EQUAL(120, nmos::video_h265::make_profile_tier_level(nmos::video_h265::profiles::Main, nmos::video_h265::levels::Main_4).level_id);
    BST_REQUIRE_EQUAL(153, nmos::video_h265::make_profile_tier_level(nmos::video_h265::profiles::Main, nmos::video_h265::levels::High_5_1).level_id);
    BST_REQUIRE_EQUAL(1, nmos::video_h265::make_profile_tier_level(nmos::video_h265::profiles::Main, nmos::video_h265::levels::High_5_1).tier_flag);

    // a profile outside the table must be rejected, not silently mapped to something plausible
    BST_REQUIRE_THROW(nmos::video_h265::make_profile_tier_level(nmos::video_h265::profiles::ScreenExtendedMain10, nmos::video_h265::levels::Main_4), std::invalid_argument);
}

////////////////////////////////////////////////////////////////////////////////////////////
BST_TEST_CASE(testValidateVideoH265SdpParameters)
{
    using web::json::value;
    using web::json::value_of;

    const auto s = parse(test_sdp_in_band);

    auto receiver = nmos::make_receiver(U("receiver"), U("device"), nmos::transports::rtp, { U("eth0") }, nmos::formats::video, { nmos::media_types::video_H265 }, nmos::settings{});

    // a constraint set that the Sender satisfies
    receiver.data[nmos::fields::caps][nmos::fields::constraint_sets] = value_of({
        value_of({
            { nmos::caps::format::profile, nmos::make_caps_string_constraint({ nmos::video_h265::profiles::Main.name, nmos::video_h265::profiles::Main10.name }) },
            { nmos::caps::format::level, nmos::make_caps_string_constraint({ nmos::video_h265::levels::Main_4.name }) },
            { nmos::caps::transport::bit_rate, nmos::make_caps_integer_constraint({}, nmos::no_minimum<int64_t>(), 50000) },
            { nmos::caps::transport::packet_transmission_mode, nmos::make_caps_string_constraint({ nmos::video_h265::packet_transmission_modes::non_interleaved_nal_units.name }) },
            { nmos::caps::transport::parameter_sets_transport_mode, nmos::make_caps_string_constraint({ nmos::video_h265::parameter_sets_transport_modes::in_band.name }) }
        })
    });
    BST_REQUIRE_NO_THROW(nmos::validate_video_H265_sdp_parameters(receiver.data, s));

    // the Sender is Main10, so a Main-only Receiver must reject it
    receiver.data[nmos::fields::caps][nmos::fields::constraint_sets] = value_of({
        value_of({
            { nmos::caps::format::profile, nmos::make_caps_string_constraint({ nmos::video_h265::profiles::Main.name }) }
        })
    });
    BST_REQUIRE_THROW(nmos::validate_video_H265_sdp_parameters(receiver.data, s), std::runtime_error);

    // the Sender is 20000 kbit/s, so a Receiver capped lower must reject it
    receiver.data[nmos::fields::caps][nmos::fields::constraint_sets] = value_of({
        value_of({
            { nmos::caps::transport::bit_rate, nmos::make_caps_integer_constraint({}, nmos::no_minimum<int64_t>(), 10000) }
        })
    });
    BST_REQUIRE_THROW(nmos::validate_video_H265_sdp_parameters(receiver.data, s), std::runtime_error);

    // the Sender conveys parameter sets in-band, so an out-of-band-only Receiver must reject it
    receiver.data[nmos::fields::caps][nmos::fields::constraint_sets] = value_of({
        value_of({
            { nmos::caps::transport::parameter_sets_transport_mode, nmos::make_caps_string_constraint({ nmos::video_h265::parameter_sets_transport_modes::out_of_band.name }) }
        })
    });
    BST_REQUIRE_THROW(nmos::validate_video_H265_sdp_parameters(receiver.data, s), std::runtime_error);

    // constraints the SDP cannot speak to - width, height, sampling, depth - must fail *open*,
    // not closed, since RFC 7798 carries none of them
    receiver.data[nmos::fields::caps][nmos::fields::constraint_sets] = value_of({
        value_of({
            { nmos::caps::format::frame_width, nmos::make_caps_integer_constraint({ 1920 }) },
            { nmos::caps::format::frame_height, nmos::make_caps_integer_constraint({ 1080 }) },
            { nmos::caps::format::color_sampling, nmos::make_caps_string_constraint({ U("YCbCr-4:2:0") }) }
        })
    });
    BST_REQUIRE_NO_THROW(nmos::validate_video_H265_sdp_parameters(receiver.data, s));
}

////////////////////////////////////////////////////////////////////////////////////////////
BST_TEST_CASE(testValidateVideoH265RejectsNonSrst)
{
    // BCP-006-03 permits only Single RTP stream transport
    const std::string test_sdp = R"(v=0
o=- 1443716955 1443716955 IN IP4 192.168.1.2
s=Example H.265 Sender
t=0 0
m=video 30000 RTP/AVP 96
c=IN IP4 224.1.1.1/64
a=rtpmap:96 H265/90000
a=fmtp:96 profile-id=1; level-id=93; tx-mode=MRST
)";
    auto receiver = nmos::make_receiver(U("receiver"), U("device"), nmos::transports::rtp, { U("eth0") }, nmos::formats::video, { nmos::media_types::video_H265 }, nmos::settings{});
    BST_REQUIRE_THROW(nmos::validate_video_H265_sdp_parameters(receiver.data, parse(test_sdp)), std::runtime_error);
}
