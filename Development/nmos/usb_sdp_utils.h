#ifndef NMOS_USB_SDP_UTILS_H
#define NMOS_USB_SDP_UTILS_H

#include "nmos/sdp_utils.h"

#include "sdp/json.h"
#include "cpprest/json.h"

namespace nmos
{
    struct usb_sdp_parameters
    {
        sdp_parameters::origin_t origin;
        utility::string_t session_name;
        sdp_parameters::timing_t timing;

        struct leg_t
        {
            utility::string_t source_ip;
            uint16_t          source_port;
        };
        std::vector<leg_t> legs;

        std::vector<sdp_parameters::ts_refclk_t> ts_refclk;
        sdp_parameters::mediaclk_t mediaclk;

        utility::string_t setup;
        utility::string_t privacy;
        sdp_parameters::group_t group;

        usb_sdp_parameters() {}
    };

    // Returns true when the raw SDP text describes a USB-over-TCP stream
    bool is_usb_transport_file(const utility::string_t& transport_file_data);

    // Parse the JSON representation of a USB SDP file into usb_sdp_parameters
    // and IS-05-compatible transport parameters
    std::pair<usb_sdp_parameters, web::json::value> parse_usb_session_description(const web::json::value& session_description);

    // High-level entry point: parse a raw transport file
    std::pair<usb_sdp_parameters, web::json::value> parse_usb_transport_file(const utility::string_t& transport_file);
}

#endif
