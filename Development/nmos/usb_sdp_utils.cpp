#include "nmos/usb_sdp_utils.h"
#include "nmos/json_fields.h"
#include "sdp/sdp.h"

namespace nmos
{
    bool is_usb_transport_file(const utility::string_t& transport_file_data)
    {
        return transport_file_data.find(U("TCP usb")) != utility::string_t::npos ||
               transport_file_data.find(U("TCP/usb")) != utility::string_t::npos;
    }

    std::pair<usb_sdp_parameters, web::json::value>
    parse_usb_session_description(const web::json::value& session_description)
    {
        using web::json::value;
        using web::json::value_of;

        usb_sdp_parameters usb_params;
        web::json::value transport_params = value::array();

        if (0 != sdp::fields::protocol_version(session_description))
            throw details::sdp_processing_error("unsupported protocol version");

        const auto& origin = sdp::fields::origin(session_description);
        usb_params.origin = {
            sdp::fields::user_name(origin),
            sdp::fields::session_id(origin),
            sdp::fields::session_version(origin)
        };

        usb_params.session_name = sdp::fields::session_name(session_description);

        const auto& time_descriptions = sdp::fields::time_descriptions(session_description).as_array();
        if (time_descriptions.size())
        {
            const auto& timing = sdp::fields::timing(time_descriptions.at(0));
            usb_params.timing = { sdp::fields::start_time(timing), sdp::fields::stop_time(timing) };
        }

        auto& session_attributes = sdp::fields::attributes(session_description).as_array();

        {
            auto group_it = sdp::find_name(session_attributes, sdp::attributes::group);
            if (session_attributes.end() != group_it)
            {
                const auto& gval = sdp::fields::value(*group_it);
                usb_params.group.semantics = sdp::group_semantics_type{ sdp::fields::semantics(gval) };
                for (const auto& mid : sdp::fields::mids(gval))
                    usb_params.group.media_stream_ids.push_back(mid.as_string());
            }
        }

        utility::string_t session_connection_address;
        {
            const auto& scd = sdp::fields::connection_data(session_description);
            if (!scd.is_null())
                session_connection_address = sdp::fields::connection_address(scd);
        }

        const auto& media_descriptions = sdp::fields::media_descriptions(session_description);
        usb_params.ts_refclk.reserve(media_descriptions.size());

        for (const auto& md : media_descriptions.as_array())
        {
            const auto& media = sdp::fields::media(md);

            const sdp::media_type media_type{ sdp::fields::media_type(media) };
            const sdp::protocol   protocol  { sdp::fields::protocol(media)   };

            if (sdp::media_types::application != media_type) continue;
            if (U("TCP") != protocol.name)                   continue;

            const uint16_t port = static_cast<uint16_t>(sdp::fields::port(media));

            utility::string_t connection_address = session_connection_address;
            {
                const auto& mcd = sdp::fields::connection_data(md);
                if (!mcd.is_null() && mcd.size() > 0)
                    connection_address = sdp::fields::connection_address(mcd.at(0));
            }
            web::json::value leg_params = value_of({
                { nmos::fields::usb_source_ip,    value::string(connection_address) },
                { nmos::fields::usb_source_port,  value::number(port)               },
                { nmos::fields::usb_interface_ip, value::string(U("auto"))           },
            });
            web::json::push_back(transport_params, std::move(leg_params));

            usb_params.legs.push_back({ connection_address, port });

            auto& media_attributes = sdp::fields::attributes(md).as_array();

            // a=ts-refclk (fall back to session-level)
            usb_params.ts_refclk.push_back([&]() -> sdp_parameters::ts_refclk_t
            {
                auto ts_it = sdp::find_name(media_attributes, sdp::attributes::ts_refclk);
                if (media_attributes.end() == ts_it)
                {
                    ts_it = sdp::find_name(session_attributes, sdp::attributes::ts_refclk);
                    if (session_attributes.end() == ts_it)
                        return {};
                }
                const auto& tsval = sdp::fields::value(*ts_it);
                sdp::ts_refclk_source clock_source{ sdp::fields::clock_source(tsval) };
                if (sdp::ts_refclk_sources::ptp == clock_source)
                    return sdp_parameters::ts_refclk_t::ptp(
                        sdp::ptp_version{ sdp::fields::ptp_version(tsval) },
                        sdp::fields::ptp_server(tsval));
                else if (sdp::ts_refclk_sources::local_mac == clock_source)
                    return sdp_parameters::ts_refclk_t::local_mac(
                        sdp::fields::mac_address(tsval));
                else
                    return {};
            }());

            if (usb_params.mediaclk.clock_source.name.empty()){
                // a=mediaclk (fall back to session-level)
                usb_params.mediaclk = [&]() -> sdp_parameters::mediaclk_t
                {
                    auto mclk_it = sdp::find_name(media_attributes, sdp::attributes::mediaclk);
                    if (media_attributes.end() == mclk_it)
                    {
                        mclk_it = sdp::find_name(session_attributes, sdp::attributes::mediaclk);
                        if (session_attributes.end() == mclk_it)
                            return {};
                    }
                    const auto& val = sdp::fields::value(*mclk_it).as_string();
                    const auto eq = val.find(U('='));
                    return { sdp::media_clock_source{ val.substr(0, eq) }, utility::string_t::npos != eq ? val.substr(eq + 1) : utility::string_t{} };
                }();
            }

            if (usb_params.setup.empty())
            {
                auto setup_it = sdp::find_name(media_attributes, U("setup"));
                if (media_attributes.end() != setup_it)
                    usb_params.setup = sdp::fields::value(*setup_it).as_string();
            }

            if (usb_params.privacy.empty())
            {
                auto priv_it = sdp::find_name(media_attributes, U("privacy"));
                if (media_attributes.end() != priv_it)
                {
                    const auto& pval = sdp::fields::value(*priv_it);
                    usb_params.privacy = pval.is_string() ? pval.as_string() : pval.serialize();
                }
            }            
        }

        if (usb_params.legs.empty())
            throw details::sdp_processing_error("no USB media description found");

        return { usb_params, transport_params };
    }

    std::pair<usb_sdp_parameters, web::json::value>
    parse_usb_transport_file(const utility::string_t& transport_file)
    {
        const auto session_description = sdp::parse_session_description(utility::us2s(transport_file));

        return parse_usb_session_description(session_description);
    }
}
