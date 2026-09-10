#include "SnapmakerCameraSession.hpp"
#include "SnapmakerMonitorUtils.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

namespace {

namespace beast = boost::beast;
namespace net = boost::asio;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;

constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(5);
// Wall-time bounds for servicing stop requests and best-effort shutdown writes.
constexpr auto STOP_POLL_INTERVAL = std::chrono::milliseconds(25);
constexpr auto STOP_WRITE_TIMEOUT = std::chrono::milliseconds(250);
constexpr auto KEEPALIVE_INTERVAL = std::chrono::seconds(10);
constexpr auto MAX_RETRY_DELAY = std::chrono::seconds(5);

json make_identify_request()
{
    return {
        {"jsonrpc", "2.0"},
        {"method", "server.connection.identify"},
        {"params", {
            {"client_name", "MagpieSlicer"},
            {"version", "2.5.0"},
            {"type", "agent"},
            {"url", "https://github.com"}
        }},
        {"id", 1}
    };
}

json make_camera_request(const char *method, int id)
{
    json params = {{"domain", "lan"}};
    if (std::string(method) == "camera.start_monitor") {
        params["interval"] = 0;
        params["expect_pw"] = true;
    }
    return {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", std::move(params)},
        {"id", id}
    };
}

} // namespace

SnapmakerCameraSession::~SnapmakerCameraSession()
{
    stop();
}

void SnapmakerCameraSession::start(std::string base_url, std::string api_key, StateCallback callback)
{
    stop();
    if (base_url.empty())
        return;

    m_stop_requested.store(false);
    m_running.store(true);
    m_thread = std::thread(
        [this, base_url = std::move(base_url), api_key = std::move(api_key), callback = std::move(callback)]() mutable {
            run(std::move(base_url), std::move(api_key), std::move(callback));
        });
}

void SnapmakerCameraSession::stop()
{
    m_stop_requested.store(true);
    m_wait_condition.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
}

bool SnapmakerCameraSession::wait_for_stop(std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock(m_wait_mutex);
    return m_wait_condition.wait_for(lock, duration, [this]() { return m_stop_requested.load(); });
}

void SnapmakerCameraSession::run(std::string base_url, std::string api_key, StateCallback callback)
{
    auto notify = [&callback](State state, const std::string &message = std::string()) {
        if (callback)
            callback(state, message);
    };

    try {
        const SnapmakerWebSocketEndpoint endpoint =
            parse_snapmaker_websocket_endpoint(std::move(base_url));
        unsigned retry_count = 0;

        while (!m_stop_requested.load()) {
            notify(retry_count == 0 ? State::Starting : State::Reconnecting);
            try {
                net::io_context ioc;
                tcp::resolver resolver(ioc);
                websocket::stream<beast::tcp_stream> ws(ioc);
                auto cancel_io = [&] {
                    resolver.cancel();
                    beast::error_code ignored;
                    ws.next_layer().socket().cancel(ignored);
                    ws.next_layer().socket().close(ignored);
                };
                // All stream access stays on this worker. stop() only sets an
                // atomic flag; no other thread touches a Beast stream in flight.
                auto await_io = [&](auto initiate, std::chrono::steady_clock::duration timeout,
                                    bool send_stop = false) {
                    bool done = false;
                    beast::error_code result;
                    initiate([&](beast::error_code error) { result = error; done = true; });
                    const auto deadline = timeout == std::chrono::steady_clock::duration::zero()
                        ? std::chrono::steady_clock::time_point::max()
                        : std::chrono::steady_clock::now() + timeout;
                    while (!done) {
                        ioc.restart();
                        ioc.run_for(STOP_POLL_INTERVAL);
                        if (m_stop_requested.load() || std::chrono::steady_clock::now() >= deadline) {
                            if (send_stop && m_stop_requested.load() && ws.is_open()) {
                                // A write may coexist with the outstanding read.
                                // Keep its buffer alive until completion/cancellation.
                                const std::string message = make_camera_request("camera.stop_monitor", 3).dump();
                                bool sent = false;
                                ws.async_write(net::buffer(message), [&](beast::error_code, size_t) { sent = true; });
                                const auto write_deadline = std::chrono::steady_clock::now() + STOP_WRITE_TIMEOUT;
                                while (!sent && std::chrono::steady_clock::now() < write_deadline) {
                                    ioc.restart();
                                    ioc.run_for(STOP_POLL_INTERVAL);
                                }
                                cancel_io();
                                while (!done || !sent) {
                                    ioc.restart();
                                    ioc.run_for(STOP_POLL_INTERVAL);
                                }
                            } else {
                                cancel_io();
                                // Drain this operation, not the stream's idle
                                // timers, which can remain queued after cancel.
                                while (!done) {
                                    ioc.restart();
                                    ioc.run_for(STOP_POLL_INTERVAL);
                                }
                            }
                            if (!result)
                                result = m_stop_requested.load() ? net::error::operation_aborted : net::error::timed_out;
                            break;
                        }
                    }
                    if (result)
                        throw beast::system_error(result);
                };
                tcp::resolver::results_type addresses;
                await_io([&](auto done) {
                    resolver.async_resolve(endpoint.host, endpoint.port,
                        [&, done](beast::error_code error, tcp::resolver::results_type resolved) {
                            addresses = std::move(resolved);
                            done(error);
                        });
                }, CONNECT_TIMEOUT);
                await_io([&](auto done) {
                    ws.next_layer().async_connect(addresses,
                        [done](beast::error_code error, const tcp::endpoint &) { done(error); });
                }, CONNECT_TIMEOUT);
                ws.set_option(websocket::stream_base::timeout {
                    CONNECT_TIMEOUT, 2 * KEEPALIVE_INTERVAL, true});
                ws.set_option(websocket::stream_base::decorator([&api_key](websocket::request_type &request) {
                    request.set(beast::http::field::user_agent, "MagpieSlicer/2.5.0");
                    if (!api_key.empty())
                        request.set("X-Api-Key", api_key);
                }));
                await_io([&](auto done) { ws.async_handshake(endpoint.host_header, endpoint.target, done); }, CONNECT_TIMEOUT);
                ws.text(true);
                for (const std::string &message : {make_identify_request().dump(), make_camera_request("camera.start_monitor", 2).dump()})
                    await_io([&](auto done) {
                        ws.async_write(net::buffer(message), [done](beast::error_code error, size_t) { done(error); });
                    }, CONNECT_TIMEOUT);

                retry_count = 0;
                notify(State::Active);
                while (!m_stop_requested.load()) {
                    beast::flat_buffer buffer;
                    await_io([&](auto done) {
                        ws.async_read(buffer, [done](beast::error_code error, size_t) { done(error); });
                    }, std::chrono::steady_clock::duration::zero(), true);
                }
                cancel_io();
                break;
            } catch (const std::exception &e) {
                if (m_stop_requested.load())
                    break;
                ++retry_count;
                BOOST_LOG_TRIVIAL(warning) << "Snapmaker camera session reconnecting: " << e.what();
                notify(State::Reconnecting, e.what());
                const auto retry_delay = std::min(MAX_RETRY_DELAY, std::chrono::seconds(retry_count));
                if (wait_for_stop(std::chrono::duration_cast<std::chrono::milliseconds>(retry_delay)))
                    break;
            }
        }
        notify(State::Stopped);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Snapmaker camera session failed: " << e.what();
        notify(State::Error, e.what());
    }
    m_running.store(false);
}

} // namespace GUI
} // namespace Slic3r
