// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Integration tests for RemoteSession: record through McapTrackerChannels into a record stream,
// as a live DeviceIOSession does when it records into one, feed the stream to a RemoteSession,
// and read the data back through the typed tracker facades.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <deviceio_session/remote_session.hpp>
#include <deviceio_trackers/hand_tracker.hpp>
#include <deviceio_trackers/head_tracker.hpp>
#include <deviceio_trackers/joint_se3_pose_tracker.hpp>
#include <deviceio_trackers/message_channel_tracker.hpp>
#include <mcap/record_stream.hpp>
#include <mcap/recording_traits.hpp>
#include <mcap/tracker_channels.hpp>
#include <mcap/writer.hpp>
#include <oxr_utils/os_time.hpp>
#include <schema/hand_generated.h>
#include <schema/head_generated.h>
#include <schema/joint_se3_pose_generated.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using HeadChannels = core::McapTrackerChannels<core::HeadPoseRecord>;
using HandChannels = core::McapTrackerChannels<core::HandPoseRecord>;
using JointSe3PoseChannels = core::McapTrackerChannels<core::JointSe3PoseOutputRecord>;

template <typename Array>
std::vector<std::string> to_string_vec(const Array& names)
{
    return std::vector<std::string>(names.begin(), names.end());
}

//! The server half: records the way a DeviceIOSession recording into a stream does.
struct Server
{
    // Declared first so it outlives the writer, which writes the end of the recording on close.
    core::McapRecordStreamWriter out;
    mcap::McapWriter writer;

    Server()
    {
        writer.open(out, core::McapRecordStreamWriter::writer_options());
    }

    uint64_t next_frame_seq = 1;

    //! Hands one update's records to `stream`, as captured `age_ns` ago.
    void send(core::McapRecordStreamReader& stream, int64_t age_ns = 0)
    {
        stream.add_setup(out.take_setup());
        stream.add_frame(out.take_frame(), next_frame_seq++, core::os_monotonic_now_ns() - age_ns);
    }
};

core::DeviceDataTimestamp stamp()
{
    const int64_t now = core::os_monotonic_now_ns();
    return core::DeviceDataTimestamp(now, now, now);
}

void write_head(HeadChannels& ch, float x)
{
    core::HeadPoseT data;
    data.is_valid = true;
    data.pose = std::make_shared<core::Pose>(core::Point(x, 0.0f, 0.0f), core::Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    ch.write(0, core::pack_record<core::HeadPoseRecord>(&data, stamp()));
}

//! The tracked channel only, which is all replay and remote read of a pusher-fed tracker.
void write_joint_se3_pose(JointSe3PoseChannels& ch, float x)
{
    core::JointSe3PoseOutputT data;
    data.type = core::JointType_HAND_RAW;
    data.joints.emplace_back(core::JointName_HAND_RAW_THUMB_TIP,
                             core::Pose(core::Point(x, 0.0f, 0.0f), core::Quaternion(0.0f, 0.0f, 0.0f, 1.0f)));
    ch.write(1, core::pack_record<core::JointSe3PoseOutputRecord>(&data, stamp()));
}

} // namespace

TEST_CASE("RemoteSession: head and hands round-trip through the stream", "[remote][session]")
{
    Server server;
    HeadChannels head_ch(server.writer, "0", to_string_vec(core::HeadRecordingTraits::recording_channels));
    HandChannels hand_ch(server.writer, "1", to_string_vec(core::HandRecordingTraits::recording_channels));

    core::HeadTracker head;
    core::HandTracker hands;
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "0" }, { &hands, "1" } } });

    session->update();
    CHECK_FALSE(session->is_fresh());
    CHECK_FALSE(head.get_head(*session));

    write_head(head_ch, 1.5f);
    core::HandPoseT left;
    hand_ch.write(0, core::pack_record<core::HandPoseRecord>(&left, stamp()));
    // An inactive right hand is a record with no payload.
    hand_ch.write(1, core::pack_record<core::HandPoseRecord>(nullptr, stamp()));
    server.send(*stream);

    session->update();
    CHECK(session->is_fresh());
    const auto& pose = head.get_head(*session);
    REQUIRE(pose);
    CHECK(pose->pose()->position().x() == 1.5f);
    CHECK(hands.get_left_hand(*session));
    CHECK_FALSE(hands.get_right_hand(*session));
}

TEST_CASE("RemoteSession: every update sees the newest frame", "[remote][session]")
{
    Server server;
    HeadChannels head_ch(server.writer, "0", to_string_vec(core::HeadRecordingTraits::recording_channels));
    core::HeadTracker head;
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "0" } } });

    write_head(head_ch, 1.0f);
    server.send(*stream);
    write_head(head_ch, 2.0f);
    server.send(*stream);

    session->update();
    CHECK(head.get_head(*session)->pose()->position().x() == 2.0f);

    // No new frame: the newest one is published again while it is fresh.
    session->update();
    CHECK(head.get_head(*session)->pose()->position().x() == 2.0f);
}

TEST_CASE("RemoteSession: a pusher-fed tracker keeps its last sample until a new one arrives", "[remote][session]")
{
    Server server;
    HeadChannels head_ch(server.writer, "0", to_string_vec(core::HeadRecordingTraits::recording_channels));
    JointSe3PoseChannels tips_ch(
        server.writer, "1", to_string_vec(core::JointSe3PoseRecordingTraits::recording_channels));

    core::HeadTracker head;
    core::JointSe3PoseTracker tips("manus_sensors_left");
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "0" }, { &tips, "1" } } });

    write_head(head_ch, 1.0f);
    write_joint_se3_pose(tips_ch, 3.0f);
    server.send(*stream);
    session->update();
    REQUIRE(tips.get_data(*session));

    // The next update brings head data but no new tips sample, as when the plugin pushed none.
    write_head(head_ch, 2.0f);
    server.send(*stream);
    session->update();
    const auto& kept = tips.get_data(*session);
    REQUIRE(kept);
    CHECK(kept->joints()->Get(0)->pose().position().x() == 3.0f);
}

TEST_CASE("RemoteSession: a stale stream reports no data", "[remote][session]")
{
    Server server;
    HeadChannels head_ch(server.writer, "0", to_string_vec(core::HeadRecordingTraits::recording_channels));
    core::HeadTracker head;
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "0" } }, 250'000'000 });

    write_head(head_ch, 1.0f);
    server.send(*stream, 1'000'000'000);
    session->update();
    CHECK_FALSE(session->is_fresh());
    CHECK_FALSE(head.get_head(*session));

    write_head(head_ch, 2.0f);
    server.send(*stream);
    session->update();
    CHECK(session->is_fresh());
    REQUIRE(head.get_head(*session));
}

TEST_CASE("RemoteSession: rejects a config without a stream", "[remote][session]")
{
    core::HeadTracker head;
    CHECK_THROWS_AS(core::RemoteSession::run({ nullptr, { { &head, "0" } } }), std::invalid_argument);
}

TEST_CASE("RemoteSession: trackers subscribe to the topics they read", "[remote][session]")
{
    core::HeadTracker head;
    core::HandTracker hands;
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "1" }, { &hands, "0" } } });

    CHECK(stream->subscribed_topics() == std::vector<std::string>{ "0/left_hand", "0/right_hand", "1/head" });
}

TEST_CASE("RemoteSession: an incompatible declared schema fails the first update, before any record", "[remote][session]")
{
    Server server;
    // "0/head" declared with the hand record's schema.
    HandChannels wrong(server.writer, "0", { "head" });
    wrong.write(0, core::pack_record<core::HandPoseRecord>(nullptr, stamp()));

    core::HeadTracker head;
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "0" } } });

    // Only the setup arrives; no frame does.
    stream->add_setup(server.out.take_setup());
    CHECK_THROWS_WITH(session->update(), Catch::Matchers::ContainsSubstring("MCAP schema mismatch on '0/head'"));
    CHECK_FALSE(session->is_fresh());
}

TEST_CASE("RemoteSession: rejects a MessageChannelTracker before subscribing anything", "[remote][session]")
{
    core::HeadTracker head;
    core::MessageChannelTracker channel(std::array<uint8_t, core::MessageChannelTracker::CHANNEL_UUID_SIZE>{});
    auto stream = std::make_shared<core::McapRecordStreamReader>();

    CHECK_THROWS_MATCHES(core::RemoteSession::run({ stream, { { &head, "0" }, { &channel, "1" } } }),
                         std::invalid_argument,
                         Catch::Matchers::Message("MessageChannelTracker is not supported over a remote session"));
    CHECK(stream->subscribed_topics().empty());
}

TEST_CASE("RemoteSession: frame_age_ns is the published frame's age", "[remote][session]")
{
    constexpr int64_t kAge = 50'000'000;
    constexpr int64_t kMaxAge = 250'000'000;

    Server server;
    HeadChannels head_ch(server.writer, "0", to_string_vec(core::HeadRecordingTraits::recording_channels));
    core::HeadTracker head;
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "0" } }, kMaxAge });

    session->update();
    CHECK_FALSE(session->frame_age_ns().has_value());

    write_head(head_ch, 1.0f);
    server.send(*stream, kAge);
    session->update();
    REQUIRE(session->frame_age_ns().has_value());
    CHECK(*session->frame_age_ns() >= kAge);
    CHECK(*session->frame_age_ns() <= kMaxAge);

    // A newer frame whose mapped capture time is older still replaces it; by then it is stale.
    write_head(head_ch, 2.0f);
    server.send(*stream, 2 * kMaxAge);
    session->update();
    CHECK_FALSE(session->is_fresh());
    CHECK_FALSE(session->frame_age_ns().has_value());
    CHECK_FALSE(head.get_head(*session));

    write_head(head_ch, 3.0f);
    server.send(*stream);
    session->update();
    CHECK(session->is_fresh());
    REQUIRE(session->frame_age_ns().has_value());
    CHECK(*session->frame_age_ns() < kMaxAge);
    CHECK(head.get_head(*session)->pose()->position().x() == 3.0f);
}

TEST_CASE("RemoteSession: frames are ordered by frame_seq", "[remote][session]")
{
    Server server;
    HeadChannels head_ch(server.writer, "0", to_string_vec(core::HeadRecordingTraits::recording_channels));
    core::HeadTracker head;
    auto stream = std::make_shared<core::McapRecordStreamReader>();
    auto session = core::RemoteSession::run({ stream, { { &head, "0" } } });

    write_head(head_ch, 1.0f);
    stream->add_setup(server.out.take_setup());
    const auto first = server.out.take_frame();
    write_head(head_ch, 2.0f);
    const auto second = server.out.take_frame();

    const int64_t now = core::os_monotonic_now_ns();
    REQUIRE(stream->add_frame(second, 2, now - 10'000'000));
    // Captured later by the mapped clock, but an older frame.
    CHECK_FALSE(stream->add_frame(first, 1, now));
    session->update();
    CHECK(head.get_head(*session)->pose()->position().x() == 2.0f);
}
