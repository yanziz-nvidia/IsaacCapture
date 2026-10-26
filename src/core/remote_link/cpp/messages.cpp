// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "inc/remote_link/messages.hpp"

#include "byte_order.hpp"
#include "inc/remote_link/frame_header.hpp"

#include <algorithm>
#include <string>

namespace core
{

using remote_link_detail::read_le;
using remote_link_detail::write_le;

const char* to_string(MessageType type)
{
    switch (type)
    {
    case MessageType::Hello:
        return "HELLO";
    case MessageType::HelloAck:
        return "HELLO_ACK";
    case MessageType::Heartbeat:
        return "HEARTBEAT";
    case MessageType::Error:
        return "ERROR";
    case MessageType::Ping:
        return "PING";
    case MessageType::Pong:
        return "PONG";
    case MessageType::Status:
        return "STATUS";
    case MessageType::Setup:
        return "SETUP";
    case MessageType::Frame:
        return "FRAME";
    case MessageType::Ready:
        return "READY";
    }
    return "UNKNOWN";
}

std::vector<uint8_t> encode_pong(const PongPayload& pong)
{
    std::vector<uint8_t> out(kPongPayloadSize);
    write_le<int64_t>(out.data(), pong.t1_ns);
    write_le<int64_t>(out.data() + 8, pong.t2_ns);
    return out;
}

PongPayload decode_pong(std::span<const uint8_t> payload)
{
    if (payload.size() != kPongPayloadSize)
    {
        throw FrameDecodeError("remote link: PONG payload of " + std::to_string(payload.size()) + " bytes");
    }
    return PongPayload{ read_le<int64_t>(payload.data()), read_le<int64_t>(payload.data() + 8) };
}

void encode_frame_prefix(const FramePrefix& prefix, uint8_t* out16)
{
    write_le<uint64_t>(out16, prefix.frame_seq);
    write_le<int64_t>(out16 + 8, prefix.capture_time_ns);
}

std::vector<uint8_t> encode_frame_payload(const FramePrefix& prefix, std::span<const uint8_t> records)
{
    std::vector<uint8_t> out(kFramePrefixSize + records.size());
    encode_frame_prefix(prefix, out.data());
    std::copy(records.begin(), records.end(), out.begin() + kFramePrefixSize);
    return out;
}

FramePrefix decode_frame_prefix(std::span<const uint8_t> payload)
{
    if (payload.size() < kFramePrefixSize)
    {
        throw FrameDecodeError("remote link: FRAME payload of " + std::to_string(payload.size()) +
                               " bytes is shorter than its prefix");
    }
    return FramePrefix{ read_le<uint64_t>(payload.data()), read_le<int64_t>(payload.data() + 8) };
}

} // namespace core
