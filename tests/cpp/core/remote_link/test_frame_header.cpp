// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <remote_link/frame_header.hpp>
#include <remote_link/messages.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace core;

TEST_CASE("RemoteLink: header round-trips and has the documented layout", "[remote_link][unit]")
{
    FrameHeader header;
    header.type = 34;
    header.channel = 0x0102;
    header.seq = 0x03040506;
    header.payload_size = 0x00070809;
    header.t_send_ns = -1234567890123;
    std::array<uint8_t, kHeaderSize> bytes{};
    encode_header(header, bytes.data());

    const std::array<uint8_t, 8> prefix{ 0x52, 0x54, 0x53, 0x30, 34, 0, 0x02, 0x01 }; // "RTS0", type, version, channel
    REQUIRE(std::equal(prefix.begin(), prefix.end(), bytes.begin()));
    REQUIRE(bytes[8] == 0x06);
    REQUIRE(bytes[12] == 0x09);

    const FrameHeader decoded = decode_header(bytes.data());
    REQUIRE(decoded.type == 34);
    REQUIRE(decoded.channel == 0x0102);
    REQUIRE(decoded.seq == 0x03040506);
    REQUIRE(decoded.payload_size == 0x00070809);
    REQUIRE(decoded.t_send_ns == -1234567890123);
}

TEST_CASE("RemoteLink: decode_header rejects bad magic, version and size only", "[remote_link][unit]")
{
    std::array<uint8_t, kHeaderSize> bytes{};
    FrameHeader header;
    header.type = 250; // unknown types are not an error
    encode_header(header, bytes.data());
    REQUIRE(decode_header(bytes.data()).type == 250);

    SECTION("magic")
    {
        bytes[0] ^= 0xFF;
        REQUIRE_THROWS_AS(decode_header(bytes.data()), FrameDecodeError);
    }
    SECTION("version")
    {
        bytes[5] = 1;
        REQUIRE_THROWS_AS(decode_header(bytes.data()), FrameDecodeError);
    }
    SECTION("payload size")
    {
        header.payload_size = kMaxPayloadSize;
        encode_header(header, bytes.data());
        REQUIRE(decode_header(bytes.data()).payload_size == kMaxPayloadSize);
        header.payload_size = kMaxPayloadSize + 1;
        encode_header(header, bytes.data());
        REQUIRE_THROWS_AS(decode_header(bytes.data()), FrameDecodeError);
    }
}

TEST_CASE("RemoteLink: encode_message and stamp_header", "[remote_link][unit]")
{
    const std::vector<uint8_t> payload{ 1, 2, 3 };
    std::vector<uint8_t> message = encode_message(static_cast<uint8_t>(MessageType::Status), payload);
    REQUIRE(message.size() == kHeaderSize + 3);
    FrameHeader header = decode_header(message.data());
    REQUIRE(header.type == static_cast<uint8_t>(MessageType::Status));
    REQUIRE(header.seq == 0);
    REQUIRE(header.t_send_ns == 0);
    REQUIRE(header.payload_size == 3);
    REQUIRE(std::vector<uint8_t>(message.begin() + kHeaderSize, message.end()) == payload);

    stamp_header(message.data(), 77, 123456789);
    header = decode_header(message.data());
    REQUIRE(header.seq == 77);
    REQUIRE(header.t_send_ns == 123456789);
    REQUIRE(header.payload_size == 3);

    REQUIRE(encode_message(11, {}).size() == kHeaderSize);
    const std::vector<uint8_t> too_big(kMaxPayloadSize + 1);
    REQUIRE_THROWS_AS(encode_message(34, too_big), std::invalid_argument);
}

TEST_CASE("RemoteLink: PONG and FRAME payload codecs", "[remote_link][unit]")
{
    const std::vector<uint8_t> pong = encode_pong(PongPayload{ -5, 1'000'000'000'000 });
    REQUIRE(pong.size() == kPongPayloadSize);
    const PongPayload decoded = decode_pong(pong);
    REQUIRE(decoded.t1_ns == -5);
    REQUIRE(decoded.t2_ns == 1'000'000'000'000);
    REQUIRE_THROWS_AS(decode_pong(std::vector<uint8_t>(15)), FrameDecodeError);
    REQUIRE_THROWS_AS(decode_pong(std::vector<uint8_t>(17)), FrameDecodeError);

    const std::vector<uint8_t> records{ 9, 8, 7 };
    const std::vector<uint8_t> frame = encode_frame_payload(FramePrefix{ 0xFFFF'FFFF'0000'0001ull, 42 }, records);
    REQUIRE(frame.size() == kFramePrefixSize + records.size());
    const FramePrefix prefix = decode_frame_prefix(frame);
    REQUIRE(prefix.frame_seq == 0xFFFF'FFFF'0000'0001ull);
    REQUIRE(prefix.capture_time_ns == 42);
    REQUIRE(std::vector<uint8_t>(frame.begin() + kFramePrefixSize, frame.end()) == records);
    REQUIRE(decode_frame_prefix(std::vector<uint8_t>(kFramePrefixSize)).frame_seq == 0);
    REQUIRE_THROWS_AS(decode_frame_prefix(std::vector<uint8_t>(kFramePrefixSize - 1)), FrameDecodeError);
}

TEST_CASE("RemoteLink: message type ids and names", "[remote_link][unit]")
{
    REQUIRE(static_cast<int>(MessageType::Hello) == 1);
    REQUIRE(static_cast<int>(MessageType::HelloAck) == 2);
    REQUIRE(static_cast<int>(MessageType::Heartbeat) == 11);
    REQUIRE(static_cast<int>(MessageType::Error) == 12);
    REQUIRE(static_cast<int>(MessageType::Ping) == 13);
    REQUIRE(static_cast<int>(MessageType::Pong) == 14);
    REQUIRE(static_cast<int>(MessageType::Status) == 32);
    REQUIRE(static_cast<int>(MessageType::Setup) == 33);
    REQUIRE(static_cast<int>(MessageType::Frame) == 34);
    REQUIRE(static_cast<int>(MessageType::Ready) == 35);
    REQUIRE(std::string(to_string(MessageType::Frame)) == "FRAME");
    REQUIRE(std::string(to_string(static_cast<MessageType>(200))) == "UNKNOWN");
}
