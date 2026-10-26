<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Agent notes — `replay_trackers`

**CRITICAL (non-optional):** Before editing this package, complete the mandatory **`AGENTS.md` preflight** in [`../../../AGENTS.md`](../../../AGENTS.md) (read every applicable `AGENTS.md` on your paths, not just this file).

## Half this package is generated

Replay impls for trackers declared in
[`../deviceio_trackers/trackers.toml`](../deviceio_trackers/trackers.toml) are emitted into
`${CMAKE_BINARY_DIR}/generated/trackers/replay_trackers/`. Only the hand-written trackers
(`head`, `hand`, `controller`, `full_body`, `message_channel`, `haptic_command_reader`,
`TensorPushTracker`, `keyboard`) have `.cpp` files in this directory.

- `replay_deviceio_factory.{hpp,cpp}` stays hand-written but `#include`s generated `.inc` fragments
  for the manifest trackers' forward decls, try-create thunks, dispatch rows, and factory methods.
  **Do not** add rows for a manifest tracker by hand.
- The forward-decl fragment is shared with the live factory header — it is
  `generated_tracker_forward_decls.inc`, not a replay-specific one. Keep it direction-agnostic.

## Impls read through a `TrackerRecordSource`, not a file

Each impl takes a `TrackerRecordSource`: a recording file, or a live record stream
(`McapRecordStreamReader`) that `RemoteSession` reads through these same impls. Keep impl logic
source-agnostic: read only through `McapTrackerViewers::read()`, and do not open files or assume
end-of-file semantics inside an impl. In stream mode `read()` returns the newest record again
until a newer one arrives, and nothing once the stream is stale.

- **Stream-mode viewers subscribe** to their topics on the stream at construction; those
  subscriptions are the topics the sending side is asked to record. Read every channel on every
  `update()`, so a channel's schema is checked as soon as the stream declares it.
- **An impl that reads records until something changes cannot run on a stream**, which repeats its
  newest record and keeps one per topic: `ReplayMessageChannelTrackerImpl` would never return, so
  `RemoteSession` refuses `MessageChannelTracker`.
- **Metadata is read once, at construction**, before any frame has arrived, so a tracker with
  metadata reads none over a stream. No tracker in the manifest has metadata yet.

## Missing-data logging is warn-once, by design

A replay impl reaching EOF or a gap is called **every frame**. Log the "no data" message once per
gap behind a `warned_no_data_` flag and reset the flag when a record arrives — never write to
`std::cerr` unconditionally in `update()`. The generated impls do this; hand-written ones must too.
This unification is why the generated replay stack is not a byte-for-byte port of the older
per-tracker files.

## Related docs

- Manifest and generator rules: [`../codegen/AGENTS.md`](../codegen/AGENTS.md)
- Replay session lifecycle: [`../deviceio_session/AGENTS.md`](../deviceio_session/AGENTS.md)
- Live counterpart: [`../live_trackers/AGENTS.md`](../live_trackers/AGENTS.md)
