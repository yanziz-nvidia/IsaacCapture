<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Remote Teleop Session: architecture

Status: draft for review. Base: `main`. Date: 2026-10-23.

## 1. Goal

Teleoperate a robot when the operator (headset and gloves), the robot, and the GPU machine
running CloudXR are at three different sites.

- The robot app runs at the robot site and keeps calling `session.step()` with its own
  retargeting pipeline. The network stays hidden inside `TeleopSession`.
- Changes stay small: one config change in the robot app, and one program to start on the
  cloud machine.

Out of scope: video and audio from the robot; how the robot app commands the robot (unchanged
and local to the robot site); the gaps in remote device plugins (see section 10).

## 2. Terms

| Term | Meaning |
|---|---|
| Cloud machine | The GPU server that runs CloudXR. |
| CloudXR runtime | The process on the cloud machine that implements OpenXR. Headset data and all device data live here. |
| Device plugin | A program that reads one device and writes its data into the CloudXR runtime, e.g. `manus_hand_plugin`. |
| Tracker | A DeviceIO object that reads one kind of device data (head, hands, controllers, one plugin's data) for `TeleopSession`. |
| Robot app | The program at the robot site that calls `session.step()`, runs its retargeting graph, and commands the robot. |
| Teleop Remote Session | `TeleopSession` in the new remote mode, inside the robot app. Gets tracker data over the network instead of from a local CloudXR runtime. |
| Teleop Session Server | New program on the cloud machine ("Isaac Capture Core" in the overview picture). Reads tracker data from the CloudXR runtime for each connected robot app, and owns plugins on the cloud machine. |
| Main process / session process | The Teleop Session Server's two process types: one main process, and one session process per connected robot app. |
| Frame | Everything one `update()` of a session process read: one sample per tracker, sent as one message. |
| Sink data | Data the robot app's sink nodes write back toward devices, e.g. haptic commands. |
| MCAP | The file format Isaac Capture uses to record and replay tracker data, with one named stream ("channel") per tracker. Not used on the network. |
| Device Plugin Bridge | `remote_device_bridge`, on Jonghyun's branch, not on `main`. Lets device plugins on other machines write into the CloudXR runtime. |

## 3. Today on `main`

Everything runs on one machine:

```
                 ┌──────────────────────── one workstation ─────────────────────────┐
 headset ═══════►│ CloudXR service + CloudXR runtime                                │
                 │        ▲ write                    │ read                         │
                 │        │                          ▼                              │
 gloves ══USB═══►│ device plugin (manus_hand_plugin)  app: TeleopSession.step()     │
                 │                                    → retargeting → outputs ══════╪══► robot
                 └──────────────────────────────────────────────────────────────────┘
```

- The app owns the loop. `step()` reads OpenXR through trackers, runs the app's retargeting
  graph, and returns outputs. The app then sends them to the robot (ROS 2, USB, or in-process
  sim).
- The app launches the plugins listed in `TeleopSessionConfig.plugins` and health-checks them.
- Every process that reads or writes device data opens its own OpenXR session to the CloudXR
  runtime over a local socket, so all of them must run on that machine.

## 4. Proposed architecture

```
 OPERATOR SITE    │ CLOUD MACHINE                                                  │ ROBOT SITE
                  │                                                                │
 headset ═════════╪═══► CloudXR runtime                                            │
                  │          ▲                              │                      │
                  │    write │                              │ read tracker data    │
                  │          │                              ▼                      │
                  │ ┌─ device plugins ────────┐            Teleop Session Server   │
                  │ │ local plugins           │◄─launches─ main process            │
                  │ │                         │            session process ════════╪══► robot app
 mini PC: device ═╪═╪═► Device Plugin Bridge  │            (one per robot app) ◄═══╪═══ TeleopSession (remote mode)
 plugin (later)   │ │   (later)               │                                    │     step() → retargeting → outputs
                  │ └─────────────────────────┘                                    │                               │
                  │                                                                │                               ▼
                  │                                                                │                             robot

 ═══ network     ─── same machine
```

The Teleop Session Server does for the robot app what a local app does for itself today: it
reads tracker data from the CloudXR runtime and launches the plugins the app asks for. Robot
commands never pass through it.

**Who starts what:**

| Process | Started by |
|---|---|
| Teleop Session Server, main process | You or your deployment (systemd, a container entrypoint) |
| CloudXR service and runtime | The main process, if none is running; otherwise the main process attaches to the running one |
| Session process | The main process, one per connected robot app |
| Plugins on the cloud machine | The session process, as the robot app requests |
| Device plugins on the mini PC | The operator site |

## 5. Components

### 5.1 Robot app and Teleop Remote Session

The robot app changes one config block:

```python
config = TeleopSessionConfig(
    app_name="my_robot",
    pipeline=pipeline,                 # unchanged
    plugins=[PluginConfig(...)],       # unchanged; forwarded to the Teleop Session Server
    mode=SessionMode.REMOTE,           # new
    remote_endpoint="host:port",       # new
)
with TeleopSession(config) as session:
    while running:
        outputs = session.step(external_inputs=...)  # unchanged
        send_to_robot(outputs)                       # unchanged, local
```

- **Startup.** `__enter__` connects, sends HELLO, and returns when the server reports READY:
  plugins started and headset connected. A local `__enter__` returns at the same point today,
  because opening OpenXR waits for the headset. It waits up to 60 s by default (configurable;
  `None` waits without limit). On timeout it raises with the stage it was stuck at: server
  unreachable, a plugin failed to start, or waiting for headset.
- **Push, not pull.** The session process sends one frame per tick, at the rate the robot app
  sets in HELLO (90 Hz by default, configurable). `step()` never waits on the network: it uses the newest frame received and
  reports that frame's age alongside the existing `last_step_info` timing. A blocking pull
  mode, in which each step waits for a fresh frame, is added only if a lockstep user appears.
- **Stale data.** A frame older than 250 ms (default, configurable) is treated as missing. The default teleop state
  manager already stops teleop when its inputs disappear, so a network stall stops teleop the
  same way.
- **Plugin failures.** A plugin failure reported by the server makes `step()` raise, as a local
  plugin crash does today.
- **Not available in remote mode:** direct OpenXR access from the app (`session.oxr_session`)
  and rendering to the headset (Televiz).

### 5.2 Teleop Session Server

**Main process.** It:
- starts the CloudXR service if none is running, as apps do today through `CloudXRLauncher`;
- listens for robot apps and starts a session process for each; while one robot app is
  connected, it answers any other with ERROR (section 7);
- never opens OpenXR itself.

**Session process**, one per robot app (OpenXR allows one session per process). It:
1. checks the robot app's HELLO (protocol version, tracker descriptors, plugin names) and
   answers at once: HELLO_ACK, or ERROR with the reason;
2. launches the requested plugins with `PluginManager`, and reports status while it does;
3. opens an OpenXR session and creates the trackers with the live backend (`DeviceIOSession`),
   reporting "waiting for headset" until one connects;
4. reports READY, then on every tick calls `update()` and sends one frame;
5. (P1) writes sink data from the robot app into the CloudXR runtime;
6. keeps reporting plugin health.

Apart from the new network library and a way to read each tracker's latest sample as bytes
(section 9), it reuses pieces already on `main`: `DeviceIOSession` with the live trackers,
`PluginManager`, and `CloudXRLauncher`.

### 5.3 Device Plugin Bridge (later)

Not on `main` yet. When it lands, the Teleop Session Server launches it like any plugin; it
already ships as one. Device plugins on the mini PC then write into the CloudXR runtime
through it, and the server reads their data like any other tracker data.

## 6. Startup and one frame, end to end

Startup:
1. Cloud machine: start the Teleop Session Server. It starts CloudXR if needed, then listens.
2. Operator: connect the headset to CloudXR, at any time.
3. Robot app: enter `TeleopSession`. It sends HELLO; the server answers HELLO_ACK, starts
   plugins, waits for the headset, and reports READY.

Every frame:
1. Headset → CloudXR runtime: head, controller and hand tracking.
2. Device plugins → CloudXR runtime: device data, written locally, or through the Device
   Plugin Bridge later.
3. Session process: `update()` reads every tracker the robot app asked for.
4. Session process → network → Teleop Remote Session, which keeps the newest frame.
5. Robot app: `step()` takes the newest frame, runs retargeting, and returns outputs. The robot
   app commands the robot locally.
6. Back up (P1): sink data → network → session process → CloudXR runtime → device plugin.

## 7. Plugin ownership

| Plugin kind | Example | Teleop Session Server's role |
|---|---|---|
| Runs on the cloud machine, no local hardware | `controller_synthetic_hands`, a compute-heavy plugin, the Device Plugin Bridge | Launch, monitor, stop |
| Hardware at the operator site | Manus on the mini PC | Cannot launch. Sees it through the Device Plugin Bridge. |
| Hardware at the robot site | A camera on the robot | Cannot launch. Must also connect through the Device Plugin Bridge. |

Rules:
- **Requests by name only.** The robot app sends plugin name and arguments; the server looks
  the name up in its own installed plugins directory. `PluginConfig.search_paths` is ignored in
  remote mode. Accepting paths or commands over the network would let a remote machine run
  arbitrary programs on the cloud machine.
- **Lifetime.** Plugins start when the robot app connects and stop when it leaves, as they live
  and die with the app today. Once reconnect exists (P1), they survive short network drops, so
  gloves do not restart on every blip.
- **One robot app per server.** A second robot app gets an ERROR. `main` has no multi-app case
  either: two local apps that launch the same plugin already conflict, since the Manus SDK
  allows one connection and the runtime one pushed hand per side.

## 8. Network link

| Message | Direction | Delivery |
|---|---|---|
| HELLO: protocol version, app name, frame rate, tracker descriptors, plugin requests | robot app → server | reliable |
| HELLO_ACK (session id) or ERROR (reason) | server → robot app | reliable |
| STATUS: starting plugins, waiting for headset, READY, plugin failed | server → robot app | reliable |
| FRAME: sequence number, capture time, one sample per tracker | server → robot app | droppable |
| SINK_SAMPLE: sequence number, time, one sink's newest data | robot app → server | droppable |
| PING / PONG, HEARTBEAT | both | reliable |

- **Two delivery classes.** Reliable messages arrive in order and are never dropped. For
  droppable messages, only the newest one matters, and older unsent ones are replaced.
- **Swappable transport.** One transport interface carries both classes, chosen by the address
  scheme. TCP and WebSocket come first (both classes delivered reliably); LiveKit/WebRTC or QUIC
  can follow behind the same interface.
- **One frame per update.** `DeviceIOSession::update()` reads every tracker against one
  timestamp, so a frame is a consistent snapshot, exactly as locally today. Sending trackers in
  separate messages could mix updates, e.g. the left hand from update N with the head from
  N−1. A frame is about 3 KB (two hands at about 1 KB each, plus head, controllers and plugin
  data), or about 2 Mbit/s at 90 Hz. On a later UDP transport a frame spans a few packets and
  losing any one loses the frame; revisit at P2 if loss rates demand it.
- **Plugin data can still lag head data** inside one frame, because plugins write on their own
  schedule. That is true locally too. Each sample keeps its own device timestamp.
- **Known gap (P1):** when a plugin's data source disappears, recording writes nothing, so the
  robot app keeps that plugin's last sample until plugin health reports the failure. Locally the
  tracker empties at once. Closing the gap needs an "absent" record.
- **Tracker descriptors.** Locally, `TeleopSession` hands its tracker objects to
  `DeviceIOSession`, which tells them apart by object, not by name. Remote mode does the same:
  `TeleopSession` hands the same objects to `RemoteSession`, which numbers them and sends one
  descriptor per tracker in HELLO. A descriptor is the tracker's type (`get_name()`, e.g.
  `HandTracker`) and its constructor parameters (e.g. `collection_id="manus_sensors_left"`).
  The session process creates matching trackers, and every frame labels each sample by its
  number. The robot app names nothing new.
- **Clocks.** The Teleop Remote Session estimates the clock offset from PING/PONG round trips,
  using the fastest recent ones. It maps sample timestamps into the robot machine's monotonic
  clock, so tracker timestamps keep their meaning.

### 8.1 Jitter, late and out-of-order data

| Situation | Handling |
|---|---|
| Frames arrive unevenly (jitter) | `step()` uses the newest frame and reports its age. No jitter buffer, since buffering adds latency; smoothing, if needed, belongs in the retargeting graph. |
| A burst arrives after a stall | Keep only the newest frame and drop the rest, so old motion is never replayed. |
| An older frame arrives after a newer one (possible on UDP transports) | Discard any frame whose sequence number is not newer than the last one used. |
| Frames from an old connection after a reconnect | Rejected by session id: each HELLO_ACK starts a new session id and sequence. |
| The link stalls | Frame age passes the threshold, trackers report no data, and teleop stops (section 5.1). |
| The link is lost (nothing received for 3 s; each side sends a heartbeat every 500 ms when otherwise idle) | P0: the session ends. `step()` raises, as a local session does when the runtime dies, and the server stops the plugins. P1: the robot app reconnects, and the plugins keep running. |
| The sender falls behind | Its queue keeps only the newest frame, so no backlog builds up. |
| The clock offset drifts | Re-estimated continuously; ages use the latest estimate. |
| Sink data going up | Same rules: sequence numbers, newest wins. |

## 9. Where it plugs into `main`

| Piece | On `main` today | Change |
|---|---|---|
| Tracker backends | `LiveDeviceIOFactory` (OpenXR) and `ReplayDeviceIOFactory` (MCAP); sessions `DeviceIOSession` and `ReplaySession`, both implementing `ITrackerSession` | Add `RemoteSession`, which reads through the existing replay impls: `McapTrackerViewers` can read a live record stream instead of a file (done in M1) |
| Server-side recording | `DeviceIOSession` records to an MCAP file | Can also record into an in-memory record stream: one frame per `update()` (done in M1) |
| `TeleopSession` | `SessionMode.LIVE` / `REPLAY`, chosen in `__enter__` | Add `SessionMode.REMOTE`: no OpenXR session; open a `RemoteSession`; forward plugin requests |
| Plugin launching | `PluginManager`, driven by `TeleopSession` | In remote mode, driven by the session process instead |
| CloudXR startup | `CloudXRLauncher`, in apps | Also used by the server's main process |
| Transport, messages, clock sync | Not on `main` | New shared library under `src/core/` |
| Teleop Session Server program | None | New |

**Language** follows `main`'s split: Python orchestration over a C++ data path.
- **C++:**
  - the network layer, and frame encoding and decoding;
  - `RemoteSession` and the remote tracker implementations, since tracker objects find their
    implementation through the C++ `ITrackerSession` interface;
  - a server-side way to read each tracker's latest sample as FlatBuffer bytes. Live trackers
    already build these records, but write them only through an MCAP file writer, so this
    needs a small new hook (section 11).

  A C++ receive thread does not compete with the robot app's Python loop for the GIL.
- **Python:** the server's main and session processes, `SessionMode.REMOTE` in
  `TeleopSession`, plugin requests, and CloudXR startup. These reuse `CloudXRLauncher` and the
  existing bindings.

## 10. Proposed priorities

- **P0:** Teleop Session Server with one robot app; HELLO / STATUS / READY startup;
  `SessionMode.REMOTE` for head, hand and controller trackers and plugin data trackers; frames
  with sequence numbers and ages; TCP/WebSocket transport; clock sync; plugin launch by name
  with health reporting; stale data treated as missing.
- **P1:** sink data (haptics); reconnect after link loss; authentication and TLS on the robot
  link. The robot-side client must speak `wss://` itself; a gateway can end TLS on the server
  side.
- **P2:** LiveKit/WebRTC or QUIC transport; splitting frames across packets if UDP loss
  demands it; several robot apps at once.
- **P3:** Device Plugin Bridge gaps: TLS and authentication on the glove side, reconnect, the
  wrist round trip, remote haptics.

## 11. Decisions and open questions

Decided:
1. The program is named Teleop Session Server.
2. Push: the robot app sets the rate, and `step()` never blocks. A blocking pull mode waits
   for a lockstep user.
3. One frame per `update()`.
4. One robot app per server.
5. The robot link uses direct TCP/WebSocket without TLS in P0, as the Device Plugin Bridge
   does; TLS comes in P1. LiveKit vs direct is decided at P2.
6. We build our own small network layer in `src/core/` now, shaped like the Device Plugin
   Bridge's (transport interface, clock-offset estimator, newest-wins queues), and converge with
   it after it merges.
7. Trackers are identified as `main` identifies them: by object, numbered by `RemoteSession`
   and described by type and constructor parameters. MCAP names are not used.
8. `__enter__` waits up to 60 s for READY by default, and the timeout error names the stage.
9. Python orchestration over a C++ data path (section 9).

10. Stale-data threshold: 250 ms by default, the Device Plugin Bridge's maximum sample age.
11. Link-lost timing: a heartbeat every 500 ms when otherwise idle, and the link declared lost
    after 3 s, the Device Plugin Bridge's values.
12. Frame rate: 90 Hz by default, with a config override.

13. Tracker samples travel as the MCAP records recording already writes. The server's
    `DeviceIOSession` records into an in-memory `McapRecordStreamWriter` instead of a file. The
    robot's replay impls read them from an `McapRecordStreamReader`. No tracker needs new code on
    either side.

To settle during implementation: how each tracker type exposes its constructor parameters.
Generated trackers can take them from `trackers.toml`; head, hands and controllers are written
by hand.

## 12. Implementation milestones

- **M1, done:** record stream (`McapRecordStreamWriter` / `McapRecordStreamReader`), stream mode
  in `McapTrackerViewers` and the replay factory, `RemoteSession`, `DeviceIOSession` recording
  into a stream; C++ tests for each, end to end without OpenXR or a network.
- **M2:** network library in `src/core/`: transport interface (TCP, WebSocket), messages,
  heartbeats, clock offset.
- **M3:** Teleop Session Server program, `SessionMode.REMOTE` in `TeleopSession`, Python
  bindings and tests.
