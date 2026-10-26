// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <remote_link/frame_header.hpp>
#include <remote_link/tcp_transport.hpp>
#include <remote_link/websocket_transport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace core;
using namespace remote_link_test;

TEST_CASE("RemoteLink: parse_endpoint", "[remote_link][unit]")
{
    Endpoint e = parse_endpoint("10.0.0.2:5000", 1);
    REQUIRE(e.host == "10.0.0.2");
    REQUIRE(e.port == 5000);

    e = parse_endpoint("robot.local", 47600);
    REQUIRE(e.host == "robot.local");
    REQUIRE(e.port == 47600);

    e = parse_endpoint("[::1]:47600", 1);
    REQUIRE(e.host == "::1");
    REQUIRE(e.port == 47600);

    e = parse_endpoint("[fe80::1]", 9);
    REQUIRE(e.host == "fe80::1");
    REQUIRE(e.port == 9);

    e = parse_endpoint("::1", 9);
    REQUIRE(e.host == "::1");
    REQUIRE(e.port == 9);

    e = parse_endpoint("127.0.0.1:0", 9);
    REQUIRE(e.port == 0);

    for (const char* bad : { "", ":5000", "host:", "host:x", "host:65536", "host:-1", "[::1", "[::1]x", "[]:1" })
    {
        INFO(bad);
        REQUIRE_THROWS_AS(parse_endpoint(bad, 1), std::invalid_argument);
    }
}

TEST_CASE("RemoteLink: parse_remote", "[remote_link][unit]")
{
    RemoteAddress r = parse_remote("10.0.0.2:5000");
    REQUIRE_FALSE(r.websocket);
    REQUIRE(r.endpoint.host == "10.0.0.2");
    REQUIRE(r.endpoint.port == 5000);

    r = parse_remote("tcp://server");
    REQUIRE_FALSE(r.websocket);
    REQUIRE(r.endpoint.host == "server");
    REQUIRE(r.endpoint.port == kDefaultTcpPort);
    REQUIRE(kDefaultTcpPort == 47600);

    r = parse_remote("server");
    REQUIRE(r.endpoint.port == 47600);

    r = parse_remote("ws://server");
    REQUIRE(r.websocket);
    REQUIRE(r.endpoint.port == kDefaultWebSocketPort);
    REQUIRE(kDefaultWebSocketPort == 47601);
    REQUIRE(r.path == "/");

    r = parse_remote("ws://server:8080/teleop/x");
    REQUIRE(r.websocket);
    REQUIRE(r.endpoint.host == "server");
    REQUIRE(r.endpoint.port == 8080);
    REQUIRE(r.path == "/teleop/x");

    r = parse_remote("tcp://[::1]:47600");
    REQUIRE(r.endpoint.host == "::1");
    REQUIRE(r.endpoint.port == 47600);

    r = parse_remote("ws://[::1]/p");
    REQUIRE(r.websocket);
    REQUIRE(r.endpoint.host == "::1");
    REQUIRE(r.endpoint.port == 47601);
    REQUIRE(r.path == "/p");

    try
    {
        parse_remote("wss://server");
        FAIL("wss:// must be rejected");
    }
    catch (const std::invalid_argument& e)
    {
        REQUIRE(std::string(e.what()) == "wss:// is not supported yet; terminate TLS in front of the server");
    }
    REQUIRE_THROWS_AS(parse_remote("http://server"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_remote("ws:///path"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_remote("tcp://server/path"), std::invalid_argument);
}

TEST_CASE("RemoteLink: TcpTransport frames messages over a stream", "[remote_link][unit]")
{
    auto [a_end, b_end] = socket_pair();
    TcpTransport a(std::move(a_end));
    TcpTransport b(std::move(b_end));

    const size_t message_size =
        GENERATE(kHeaderSize, kHeaderSize + 1, size_t{ 65536 }, size_t{ 1 } << 20, kHeaderSize + kMaxPayloadSize);
    const std::vector<uint8_t> sent = message_of_size(message_size);
    bool send_ok = false;
    std::thread sender(
        [&]
        {
            // Two messages back to back: the receiver must split them at the header's payload size.
            send_ok = a.send(sent.data(), sent.size()) && a.send(sent.data(), sent.size());
        });
    std::vector<uint8_t> received;
    REQUIRE(b.receive(received, kWait) == ReceiveResult::Ok);
    REQUIRE(received == sent);
    REQUIRE(b.receive(received, kWait) == ReceiveResult::Ok);
    REQUIRE(received == sent);
    sender.join();
    REQUIRE(send_ok);
    REQUIRE(b.receive(received, std::chrono::milliseconds{ 20 }) == ReceiveResult::Timeout);
}

TEST_CASE("RemoteLink: TcpTransport reports timeout, EOF, truncation and bad headers", "[remote_link][unit]")
{
    auto [raw, own_end] = socket_pair();
    TcpTransport transport(std::move(own_end));
    std::vector<uint8_t> message;

    SECTION("timeout before the first byte")
    {
        REQUIRE(transport.receive(message, std::chrono::milliseconds{ 20 }) == ReceiveResult::Timeout);
    }
    SECTION("peer EOF")
    {
        raw.shutdown();
        REQUIRE(transport.receive(message, kWait) == ReceiveResult::Closed);
    }
    SECTION("peer EOF in the middle of a payload")
    {
        const std::vector<uint8_t> full = message_of_size(kHeaderSize + 100);
        REQUIRE(raw.send_all(full.data(), kHeaderSize + 10));
        raw.shutdown();
        REQUIRE(transport.receive(message, kWait) == ReceiveResult::Closed);
    }
    SECTION("bad magic")
    {
        std::vector<uint8_t> bad = message_of_size(kHeaderSize);
        bad[0] = 'X';
        REQUIRE(raw.send_all(bad.data(), bad.size()));
        REQUIRE(transport.receive(message, kWait) == ReceiveResult::Malformed);
    }
    SECTION("oversized payload")
    {
        std::vector<uint8_t> bad = message_of_size(kHeaderSize);
        FrameHeader header = decode_header(bad.data());
        header.payload_size = kMaxPayloadSize + 1;
        encode_header(header, bad.data());
        REQUIRE(raw.send_all(bad.data(), bad.size()));
        REQUIRE(transport.receive(message, kWait) == ReceiveResult::Malformed);
    }
    SECTION("shutdown unblocks a waiting receive")
    {
        std::thread closer(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
                transport.shutdown();
            });
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(transport.receive(message, std::chrono::milliseconds{ 5000 }) == ReceiveResult::Closed);
        closer.join();
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{ 2000 });
        const std::vector<uint8_t> out = message_of_size(kHeaderSize);
        REQUIRE_FALSE(transport.send(out.data(), out.size()));
    }
}

TEST_CASE("RemoteLink: TcpSocket wait_writable, release and peer EOF on send", "[remote_link][unit]")
{
    auto [a, b] = socket_pair();
    REQUIRE(a.wait_writable(std::chrono::milliseconds{ 0 }));
    const int fd = b.release();
    REQUIRE_FALSE(b.valid());
    REQUIRE(fd >= 0);
    ::close(fd);
    const std::vector<uint8_t> out = message_of_size(kHeaderSize + 10);
    // The first send after the peer closed may still be accepted by the kernel; a later one fails (no SIGPIPE).
    bool failed = false;
    for (int i = 0; i < 10 && !failed; ++i)
    {
        failed = !a.send_all(out.data(), out.size());
    }
    REQUIRE(failed);
}

TEST_CASE("RemoteLink: Listener accepts a TCP connection on an ephemeral port", "[remote_link][unit]")
{
    Listener listener(Endpoint{ "127.0.0.1", 0 });
    REQUIRE(listener.port() != 0);
    REQUIRE_FALSE(listener.accept(std::chrono::milliseconds{ 10 }).has_value());

    std::unique_ptr<ITransport> client;
    std::thread connector(
        [&] { client = connect_transport(parse_remote("127.0.0.1:" + std::to_string(listener.port())), kWait); });
    std::optional<Listener::Accepted> accepted = listener.accept(kWait);
    connector.join();
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->fd >= 0);
    REQUIRE(accepted->peer.rfind("127.0.0.1:", 0) == 0);
    REQUIRE(client);

    std::unique_ptr<ITransport> server = adopt_transport(accepted->fd, false);
    REQUIRE(server->wait_ready(std::chrono::milliseconds{ 0 }) == ReceiveResult::Ok);
    const std::vector<uint8_t> sent = message_of_size(kHeaderSize + 300);
    std::vector<uint8_t> received;
    REQUIRE(client->send(sent.data(), sent.size()));
    REQUIRE(server->receive(received, kWait) == ReceiveResult::Ok);
    REQUIRE(received == sent);
    REQUIRE(server->send(sent.data(), sent.size()));
    REQUIRE(client->receive(received, kWait) == ReceiveResult::Ok);
    REQUIRE(received == sent);

    client->shutdown();
    REQUIRE(server->receive(received, kWait) == ReceiveResult::Closed);
}

TEST_CASE("RemoteLink: Listener binds IPv6 loopback when available", "[remote_link][unit]")
{
    std::optional<Listener> listener;
    try
    {
        listener.emplace(Endpoint{ "::1", 0 });
    }
    catch (const std::runtime_error&)
    {
        SKIP("no IPv6 loopback on this host");
    }
    std::unique_ptr<ITransport> client;
    std::thread connector(
        [&] { client = connect_transport(parse_remote("[::1]:" + std::to_string(listener->port())), kWait); });
    std::optional<Listener::Accepted> accepted = listener->accept(kWait);
    connector.join();
    REQUIRE(accepted.has_value());
    REQUIRE(client);
    ::close(accepted->fd);
}

TEST_CASE("RemoteLink: connect_transport fails with nothing listening", "[remote_link][unit]")
{
    uint16_t port = 0;
    {
        Listener listener(Endpoint{ "127.0.0.1", 0 });
        port = listener.port();
    }
    REQUIRE_THROWS_AS(connect_transport(parse_remote("127.0.0.1:" + std::to_string(port)), kWait), std::runtime_error);
}

TEST_CASE("RemoteLink: TcpTransport shutdown_write ends the peer's stream and keeps receiving", "[remote_link][unit]")
{
    auto [a_end, b_end] = socket_pair();
    TcpTransport a(std::move(a_end));
    TcpTransport b(std::move(b_end));
    const std::vector<uint8_t> sent = message_of_size(kHeaderSize + 100);
    std::vector<uint8_t> received;

    REQUIRE(a.send(sent.data(), sent.size()));
    a.shutdown_write();
    REQUIRE(b.receive(received, kWait) == ReceiveResult::Ok);
    REQUIRE(received == sent);
    REQUIRE(b.receive(received, kWait) == ReceiveResult::Closed);

    REQUIRE(b.send(sent.data(), sent.size()));
    REQUIRE(a.receive(received, kWait) == ReceiveResult::Ok);
    REQUIRE(received == sent);
    REQUIRE_FALSE(a.send(sent.data(), sent.size()));
}
