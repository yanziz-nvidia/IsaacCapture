// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <oxr_utils/os_time.hpp>
#include <remote_link/frame_header.hpp>
#include <remote_link/messages.hpp>
#include <remote_link/reject.hpp>
#include <remote_link/tcp_transport.hpp>
#include <remote_link/websocket_transport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace core;
using namespace remote_link_test;

namespace
{

const std::string kRefusal = "busy: another robot app is connected to this Teleop Session Server";

//! Accepts one connection on its own thread and refuses it.
class Rejecter
{
public:
    explicit Rejecter(bool websocket) : m_websocket(websocket)
    {
        m_thread = std::jthread(
            [this]
            {
                if (const std::optional<Listener::Accepted> accepted = m_listener.accept(kWait))
                {
                    reject_remote_connection(accepted->fd, m_websocket, kRefusal);
                }
            });
    }

    RemoteAddress address() const
    {
        return parse_remote(std::string(m_websocket ? "ws://" : "tcp://") +
                            "127.0.0.1:" + std::to_string(m_listener.port()));
    }

private:
    bool m_websocket;
    Listener m_listener{ Endpoint{ "127.0.0.1", 0 } };
    std::jthread m_thread;
};

// The refusal waits up to 1 s for a HELLO first.
constexpr std::chrono::milliseconds kRefusalWait{ 5000 };

void expect_refusal(ITransport& client)
{
    std::vector<uint8_t> message;
    REQUIRE(client.receive(message, kRefusalWait) == ReceiveResult::Ok);
    const FrameHeader header = decode_header(message.data());
    REQUIRE(header.type == static_cast<uint8_t>(MessageType::Error));
    REQUIRE(std::string(message.begin() + kHeaderSize, message.end()) == kRefusal);
    REQUIRE(client.receive(message, kWait) == ReceiveResult::Closed);
}

} // namespace

TEST_CASE("RemoteLink: reject_remote_connection answers HELLO with ERROR, then end of stream", "[remote_link][unit]")
{
    const bool websocket = GENERATE(false, true);
    Rejecter rejecter(websocket);
    const std::unique_ptr<ITransport> client = connect_transport(rejecter.address(), kWait);
    std::vector<uint8_t> hello = encode_message(static_cast<uint8_t>(MessageType::Hello), {});
    stamp_header(hello.data(), 0, os_monotonic_now_ns());
    REQUIRE(client->send(hello.data(), hello.size()));
    expect_refusal(*client);
    client->shutdown();
}

TEST_CASE("RemoteLink: reject_remote_connection refuses a peer that sends nothing", "[remote_link][unit]")
{
    Rejecter rejecter(false);
    const std::unique_ptr<ITransport> client = connect_transport(rejecter.address(), kWait);
    expect_refusal(*client);
}
