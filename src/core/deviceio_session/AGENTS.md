<!--
SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Agent notes — `deviceio_session`

**CRITICAL (non-optional):** Before editing this package, complete the mandatory **`AGENTS.md` preflight** in [`../../../AGENTS.md`](../../../AGENTS.md) (read every applicable `AGENTS.md` on your paths, not just this file).

## Update loop

- **`DeviceIOSession::update`** reads the clock once with **`core::os_monotonic_now_ns()`** (via `#include <oxr_utils/os_time.hpp>`) and passes that value to **`ITrackerImpl::update(int64_t)`** for every registered impl.
- **No** session-owned **`XrTimeConverter`** is required solely to drive that loop (OpenXR conversion stays inside live impls).

## Implementation / includes

- **`deviceio_session.cpp`**: if the TU uses **`XR_NULL_HANDLE`** or other OpenXR macros, include **`<openxr/openxr.h>`** explicitly after the session header so **`XR_NO_PROTOTYPES`** is already established by **`oxr_utils/oxr_funcs.hpp`** pulled in through **`deviceio_session.hpp`**.
- **`mcap_impl.cpp`** is the sole TU that defines **`MCAP_IMPLEMENTATION`** and includes **`<mcap/writer.hpp>`** and **`<mcap/reader.hpp>`**. All other TUs (`deviceio_session.cpp`, `replay_session.cpp`) get declarations only.

## Replay session

- **`ReplaySession`** lives in `replay_session.hpp` / `replay_session.cpp` alongside `DeviceIOSession`. Uses **`McapReplayConfig`** (not `McapRecordingConfig`).
- Factory method is **`ReplaySession::run(const McapReplayConfig&)`**.

## Remote session

- **`RemoteSession`** reads a live record stream through the replay factory's stream mode.
  Whatever receives the stream feeds the shared `McapRecordStreamReader`; the session only reads
  it. `update()` never waits: it publishes the newest frame, or nothing once that frame is older
  than `max_frame_age_ns`.
- Frames are ordered by the sender's `frame_seq`, never by capture time; the capture time, mapped
  to this machine's clock, only gives the frame's age (`frame_age_ns()`) and staleness.
- **`RemoteSession` refuses `MessageChannelTracker`** at construction (see
  [`../replay_trackers/AGENTS.md`](../replay_trackers/AGENTS.md)). Its trackers' viewers subscribe
  to their topics on the stream, and `subscribed_topics()` is what the receiver asks the sender for.
- **`RemoteSessionClient`** (robot) feeds the stream from its Link's receive thread; **`RemoteSessionHost`**
  (server) records a `DeviceIOSession` into an `McapRecordStreamWriter` and sends one FRAME per `tick()`. A channel
  is declared (SETUP) with its first record, so `tick()` queues new SETUP before the FRAME. The host closes
  gracefully so a final ERROR reaches the robot, and the client reports the server's ERROR text in preference to any
  later link failure.
- **`McapRecordingConfig::stream`** makes `DeviceIOSession` record into an `mcap::IWritable`,
  unchunked (`McapRecordStreamWriter::writer_options()`), instead of a file. The writable must
  outlive the session, because the writer writes the end of the recording when it is destroyed.

## Related docs

- Tracker interface contract: [`../deviceio_base/AGENTS.md`](../deviceio_base/AGENTS.md)
- Live factory + impls: [`../live_trackers/AGENTS.md`](../live_trackers/AGENTS.md)
- Replay factory + impls: [`../replay_trackers/AGENTS.md`](../replay_trackers/AGENTS.md)
