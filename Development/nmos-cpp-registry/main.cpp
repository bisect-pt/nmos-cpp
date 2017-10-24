#include <iostream>
#include "cpprest/host_utils.h"
#include "cpprest/ws_listener.h"
#include "mdns/service_advertiser.h"
#include "nmos/api_utils.h"
#include "nmos/admin_ui.h"
#include "nmos/logging_api.h"
#include "nmos/model.h"
#include "nmos/mdns.h"
#include "nmos/mdns_api.h"
#include "nmos/node_api.h"
#include "nmos/query_api.h"
#include "nmos/query_ws_api.h"
#include "nmos/registration_api.h"
#include "nmos/settings_api.h"
#include "nmos/server_resources.h"
#include "main_gate.h"

int main(int argc, char* argv[])
{
    // Construct our data models and mutexes to protect each of them
    // plus variables to signal when the server is stopping

    nmos::resources self_resources;
    std::mutex self_mutex;

    nmos::model registry_model;
    std::mutex registry_mutex;

    nmos::experimental::log_model log_model;
    std::mutex log_mutex;
    std::atomic<slog::severity> level{ slog::severities::more_info };

    bool shutdown = false;

    // Streams for logging, initially configured to write errors to stderr and to discard the access log
    std::filebuf error_log_buf;
    std::ostream error_log(std::cerr.rdbuf());
    std::filebuf access_log_buf;
    std::ostream access_log(&access_log_buf);

    // Logging should all go through this logging gateway
    main_gate gate(error_log, access_log, log_model, log_mutex, level);

    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Starting nmos-cpp registry";

    // Settings can be passed on the command-line, and some may be changed dynamically by POST to /settings/all on the Settings API
    //
    // * "logging_level": integer value, between 40 (least verbose, only fatal messages) and -40 (most verbose)
    // * "allow_invalid_resources": boolean value, true (cope with out-of-order Ledger and LAWO registrations) or false (a little less lax)
    //
    // E.g.
    //
    // # nmos-cpp-registry.exe "{\"logging_level\":-40}"
    // # curl -H "Content-Type: application/json" http://localhost:3209/settings/all -d "{\"logging_level\":-40}"
    //
    // In either case, omitted settings will assume their defaults (invisibly, currently)

    if (argc > 1)
    {
        std::error_code error;
        registry_model.settings = web::json::value::parse(utility::s2us(argv[1]), error);
        if (error || !registry_model.settings.is_object())
        {
            registry_model.settings = web::json::value::null();
            slog::log<slog::severities::error>(gate, SLOG_FLF) << "Bad command-line settings [" << error << "]";
        }
        else
        {
            // Logging level is a special case (see nmos/settings_api.h)
            level = nmos::fields::logging_level(registry_model.settings);
        }
    }

    if (registry_model.settings.is_null())
    {
        // Prepare initial settings (different than defaults)
        registry_model.settings = web::json::value::object();
        registry_model.settings[nmos::fields::logging_level] = web::json::value::number(level);
        registry_model.settings[nmos::fields::allow_invalid_resources] = web::json::value::boolean(true);
        registry_model.settings[nmos::fields::host_name] = web::json::value::string(web::http::experimental::host_name());
        registry_model.settings[nmos::fields::host_address] = web::json::value::string(web::http::experimental::host_addresses(web::http::experimental::host_name())[0]);
    }

    // Reconfigure the logging streams according to settings

    if (!nmos::fields::error_log(registry_model.settings).empty())
    {
        error_log_buf.open(nmos::fields::error_log(registry_model.settings), std::ios_base::out | std::ios_base::ate);
        std::lock_guard<std::mutex> lock(log_mutex);
        error_log.rdbuf(&error_log_buf);
    }

    if (!nmos::fields::access_log(registry_model.settings).empty())
    {
        access_log_buf.open(nmos::fields::access_log(registry_model.settings), std::ios_base::out | std::ios_base::ate);
        std::lock_guard<std::mutex> lock(log_mutex);
        access_log.rdbuf(&access_log_buf);
    }

    // Log the API addresses we'll be using

    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Configuring nmos-cpp registry with its Node API at: " << nmos::fields::host_address(registry_model.settings) << ":" << nmos::fields::node_port(registry_model.settings);
    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Configuring nmos-cpp registry with its Registration API at: " << nmos::fields::host_address(registry_model.settings) << ":" << nmos::fields::registration_port(registry_model.settings);
    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Configuring nmos-cpp registry with its Query API at: " << nmos::fields::host_address(registry_model.settings) << ":" << nmos::fields::query_port(registry_model.settings);

    // Configure the mDNS API

    web::http::experimental::listener::api_router mdns_api = nmos::experimental::make_mdns_api(registry_mutex, level, gate);
    web::http::experimental::listener::http_listener mdns_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::mdns_port(registry_model.settings)));
    nmos::support_api(mdns_listener, mdns_api);

    // Configure the Settings API

    web::http::experimental::listener::api_router settings_api = nmos::experimental::make_settings_api(registry_model.settings, registry_mutex, level, gate);
    web::http::experimental::listener::http_listener settings_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::settings_port(registry_model.settings)));
    nmos::support_api(settings_listener, settings_api);

    // Configure the Logging API

    web::http::experimental::listener::api_router logging_api = nmos::experimental::make_logging_api(log_model, log_mutex, gate);
    web::http::experimental::listener::http_listener logging_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::logging_port(registry_model.settings)));
    nmos::support_api(logging_listener, logging_api);

    // Configure the Query API

    web::http::experimental::listener::api_router query_api = nmos::make_query_api(registry_model, registry_mutex, gate);
    web::http::experimental::listener::http_listener query_listener(web::http::experimental::listener::make_listener_uri(nmos::fields::query_port(registry_model.settings)));
    nmos::support_api(query_listener, query_api);

    nmos::websockets registry_websockets;

    std::condition_variable query_ws_events_condition; // associated with registry_mutex; notify on any change to registry_model, and on shutdown

    web::websockets::experimental::listener::validate_handler query_ws_validate_handler = nmos::make_query_ws_validate_handler(registry_model, registry_mutex, gate);
    web::websockets::experimental::listener::open_handler query_ws_open_handler = nmos::make_query_ws_open_handler(registry_model, registry_websockets, registry_mutex, query_ws_events_condition, gate);
    web::websockets::experimental::listener::close_handler query_ws_close_handler = nmos::make_query_ws_close_handler(registry_model, registry_websockets, registry_mutex, gate);
    web::websockets::experimental::listener::websocket_listener query_ws_listener(nmos::fields::query_ws_port(registry_model.settings), nmos::make_slog_logging_callback(gate));
    query_ws_listener.set_validate_handler(std::ref(query_ws_validate_handler));
    query_ws_listener.set_open_handler(std::ref(query_ws_open_handler));
    query_ws_listener.set_close_handler(std::ref(query_ws_close_handler));

    std::thread query_ws_events_sending([&] { nmos::send_query_ws_events_thread(query_ws_listener, registry_model, registry_websockets, registry_mutex, query_ws_events_condition, shutdown, gate); });

    // Configure the Registration API

    web::http::experimental::listener::api_router registration_api = nmos::make_registration_api(registry_model, registry_mutex, query_ws_events_condition, gate);
    web::http::experimental::listener::http_listener registration_listener(web::http::experimental::listener::make_listener_uri(nmos::fields::registration_port(registry_model.settings)));
    nmos::support_api(registration_listener, registration_api);

    std::condition_variable registration_expiration_condition; // associated with registry_mutex; notify on shutdown
    std::thread registration_expiration([&] { nmos::erase_expired_resources_thread(registry_model, registry_mutex, registration_expiration_condition, shutdown, query_ws_events_condition, gate); });

    // Configure the Node API

    web::http::experimental::listener::api_router node_api = nmos::make_node_api(self_resources, self_mutex, gate);
    web::http::experimental::listener::http_listener node_listener(web::http::experimental::listener::make_listener_uri(nmos::fields::node_port(registry_model.settings)));
    nmos::support_api(node_listener, node_api);

    // set up the node resources
    nmos::experimental::make_server_resources(self_resources, registry_model.settings);

    // add the self resources to the registration API resources
    // (for now just copy them directly, since these resources currently do not change and are configured to never expire)
    registry_model.resources.insert(self_resources.begin(), self_resources.end());

    // Configure the Admin UI

    const utility::string_t admin_filesystem_root = U("./admin");
    web::http::experimental::listener::api_router admin_ui = nmos::experimental::make_admin_ui(admin_filesystem_root, gate);
    web::http::experimental::listener::http_listener admin_listener(web::http::experimental::listener::make_listener_uri(nmos::experimental::fields::admin_port(registry_model.settings)));
    nmos::support_api(admin_listener, admin_ui);

    // Configure the mDNS advertisements for our APIs

    std::unique_ptr<mdns::service_advertiser> advertiser = mdns::make_advertiser(gate);
    const auto pri = nmos::fields::pri(registry_model.settings);
    if (nmos::service_priorities::no_priority != pri) // no_priority allows the registry to run unadvertised
    {
        const auto records = nmos::make_txt_records(pri);
        nmos::experimental::register_service(*advertiser, nmos::service_types::query, registry_model.settings, records);
        nmos::experimental::register_service(*advertiser, nmos::service_types::registration, registry_model.settings, records);
        nmos::experimental::register_service(*advertiser, nmos::service_types::node, registry_model.settings, records);
    }

    try
    {
        slog::log<slog::severities::info>(gate, SLOG_FLF) << "Preparing for connections";

        // open in an order that means NMOS APIs don't expose references to others that aren't open yet

        logging_listener.open().wait();
        settings_listener.open().wait();

        node_listener.open().wait();
        query_ws_listener.open().wait();
        query_listener.open().wait();
        registration_listener.open().wait();

        admin_listener.open().wait();

        mdns_listener.open().wait();

        advertiser->start();  

        slog::log<slog::severities::info>(gate, SLOG_FLF) << "Ready for connections";

        std::string command;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "Press return to quit." << std::endl;
        }
        std::cin >> std::noskipws >> command;

        slog::log<slog::severities::info>(gate, SLOG_FLF) << "Closing connections";

        // close in reverse order

        advertiser->stop();

        mdns_listener.close().wait();

        admin_listener.close().wait();

        registration_listener.close().wait();
        query_listener.close().wait();
        query_ws_listener.close().wait();
        node_listener.close().wait();

        settings_listener.close().wait();
        logging_listener.close().wait();
    }
    catch (const web::http::http_exception& e)
    {
        slog::log<slog::severities::error>(gate, SLOG_FLF) << e.what() << " [" << e.error_code() << "]";
    }

    shutdown = true;
    registration_expiration_condition.notify_all();
    query_ws_events_condition.notify_all();
    registration_expiration.join();
    query_ws_events_sending.join();

    slog::log<slog::severities::info>(gate, SLOG_FLF) << "Stopping nmos-cpp registry";

    return 0;
}