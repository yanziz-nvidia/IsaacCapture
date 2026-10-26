// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace core
{

//! Remote session message types. JSON payloads (HELLO, HELLO_ACK, STATUS, READY) are opaque UTF-8 here.
enum class MessageType : uint8_t
{
    Hello = 1, //!< robot -> server, JSON
    HelloAck = 2, //!< server -> robot, JSON
    Heartbeat = 11, //!< both, empty
    Error = 12, //!< server -> robot, UTF-8 text; ends the session
    Ping = 13, //!< robot -> server, empty; t1 = header t_send
    Pong = 14, //!< server -> robot, PongPayload; t3 = header t_send
    Status = 32, //!< server -> robot, JSON
    Setup = 33, //!< server -> robot, MCAP Schema and Channel records
    Frame = 34, //!< server -> robot, FramePrefix then MCAP Message records; latest-wins
    Ready = 35, //!< server -> robot, JSON
};

//! Name for logs; "UNKNOWN" for values outside the enum.
const char* to_string(MessageType type);

struct PongPayload
{
    int64_t t1_ns = 0; //!< the PING's header t_send
    int64_t t2_ns = 0; //!< server receive time of the PING
};

constexpr size_t kPongPayloadSize = 16;

std::vector<uint8_t> encode_pong(const PongPayload& pong);
//! Throws FrameDecodeError unless the payload is exactly kPongPayloadSize bytes.
PongPayload decode_pong(std::span<const uint8_t> payload);

struct FramePrefix
{
    uint64_t frame_seq = 0;
    int64_t capture_time_ns = 0; //!< server CLOCK_MONOTONIC
};

constexpr size_t kFramePrefixSize = 16;

void encode_frame_prefix(const FramePrefix& prefix, uint8_t* out16);
//! Prefix followed by `records`.
std::vector<uint8_t> encode_frame_payload(const FramePrefix& prefix, std::span<const uint8_t> records);
//! Throws FrameDecodeError when the payload is shorter than kFramePrefixSize; the records follow the prefix.
FramePrefix decode_frame_prefix(std::span<const uint8_t> payload);

} // namespace core
