// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Unit tests for the live record stream: McapRecordStreamWriter splits what an McapWriter emits
// into setup and frames, merge_frame_records folds two frames into one, McapRecordStreamReader
// keeps the newest record per topic, and McapTrackerViewers reads that through a stream source.

#include "mcap_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <mcap/record_stream.hpp>
#include <mcap/writer.hpp>
#include <schema/hand_generated.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace mcap_test;

constexpr int64_t kMaxAge = 250'000'000;

//! An McapWriter recording into a stream, opened the way DeviceIOSession opens one.
struct StreamRecorder
{
    // Declared first so it outlives the writer, which writes the end of the recording on close.
    core::McapRecordStreamWriter out;
    mcap::McapWriter writer;

    StreamRecorder()
    {
        writer.open(out, core::McapRecordStreamWriter::writer_options());
    }
};

core::Serialized<core::HeadPoseRecord> head_record(float x)
{
    core::HeadPoseT data;
    data.is_valid = true;
    data.pose = std::make_shared<core::Pose>(core::Point(x, 0.0f, 0.0f), core::Quaternion(0.0f, 0.0f, 0.0f, 1.0f));
    return core::pack_record<core::HeadPoseRecord>(&data, core::DeviceDataTimestamp(1, 1, 1));
}

float head_x(const std::vector<uint8_t>& bytes)
{
    return flatbuffers::GetRoot<core::HeadPoseRecord>(bytes.data())->data()->pose()->position().x();
}

} // namespace

TEST_CASE("McapRecordStreamWriter: setup is taken once, frames carry only messages", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });

    head.write(0, head_record(1.0f));
    const auto setup = rec.out.take_setup();
    const auto first = rec.out.take_frame();
    CHECK_FALSE(setup.empty());
    CHECK_FALSE(first.empty());

    head.write(0, head_record(2.0f));
    CHECK(rec.out.take_setup().empty());
    const auto second = rec.out.take_frame();
    CHECK_FALSE(second.empty());
    CHECK(rec.out.take_frame().empty());

    core::McapRecordStreamReader reader;
    reader.add_setup(setup);
    REQUIRE(reader.add_frame(first, 1, 100));
    REQUIRE(reader.add_frame(second, 2, 200));
    REQUIRE(reader.snapshot(200, kMaxAge));

    const auto latest = reader.latest("0/head");
    REQUIRE(latest.bytes);
    CHECK(head_x(*latest.bytes) == 2.0f);
    REQUIRE(reader.channel("0/head") != nullptr);
    CHECK(reader.channel("0/head")->message_encoding == "flatbuffer");
}

TEST_CASE("McapRecordStreamWriter: the topic filter drops other channels", "[mcap][record_stream]")
{
    StreamRecorder rec;
    rec.out.set_topic_filter({ "0/head" });
    HeadChannels kept(rec.writer, "0", { "head" });
    HeadChannels dropped(rec.writer, "1", { "head" });

    kept.write(0, head_record(1.0f));
    dropped.write(0, head_record(9.0f));

    core::McapRecordStreamReader reader;
    reader.add_setup(rec.out.take_setup());
    REQUIRE(reader.add_frame(rec.out.take_frame(), 1, 100));
    REQUIRE(reader.snapshot(100, kMaxAge));

    CHECK(reader.latest("0/head").bytes);
    CHECK_FALSE(reader.latest("1/head").bytes);
    CHECK(reader.channel("1/head") == nullptr);
    CHECK(reader.dropped_messages() == 0);
}

TEST_CASE("McapRecordStreamReader: a topic keeps its newest record across frames", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels a(rec.writer, "0", { "head" });
    HeadChannels b(rec.writer, "1", { "head" });

    core::McapRecordStreamReader reader;

    a.write(0, head_record(1.0f));
    b.write(0, head_record(10.0f));
    reader.add_setup(rec.out.take_setup());
    REQUIRE(reader.add_frame(rec.out.take_frame(), 1, 100));
    REQUIRE(reader.snapshot(100, kMaxAge));
    const uint64_t b_sequence = reader.latest("1/head").sequence;

    a.write(0, head_record(2.0f));
    REQUIRE(reader.add_frame(rec.out.take_frame(), 2, 200));
    REQUIRE(reader.snapshot(200, kMaxAge));

    CHECK(head_x(*reader.latest("0/head").bytes) == 2.0f);
    CHECK(head_x(*reader.latest("1/head").bytes) == 10.0f);
    CHECK(reader.latest("1/head").sequence == b_sequence);
}

TEST_CASE("McapRecordStreamReader: a stale stream publishes no records", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });
    core::McapRecordStreamReader reader;

    head.write(0, head_record(1.0f));
    reader.add_setup(rec.out.take_setup());
    REQUIRE(reader.add_frame(rec.out.take_frame(), 1, 1'000));

    CHECK_FALSE(reader.snapshot(1'000 + kMaxAge + 1, kMaxAge));
    CHECK_FALSE(reader.latest("0/head").bytes);
    CHECK_FALSE(reader.published_capture_time_ns().has_value());
    CHECK(reader.channel("0/head") != nullptr);

    head.write(0, head_record(2.0f));
    REQUIRE(reader.add_frame(rec.out.take_frame(), 2, 2'000 + kMaxAge));
    CHECK(reader.snapshot(2'000 + kMaxAge, kMaxAge));
    CHECK(head_x(*reader.latest("0/head").bytes) == 2.0f);
    CHECK(reader.published_capture_time_ns() == 2'000 + kMaxAge);
}

TEST_CASE("McapRecordStreamReader: a stream with no frame yet is stale", "[mcap][record_stream]")
{
    core::McapRecordStreamReader reader;
    CHECK_FALSE(reader.snapshot(1'000, kMaxAge));
    CHECK_FALSE(reader.newest_capture_time_ns().has_value());
    CHECK_FALSE(reader.published_capture_time_ns().has_value());
}

TEST_CASE("McapRecordStreamReader: orders frames by frame_seq, not by capture time", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });
    core::McapRecordStreamReader reader;

    head.write(0, head_record(1.0f));
    reader.add_setup(rec.out.take_setup());
    const auto first = rec.out.take_frame();
    head.write(0, head_record(2.0f));
    const auto second = rec.out.take_frame();
    head.write(0, head_record(3.0f));
    const auto third = rec.out.take_frame();

    REQUIRE(reader.add_frame(second, 2, 200));
    CHECK_FALSE(reader.add_frame(first, 1, 300));
    CHECK_FALSE(reader.add_frame(first, 2, 300));
    REQUIRE(reader.snapshot(200, kMaxAge));
    CHECK(head_x(*reader.latest("0/head").bytes) == 2.0f);

    // A newer frame whose mapped capture time is older, as after the clock offset estimate moves.
    REQUIRE(reader.add_frame(third, 3, 150));
    CHECK(reader.newest_capture_time_ns() == 150);
    REQUIRE(reader.snapshot(200, kMaxAge));
    CHECK(head_x(*reader.latest("0/head").bytes) == 3.0f);
    CHECK(reader.published_capture_time_ns() == 150);
    CHECK_FALSE(reader.snapshot(150 + kMaxAge + 1, kMaxAge));
}

TEST_CASE("McapRecordStreamReader: the first frame is taken whatever its frame_seq", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });
    core::McapRecordStreamReader reader;

    head.write(0, head_record(1.0f));
    reader.add_setup(rec.out.take_setup());
    REQUIRE(reader.add_frame(rec.out.take_frame(), 0, 100));
    CHECK_FALSE(reader.add_frame({}, 0, 200));
    CHECK(reader.add_frame({}, 1, 200));
}

TEST_CASE("McapRecordStreamReader: subscribed topics are sorted and unique", "[mcap][record_stream]")
{
    core::McapRecordStreamReader reader;
    CHECK(reader.subscribed_topics().empty());

    reader.subscribe("1/head");
    reader.subscribe("0/right_hand");
    reader.subscribe("1/head");
    reader.subscribe("0/left_hand");
    CHECK(reader.subscribed_topics() == std::vector<std::string>{ "0/left_hand", "0/right_hand", "1/head" });
}

TEST_CASE("McapRecordStreamReader: drops a message on an undeclared channel", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });
    head.write(0, head_record(1.0f));

    core::McapRecordStreamReader reader;
    REQUIRE(reader.add_frame(rec.out.take_frame(), 1, 100));
    REQUIRE(reader.snapshot(100, kMaxAge));
    CHECK(reader.dropped_messages() == 1);
    CHECK_FALSE(reader.latest("0/head").bytes);
}

TEST_CASE("McapRecordStreamReader: refuses bytes that end inside a record", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });
    head.write(0, head_record(1.0f));

    core::McapRecordStreamReader reader;
    auto setup = rec.out.take_setup();
    auto frame = rec.out.take_frame();
    reader.add_setup(setup);

    frame.pop_back();
    CHECK_THROWS_AS(reader.add_frame(frame, 1, 100), std::runtime_error);
    setup.pop_back();
    CHECK_THROWS_AS(reader.add_setup(setup), std::runtime_error);
}

TEST_CASE("McapTrackerViewers: a stream source reads the newest record until a newer one arrives",
          "[mcap][record_stream][tracker_viewers]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });
    core::McapRecordStreamReader reader;
    HeadViewers viewers(core::TrackerRecordSource(reader), "0", { "head" });

    CHECK_FALSE(viewers.read(0));

    head.write(0, head_record(1.0f));
    reader.add_setup(rec.out.take_setup());
    REQUIRE(reader.add_frame(rec.out.take_frame(), 1, 100));
    REQUIRE(reader.snapshot(100, kMaxAge));

    const auto first = viewers.read(0);
    REQUIRE(first);
    CHECK(first->data()->pose()->position().x() == 1.0f);
    CHECK(viewers.read(0).get() == first.get());

    head.write(0, head_record(2.0f));
    REQUIRE(reader.add_frame(rec.out.take_frame(), 2, 200));
    REQUIRE(reader.snapshot(200, kMaxAge));
    const auto second = viewers.read(0);
    REQUIRE(second);
    CHECK(second->data()->pose()->position().x() == 2.0f);

    CHECK_FALSE(reader.snapshot(200 + kMaxAge + 1, kMaxAge));
    CHECK_FALSE(viewers.read(0));
}

TEST_CASE("McapTrackerViewers: a stream source subscribes to the topics it reads",
          "[mcap][record_stream][tracker_viewers]")
{
    core::McapRecordStreamReader reader;
    HeadViewers first(core::TrackerRecordSource(reader), "1", { "head" });
    HeadViewers second(core::TrackerRecordSource(reader), "0", { "head", "other" });

    CHECK(reader.subscribed_topics() == std::vector<std::string>{ "0/head", "0/other", "1/head" });
}

TEST_CASE("McapTrackerViewers: a stream source checks a declared schema before any record arrives",
          "[mcap][record_stream][tracker_viewers]")
{
    StreamRecorder rec;
    // "0/head" declared with the hand record's schema, which a head viewer cannot read.
    core::McapTrackerChannels<core::HandPoseRecord> wrong(rec.writer, "0", { "head" });
    HeadChannels right(rec.writer, "1", { "head" });

    core::McapRecordStreamReader reader;
    HeadViewers wrong_viewers(core::TrackerRecordSource(reader), "0", { "head" });
    HeadViewers right_viewers(core::TrackerRecordSource(reader), "1", { "head" });

    // Nothing declared yet: nothing to check.
    CHECK_FALSE(wrong_viewers.read(0));

    // The writer declares a channel with its first message; only the declarations arrive.
    wrong.write(0, core::pack_record<core::HandPoseRecord>(nullptr, core::DeviceDataTimestamp(1, 1, 1)));
    right.write(0, head_record(1.0f));
    reader.add_setup(rec.out.take_setup());
    CHECK_FALSE(reader.snapshot(100, kMaxAge));
    CHECK_THROWS_WITH(wrong_viewers.read(0), Catch::Matchers::ContainsSubstring("MCAP schema mismatch on '0/head'"));
    CHECK_FALSE(right_viewers.read(0));
}

TEST_CASE("merge_frame_records: keeps the last record per channel", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels a(rec.writer, "0", { "head" });
    HeadChannels b(rec.writer, "1", { "head" });

    a.write(0, head_record(1.0f));
    b.write(0, head_record(10.0f));
    const auto setup = rec.out.take_setup();
    const auto older = rec.out.take_frame();

    a.write(0, head_record(2.0f));
    a.write(0, head_record(3.0f));
    const auto newer = rec.out.take_frame();

    const auto merged = core::merge_frame_records(older, newer);
    // One record per channel, each the size of the one it replaced.
    CHECK(merged.size() == older.size());

    core::McapRecordStreamReader reader;
    reader.add_setup(setup);
    REQUIRE(reader.add_frame(merged, 1, 100));
    REQUIRE(reader.snapshot(100, kMaxAge));
    CHECK(head_x(*reader.latest("0/head").bytes) == 3.0f);
    CHECK(head_x(*reader.latest("1/head").bytes) == 10.0f);

    CHECK(core::merge_frame_records(older, {}) == older);
    CHECK(core::merge_frame_records({}, older) == older);
    CHECK(core::merge_frame_records(newer, {}).size() == newer.size() / 2);
    CHECK(core::merge_frame_records({}, {}).empty());
}

TEST_CASE("merge_frame_records: refuses incomplete and non-Message records", "[mcap][record_stream]")
{
    StreamRecorder rec;
    HeadChannels head(rec.writer, "0", { "head" });
    head.write(0, head_record(1.0f));
    const auto setup = rec.out.take_setup();
    const auto frame = rec.out.take_frame();

    auto truncated = frame;
    truncated.pop_back();
    CHECK_THROWS_AS(core::merge_frame_records(truncated, frame), std::runtime_error);
    CHECK_THROWS_AS(core::merge_frame_records(frame, truncated), std::runtime_error);
    CHECK_THROWS_WITH(
        core::merge_frame_records(frame, setup), Catch::Matchers::ContainsSubstring("a record other than a Message"));
}
