<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Pose Provider — Standalone Plugin Design

Pose Provider is an Isaac Capture plugin that consumes images and optional IMU measurements
from the common camera interface and publishes a 6-DoF pose stream. Isaac Capture provides
the runtime; users add support for another estimation library by implementing `IPoseEngine`.
Camera acquisition and camera backend development belong to the upstream camera design.

**Status:** review draft for the standalone-plugin direction. It replaces the direct
camera-owning application architecture in [the existing
design](pose-provider-interface-design.md), which remains available for comparison. The
[camera interface][camera-design] is still under review, so this document specifies the
consumer contract without fixing its API names, metadata layout or transport. The Google Doc
has not yet been updated to this architecture.

## Goals

The goals are interchangeable engines, reliable sensor/pose correlation, useful recordings
for offline processing/debugging, and consistent lifecycle and coordinate-frame behavior.
Existing `se3_tracker` transport, recording, replay and consumer APIs remain the pose
output path.

## Initial release scope

The initial release uses one OAK-D stereo pair, optionally its onboard IMU, and one engine
per provider instance on the same host. The interface represents `Stereo` and `StereoImu`;
initial qualification targets one selected profile/mode. Each recording episode starts a
new provider run and map. Data collection prioritizes measurement correctness over minimum
latency. The engine boundary initially uses owned CPU images.

| Critical capability | Initial boundary |
|---|---|
| Launch and run | One standalone `PoseProvider` plugin managed by `PluginManager`, consuming one camera device |
| Estimate pose | One build-selected engine through `IPoseEngine`, with a stub for contract tests |
| Prepare valid input | Only the formats, calibration models and IMU preparation needed by the selected initial OAK-D profile |
| Publish usable output | One `se3_tracker` collection, preserved measurement times and one documented, stable map frame per run |
| Complete a recorded episode | Sensor data recorded once, setup/decision records, verified source/pose coverage and explicit failure/incomplete status |
| Force a fresh map | Operator-requested reset ends the current run/episode and starts a fresh pose-provider instance and map |
| Diagnose the run | Readiness, failure reason, essential loss/backpressure counters and a final summary |

A qualified profile is a tested camera configuration, input mode, engine build, calibration
and operating bounds. Support grows one profile/adapter at a time; listing a format, model
or reference engine does not commit to supporting every combination. Implement one engine
path first; further adapters can be qualified independently against the same contract.

Pose Provider owns setup resolution, input preparation, engine integration, pose publication,
and its local lifecycle. It writes small run metadata in recorded mode; the upstream camera
recorder owns sensor payloads. Camera backends, acquisition and transport stay upstream.
The application owns episode orchestration, coverage checks and tracker recording.

Correct calibration/timing, buffer ownership, bounded progress, stable frames and recording
completion remain required for this small path. Limiting supported configurations does not
weaken those guarantees. One concrete application/recorder integration is sufficient; a
general orchestration or recording framework is not part of this delivery.

## Deferred capabilities

- Additional devices/topologies, multi-source fusion, unrelated clocks and cross-host input.
- Runtime engine swapping, dynamic source switching and a stable adapter binary ABI.
- Direct GPU/zero-copy engine input and general-purpose preprocessing graphs.
- Map save/load, localization against saved maps, maps retained across takes, dynamic frame
  alignment and automatic camera/engine restart within a run.
- In-process engine reset and multiple maps within one recording episode.
- Recording full inputs after preprocessing for debugging, and generic replay tooling;
  see [offline use and repeatability](#offline-use-and-repeatability).
- Custom gravity-derived output alignment and its startup workflow.
- Calibration-estimation tools, offline re-estimation/trajectory refinement, rich telemetry
  dashboards and automatic performance tuning.

Recording the information needed by future offline tools remains in scope. Extensibility
comes from the engine contract and separate camera-consumption, preparation and output
responsibilities. Add a new interface or input model when a concrete next use case needs
it; these directions are not promises of unchanged APIs or prebuilt framework support.

## User model and extension point

NVIDIA authors the planned `PoseProvider` plugin, `IPoseEngine` contract and shared setup,
input-preparation and output code. Users configure that runtime or extend it through an
engine adapter; they do not need to implement their own pose-provider plugin.

| User need | Intended code authorship | Expected user action |
|---|---|---|
| Run a supported camera and engine | NVIDIA: pose plugin, supplied engine adapter and initial OAK-D integration | Configure and launch the provided plugins |
| Use another SLAM library | User or third party: `IPoseEngine` adapter | Build/link the NVIDIA-provided pose plugin with that adapter |
| Support another camera | NVIDIA, user or vendor: camera backend using the upstream camera interface | Use an existing backend or add one upstream, outside Pose Provider scope |
| Integrate into an application | NVIDIA: example application; user: application-specific orchestration | Reuse or adapt the example to launch plugins and coordinate recording readiness/completion |
| Force a fresh map | NVIDIA: reset action in the example application; user: optional application integration | Request a reset through application control; the pose plugin is relaunched with a fresh map |
| Use a device that already publishes pose | NVIDIA, user or vendor: existing pose-producing plugin | Consume its `se3_tracker` collection directly |

The camera interface and `CameraReader` belong to the upstream NVIDIA camera design.
Native SLAM libraries are separate dependencies with their own authors and licenses.
The proposed NVIDIA-authored ORB-SLAM3 and cuVSLAM adapters retain the qualification and
distribution constraints described in the concrete example below.

`IPoseEngine` is the only extension point in this design. One build links one adapter through
`make_pose_engine()`; runtime configuration does not load arbitrary libraries or silently
substitute another engine. This is an in-process C++ interface, not a stable binary ABI.

`PoseProvider` is an ordinary C++ runtime class in an executable plugin, analogous to
`ManusTracker` inside `manus_hand_plugin`. `PluginManager` discovers and manages that
executable. The runtime does not need a plugin base class, another host layer, or a mandatory
`DataCollectionCoordinator` class.

## Architecture and responsibilities

The camera plugin owns the device. `PoseProvider` owns its camera subscription, input
preparation, engine and output. The application launches the processes and coordinates
recording readiness, completion and operator-requested fresh-map resets. Recorded mode uses
the upstream camera recorder for original measurements, a small metadata log written by
`PoseProvider`, and `DeviceIOSession` for poses.

```text
Application
  +-- camera recording (peer consumer; hosting open)
  |     +-- CameraReader and sensor-data writer
  +-- DeviceIOSession (tracker consumers / optional pose recording)
  +-- finish_episode(...) (episode finalization)
  +-- PluginManager
        +-- camera plugin process
        |     +-- device backend and publisher
        +-- pose-provider plugin process
              +-- PoseProvider
                    +-- CameraReader
                    +-- PoseInputAdapter
                    +-- IPoseEngine implementation
                    +-- PoseOutput
                    +-- MCAP writer (run metadata only)
```

Connectors above show ownership and launch. `CameraReader` denotes the upstream consumer
client; it is not another plugin or a new interface defined here. Camera-recorder hosting
belongs to the upstream integration; this design requires no separate recorder plugin.
Resolved setup is immutable data held by `PoseProvider` and shared with its components.
`finish_episode(...)` is an application function proposed here, not an existing SDK API
or a coordinator framework.

| Responsibility | Owner | Input → output |
|---|---|---|
| Acquisition and publication | Upstream camera plugin | Device → metadata, images, IMU and source status |
| Camera consumption | `CameraReader` inside `PoseProvider` | Subscription → source measurements and buffer lifetime |
| Setup resolution | `resolve_pose_setup(...)`, called by `PoseProvider` | Source metadata + configuration/profile + engine capabilities → immutable setup |
| Input preparation | Internal `PoseInputAdapter` | Source measurements → owned, calibrated, aligned and ordered engine input |
| Estimation | `IPoseEngine` implementation | Prepared input → normalized poses and engine health |
| Output integration | `PoseOutput` | Engine poses → configured output frame and `se3_tracker` publication |
| Local lifecycle and health | `PoseProvider` | Start/finish/failure → component coordination and provider status |
| Sensor recording | Upstream MCAP / OAK-D ego recorder | Its camera subscription → lossless original images/IMU and source metadata |
| Run metadata | `PoseProvider`, using the existing MCAP writer | Setup, source references, preparation/admission decisions and output associations → small run log |
| Pose consumption and recording | Application-owned `DeviceIOSession` | Selected `se3_tracker` collections → live tracker APIs and optional tracker MCAP |
| Episode orchestration | Application startup/reset handlers and `finish_episode(...)` | Run identity, per-stream boundaries and all required recording outcomes → complete or incomplete episode |

`PoseInputAdapter` is shared internal code. It is not another user extension point. Setup
resolution starts as a function; a separate stateful setup service is unnecessary.

## Inputs, outputs and recording

Pose Provider takes one selected camera stream set and a device/engine profile. It emits
live poses and status. Recorded mode preserves original sensors once, plus the setup and
processing decisions needed for offline use; live-only operation requires no files. The
[extended OAK-D example](#extending-the-oak-d-example-with-native-hand-pose-trackers)
shows how existing pose producers and recording modules connect to these outputs.

### Camera input contract

The dependency is a consumer-facing camera interface, not a particular camera backend or
IPC implementation. The current draft's `CameraReader`/`CameraInfo` concepts fit this role;
the following contract should survive changes to their exact declarations.

| Input | What Pose Provider needs |
|---|---|
| Source description | Stable device/session identity; selected streams and stereo roles; delivered format, dimensions and shutter type |
| Images | A paired stereo measurement; each eye's source identity and measurement timestamps; pixel layout and explicit ownership/lifetime |
| IMU | Independently timestamped acceleration and angular velocity, source sequences, units/axes and factory-correction state |
| Calibration | Intrinsics for delivered images and available sensor-to-rig extrinsics; missing values remain explicitly unknown |
| Timing | Original device timestamps plus mapped common-clock sample times; timed event (exposure start/midpoint, frame end or unknown); no implied equivalence between unrelated clocks |
| Lifecycle/status | Readiness, known loss, session change, normal end and failure; ability to finish this subscription without stopping other consumers |

Images and IMU may arrive independently; receiving an image does not imply that its required
IMU is already available. Stereo pairing belongs upstream. IMU association for the selected
engine belongs in Pose Provider. Do not assume an IMU message contains fresh values for
both components, or that those components share a timestamp or acquisition sequence.

A buffer must remain valid until the consumer finishes reading it. The provider must be
able to release its interest after staging/preparation, without keeping the camera buffer
for the entire engine call. Memory location, completion signals and release operations are
upstream API details; Pose Provider must honor the agreed contract.

Recorded mode uses loss-aware delivery. Source identity is `(camera session,
stream/component, capture sequence)`; connection-local lease tokens are not measurement IDs.
The pose run logs source references, boundaries, gaps/exclusions and prepared-input
associations; the camera recorder stores payloads independently. Finalization verifies exact
reference coverage, not just matching readers' minimum/maximum sequences.
On normal finish, apply the [image/IMU cutoff](#normal-finish-cutoff),
then drain every selected API to its end marker; unsubscribed streams are not read, and a
timeout is not end-of-stream. Unexpected loss, reset or session change fails the recorded
run rather than reconnecting.

The camera design remains responsible for transport, device ownership and production
qualification. Pose Provider must not depend on draft-specific ring layouts, metadata
field names or a particular number of reader APIs.

### Live pose output

`PoseOutput` publishes each fresh estimate through `se3_tracker`. Only `Tracking` is valid;
other emitted states produce invalid samples whose pose values consumers must ignore.
Do not repeat old poses or synthesize invalid samples simply because no new estimate is
available.

Default output is `SlamWorld`. An optional fixed startup transform produces
`T_session_body = T_session_slamworld * T_slamworld_body`. The supplied transform must be
valid for the selected engine's startup convention and resolved before engine admission.
Validation checks its rigid form; physical alignment depends on the application/profile's
known start relationship. It stays immutable for the run. `Session` does not automatically
mean OpenXR session space; consumers requiring it need validated alignment.

Custom gravity-derived alignment and solver-gravity overrides are deferred; gravity alone
does not determine heading or origin.

Publication supplies the original image-anchor local/device sample times. The raw device
time passes through unchanged; the common-clock time traverses OpenXR conversions and is
not an exact identity key. Availability time is set by the receiving runtime when the pose
becomes available to consumers or recording. The camera recording retains the exact original
timestamps. Detailed state, group association and fatal errors stay outside the live schema.

The application allocates a fresh run identity and collection at launch; `pose/<instance_id>`
is an illustrative name. Publication, consumers, status and recording use the same resolved
identity. Collection naming and `PluginManager` prefix rules remain integration details.
Reject startup if any resolved collection ID, including its prefix, exceeds
`XR_MAX_TENSOR_IDENTIFIER_SIZE - 1` (255) bytes.

Native pose devices, such as VIVE trackers, publish directly to the same `se3_tracker`
consumer/recording path. They need no passthrough engine and are not Pose Provider inputs.
Shared schema does not imply a common frame or fusion; the concrete example below covers
the required frame alignment and consumer setup.

### Episode records and writer ownership

Record original sensor payloads once through the upstream MCAP / OAK-D ego recorder.
`PoseProvider` writes only a small run log using the existing MCAP writer. The log schema
and camera-recorder integration are new work; today's OAK executable is not this recorder.
Illustrative filenames, not a fixed packaging contract:

```text
episode_<id>/
  camera.mcap         upstream camera recorder: original images/IMU, once
  pose_metadata.mcap  PoseProvider: setup and processing metadata, no images
  poses.mcap          DeviceIOSession: selected pose streams
  episode.json        application: identities, artifact references and outcome
```

| Records | Required contents |
|---|---|
| Camera recording | Lossless delivered images/independent accel and gyro; original timestamps, source identities, layouts and `CameraInfo`/calibration |
| Run setup | Resolved setup and provenance; preparation implementation/settings; adapter/engine/build/asset identity, execution mode and relevant seeds/environment |
| Input decisions | Group/prepared-input IDs and fusion times; exact source references, IMU interpolation brackets and small aligned IMU values; total submission order, admission outcomes, exclusions and gap positions |
| Output associations/summary | Output-to-group association, tracking state, final successful publication count/raw anchor, termination reason and run outcome |
| Pose recording | Primary pose channels with validity and measurement/availability timestamps; existing tracked-state channels may repeat each update's final new sample |
| Episode manifest | Run/camera-session/collection identities, source frames, artifact references, actual per-stream boundaries and complete/incomplete outcome |

Original means camera-interface output before Pose Provider preparation. Record that data
losslessly; lossy viewing video cannot replace it. Extra streams such as RGB use the camera
recorder's selected-stream configuration and may produce additional artifacts, without
changing Pose Provider. A recorded pose links to its run-log group using run/collection plus
`sample_time_raw_device_clock`; the fixed [anchor contract](#input-preparation) makes that
key unique. Do not duplicate pose payloads in the run log.

The upstream recorder is a peer `CameraReader` consumer; the application owns its episode
and storage lifecycle. Hosting and API names remain upstream integration choices. It must
provide this contract:

| Guarantee | Required behavior |
|---|---|
| Readiness | Writer open, selected source session/streams known and loss-aware subscription active before pose admission; readiness does not wait for a durable first frame |
| Admission boundary | Report the first sequence observed and accepted for recording per selected stream/component; absent until observed. These are startup bounds, not proof of written coverage |
| Health | Promptly report known source loss, queue overflow or write failure; the application stops the recorded run |
| Finish | On application request, reach or report failure to reach each final required source/IMU sequence before detaching, then drain every selected API to `End` within the deadline |
| Finalization | Return one terminal success/failure result with finalized artifact identities, source session and exact per-stream/component written coverage; include write/flush/close failures and absent bounds for empty streams |

For a fresh camera start, establish the recorder subscription before acquisition. For an
already-running camera, use the [reset contract](#forced-fresh-map-reset) below. Acquisition
may run while pose admission waits for observed boundaries and pose-consumer readiness,
within the startup deadline. Exclude older retained input and any prepared group whose
required references precede those boundaries. Bounds remain absent only for streams with
no observed measurement; preserve partial-startup evidence on failure.
`PoseProvider` queues small input-decision records before submission and records each admission
outcome afterward. Its metadata buffering is bounded; loss/write failure makes the run
incomplete. It holds no image buffers for log writes. The camera recorder owns its own
reader/buffers; `IPoseEngine` has no recording dependency.

Sensor recording does not gate each engine admission. The recorder accumulates exact coverage
from successful payload writes, using disjoint sequence ranges per source stream/component
with holes preserved and duplicates counted only once. Pair this summary with the finalized
artifact identities and source session; it remains provisional until finalization succeeds.
Retain the coverage metadata with the episode artifacts.
Coverage metadata has finite bounds; missing, truncated or overflowed coverage makes the
episode incomplete.

The application's `finish_episode(...)` streams source IDs needed to reconstruct admitted
engine inputs from the run log and checks membership in the finalized coverage, including
initialization data, interpolation brackets and the required IMU tail. It reads metadata,
not image payloads. References logged only to explain gaps or excluded input do not require
payload coverage. Minimum/maximum bounds or counts alone cannot prove coverage; missing
required references leave the episode incomplete rather than silently trimming it to a
common range.

On finish/reset, keep camera recording active through the provider's image cutoff, required
IMU tail and source drain. `PoseProvider` tracks the highest required source sequence per
stream/component while recording associations and reports them with source-input closure.
The application then asks the camera recorder to finish through those targets before
detaching; missing targets or timeout leave the episode incomplete. Continue pose-session
updates through engine/output drain. The proposed core integration exposes `SchemaTracker`
receipt progress through `DeviceIOSession`: count all primary samples, including invalid
poses, retain the last raw anchor and latch duplicate/order/known-loss errors. Exclude
tracked-state copies. For the Pose Provider
collection, check order using raw anchors; time gaps alone are not losses. Native trackers
retain their own timestamp rules. The pose consumer is ready before engine admission.

Compare received count and final raw anchor with the provider's successful publication
boundary; both must match without errors. Zero output requires zero receipts and no final
anchor. Retain publication resources until acknowledgement or deadline failure. Receipt
permits publisher cleanup but does not prove storage completion.

`finish_episode(...)` confirms camera/run-log finalization and source coverage, and asks
`DeviceIOSession` to finalize pose recording with a terminal, idempotent result covering
write/flush/close failure. Only success for every configured artifact permits a complete
manifest. Current tracker APIs lack receipt progress and reliable finalization results;
agree these narrow additions with core owners. Missing data/metadata, timeout or writer
failure leaves the episode incomplete.

### Offline use and repeatability

Reconstruct inputs from original sensors plus recorded preparation settings/decisions.
To replay the live input sequence, preserve its accepted order and gap effects rather than
recreating queue pressure. To re-estimate with another engine/configuration, use the original
sensors and record new provenance; historical admission choices need not constrain that run.
No initial offline processing tool or generic replay API is promised.

A debug run may select an engine's documented repeatable synchronous/offline mode through
adapter configuration; the native work still runs behind the nonblocking engine interface.
Qualify reconstruction and repeatability for the selected implementation, settings, initial
state and execution environment. Record relevant seeds and execution mode. Async-to-sync
re-estimation need not reproduce the original live trajectory. Exact replay of IMU-gap
recovery needs adapter support; otherwise qualify a gap-free debug sequence.

Recording the full inputs after preprocessing could later help isolate preprocessing
differences from engine behavior, or replay inputs when the original preprocessing
implementation/environment is unavailable. It could also avoid costly repeated preprocessing.
This adds image storage and write bandwidth, so it remains deferred. Identical input bytes
still do not guarantee identical SLAM output.

### Status output

Live health and final summaries distinguish source loss, preparation exclusions, engine
backpressure and recording failure. `PoseProvider::status()` exposes lifecycle/readiness,
the latest emitted tracking state, the latest valid pose's original common-clock measurement
time, and the latest pose's common-clock publication time. Missing state/times remain absent
until their corresponding events occur. The application derives valid-pose age and output
silence separately; continued publication does not imply current measurements. Record applied
bounds and counters without blocking processing. Status transport and measured operator
thresholds remain integration choices; a new telemetry stream or dashboard is not required.

## Setup and calibration

`PoseProvider` calls `resolve_pose_setup(...)` after reading source metadata and before
engine admission. Inputs are source metadata, user configuration, a qualified device/engine
profile and the selected engine's capabilities/identity. The result is one immutable
`ResolvedPoseSetup`, consumed by input preparation, the engine and pose output, and retained
in the episode records.

It records selected sources and prepared format/rates/mode; applied camera/IMU calibration
and correction state; frames/conventions; anchor, clock and preparation policies/bounds;
engine configuration/build identity; and provenance/overrides. Common fields are
engine-neutral. Engine-specific settings remain an opaque recorded payload interpreted by
the adapter. Original camera metadata is retained separately; neither its wire format nor
backend types enter `IPoseEngine`.

Generic validation checks the resolved input/calibration against engine capabilities before
engine-specific validation. Missing required data, unsupported modes or contradictory
settings stop startup. Native engine configuration cannot independently override canonical
geometry or time correction.

The profile declares accepted shutter types and timed acquisition events. The initial
OAK-D stereo profile requires qualified global-shutter streams; rolling-shutter models are
deferred. Unknown shutter/event metadata rejects startup unless explicit profile evidence
resolves its meaning and is recorded. Never invent an exposure midpoint or equate arrival
time with measurement time.

Calibration describes the actual pixels entering the engine: crop/resize changes intrinsics,
and rectification can change distortion and the optical frame. The camera supplies delivered
image calibration; any further geometric preparation must produce the corresponding applied
calibration. This is a correctness condition, not a requirement to implement arbitrary
crop/resize/rectification; prepare only what the selected profile needs. Pixel-format
conversion alone need not change the camera model.

`T_A_B` maps B into A. Transforms are rigid and right-handed, in meters, with unit Hamilton
`xyzw` quaternions; matrix storage is row-major. The `se3_tracker` schema requires these units,
handedness and quaternion convention but leaves the reference frame to the producer; it
does not mandate +Z up or a compass heading.

| Frame | Defined by | Convention |
|---|---|---|
| `Body` | Device/profile configuration | The rigid frame being tracked; its origin and axes are explicit in the resolved calibration |
| `SlamWorld` | SLAM engine initialization, documented by the adapter | The map frame for this run, including any fixed conversion from native conventions |
| `Session` | Application configuration | An optional output frame with a known fixed transform from `SlamWorld` |

Each qualified adapter/profile documents how the world origin and XYZ axes are established,
whether gravity alignment is guaranteed, and what determines heading or leaves it arbitrary.
Setup records these conventions, not a claim that the actual startup pose is known before
estimation. Adapters normalize native pose conventions to the common representation above.

Resolve upstream sensor-to-rig geometry into `T_body_sensor = T_body_rig * T_rig_sensor`.
A left optical `Body` is a simple initial profile: +X right, +Y down and +Z forward. Another
physical frame or stereo midpoint requires known geometry. Selecting `Body` or applying
`T_body_native` changes the reported rigid frame, not the world origin or gravity alignment.
If an RGB camera is not delivered, its `Body` transform cannot be assumed present upstream.
The current camera draft defines the rig as the first stream's optical frame, left first
for stereo, so `T_body_rig` is identity for this left-optical `Body` profile.

Keep frame-group overrides atomic: `Body` plus both camera extrinsics and, when configured,
the IMU extrinsic. An IMU-only extrinsic override is unsupported. Noise and timing overrides
are independent. Preserve value provenance; a qualified board/profile fallback is not
per-device measured calibration.

The [canonical calibration reference](#canonical-calibration) defines model tags, coefficient
ordering and permitted normalization. Qualify only the models needed by the initial profile;
reject unsupported input. Fisheye support is deferred.

For `StereoImu`, add the qualified noise/random-walk and timing profile that camera metadata
does not supply. Preserve factory-correction state separately from online residual-bias
estimation. Unknown correction state needs explicit qualification or startup rejection.
On the selected common time basis, `t_camera_basis = t_imu_basis + imu_offset_ns` defines
the offset sign. Apply it once to derived fusion time; never overwrite original timestamps
or repeat the correction in native engine settings. An offset does not map unrelated clocks.

## Input preparation

`PoseInputAdapter` prepares memory and pixels, associates IMU, and produces one ordered
engine-input sequence. The engine adapter translates that sequence to native calls; it does
not redo source pairing, clock mapping or calibration resolution.

**Memory.** The engine receives owned CPU images. Stage GPU inputs into bounded host storage
and release camera buffers only after the final read/copy completes. Qualify the copy
strategy and its latency; direct GPU engine input remains deferred.

Validate format-specific stride, plane layout and buffer size before native engine ingestion;
malformed buffers are fatal input errors. [Canonical CPU layouts](#canonical-cpu-image-layouts)
define the prepared formats. Dimensions/format/stride must match resolved setup. Staging is
bounded; pressure excludes a whole stereo group with a recorded reason. Never drop only one
eye or release a buffer while an asynchronous read is pending.

Support only the profile's qualified delivered/prepared formats. Preserve pixel/color
semantics through any conversion, including grayscale scaling and `NV12` matrix/range;
layout compatibility is insufficient. Reject unqualified formats at startup. The canonical
reference defines representation, not a requirement to implement every conversion.

**Identity and timing.** Preserve each source measurement's identity and original timestamps.
Use the source identities from the camera contract and run-unique prepared-input IDs.
Record exact source associations, including IMU interpolation brackets; this provenance
layout stays outside the engine interface.

An image group selects one actual source image event as its anchor; engine poses carry its
original measurement times into `PoseOutput`. Derived IMU has a fusion time and source
associations, not a fabricated shared raw timestamp/counter for acceleration and angular
velocity.

Select and qualify a fusion clock before implementation of the input path. Mapped common-clock
sample time is the proposed default; a proven shared raw clock is an alternative for the
initial OAK-D integration. Camera session identity alone does not establish clock equivalence.
The initial anchor uses one fixed stream/event and source session for the run, with usable
original local/device sample times. Its raw device times must be nonnegative and fit the
existing signed 64-bit timestamp field, with strictly increasing values for distinct
admitted anchors. Reject an incompatible profile; a clock reset/wrap or session change
fails the run. Choosing the eye/event and supporting missing timestamps more broadly remain
integration decisions.

**IMU alignment and progress.** A qualified profile specifies the target IMU timeline,
interpolation rule, maximum bracket interval, initial/image coverage, and finite look-ahead
and lateness bounds. Do not repeat stale partners, assume coverage from image arrival, or
interpolate across known gaps without a qualified policy. Unresolved required-IMU loss
fails the initial recorded run.

Use a nonnegative fusion-time window `W` and finite positive reorder capacity `N`. Normally,
prepared items at or below `max_seen_time - W` may advance. When admitting an on-time item
would exceed capacity, emit the earliest item from the buffer plus incoming item, even
inside the window, and count a forced emission. This bypasses the time watermark only;
required preparation and IMU coverage must already be satisfied. Resolve an earlier pending
input, explicitly exclude it under the recorded policy, or fail before advancing past it.
Reaching an alignment/coverage bound without progress fails the run; buffers cannot grow
or wait indefinitely to satisfy missing coverage.

Order by fusion time, with IMU before images on ties and stable source order for remaining
ties. Validate monotonicity per source; source time regression fails the run. Recognized
duplicate images and inputs arriving behind committed order are excluded and recorded;
loss of required IMU coverage fails the run. Submitted image-anchor times increase strictly.
Windowing alone is not a wall-clock latency bound when input stops. Normal finish drains
selected sources using the cutoff below. Record actual bounds, duplicates, late exclusions,
pressure, forced emissions and gaps separately from engine backpressure.

### Normal-finish cutoff

`PoseProvider` fixes the last candidate image-group identity when finish is requested.
Stop selecting new images for the engine, but continue reading/releasing image sets while
collecting required IMU within the qualified coverage/deadline bounds. Log post-cut exclusions;
allowing the image queue to fill could fail the IMU subscription too. Resolve pending groups
in order, then detach the reader and drain selected APIs to `End`. The camera recorder remains
active through this boundary and preserves the source/IMU tail.

The recorded finish policy may exclude uncovered trailing groups that have never been
admitted to the engine. Required IMU missing for already-admitted input, known source loss
or an unresolved interior gap still fails the run. Flushing the reorder tail must not admit
a group without its required coverage. Complete in-flight submissions before calling
`finish_input()`; reader stop drains accepted data but cannot acquire future IMU.

## Engine interface — `IPoseEngine`

`IPoseEngine` consumes resolved setup and prepared input, with bounded asynchronous
admission, output polling and drain. Normal lifecycle is `Uninitialized` → `Initialized` →
`Running` → `Draining` → `Stopped`; abort/partial-startup cleanup may stop earlier.

The input/output records contain only what the engine boundary needs:

| Record | Content |
|---|---|
| `EngineImageGroup` | Run-unique group ID; fusion time; original anchor measurement times; exactly the selected stereo images with source stream IDs, dimensions, format, stride and owned CPU bytes |
| `EngineImuSample` | Run-unique prepared-input ID; fusion time; acceleration in m/s² and angular velocity in rad/s, both expressed in the IMU frame defined by resolved calibration |
| `EnginePose` | Originating group ID and anchor measurement times; `T_slamworld_body`; tracking state |

Source associations and original per-component IMU timestamps remain in provider/recording
records. Image stream ID identifies each eye, independent of vector position. No engine
input retains a borrowed camera pointer or upstream camera-specific type. Upstream IMU values
already have normalized units/axes. `PoseInputAdapter` applies only a profile-declared remap
if the delivered frame differs from the calibrated IMU frame, consistently with `T_body_imu`;
otherwise it preserves the values. Never repeat upstream normalization.

```cpp
enum class TrackingState { Initializing, Tracking, Lost, Relocalizing };
enum class EnqueueResult {
    Accepted, DroppedBackpressure, RejectedInvalidInput, RejectedNotRunning
};

class IPoseEngine {
public:
    virtual ~IPoseEngine() = default;
    virtual EngineCapabilities capabilities() const = 0;
    virtual EngineIdentity identity() const = 0;
    virtual std::string validate_configuration(const ResolvedPoseSetup&) const = 0;
    virtual void initialize(const ResolvedPoseSetup&) = 0;
    virtual void start() = 0;
    virtual EnqueueResult on_image_group(EngineImageGroup group) = 0;
    virtual EnqueueResult on_imu(const EngineImuSample& sample) = 0;
    virtual void finish_input() = 0;
    virtual bool input_drained() const = 0;
    virtual bool try_get_next_pose(EnginePose& out) = 0;
    virtual EngineHealth health() const = 0;
    virtual void stop() = 0;
};

std::unique_ptr<IPoseEngine> make_pose_engine();
```

These are proposed declarations. Capabilities advertise supported input modes, pixel formats
and distortion models without preference ordering or approximation. Identity gives the exact
adapter/library version. Both are available before initialization. Empty validation text
means valid; other results prevent startup. Each new engine instance initializes fresh
estimation state and an empty map; it must not automatically restore a saved map.

One serialized producer submits input, and one consumer polls output/health. Lifecycle calls
are serialized by `PoseProvider`. The engine preserves combined image/IMU admission order.
Accepted image buffers transfer to the engine; accepted IMU values are copied before return.
Rejected input is never processed. Admission does not block on processing or output.
If a native image call needs later IMU coverage, it must not block the only execution path
able to supply that IMU. Qualify the adapter's buffering and native call schedule accordingly.

Engine queues have finite recorded bounds. Image pressure rejects a whole group. Every IMU
drop is counted; consecutive drops before the next accepted IMU form one gap event, delivered
internally before that sample is processed. Each adapter needs qualified gap recovery or
must fail. Upstream/preparation loss cannot silently use this internal queue-gap mechanism.
A full pose queue stalls processing instead of dropping estimates; count each entry into
that condition once, while input admission stays non-blocking.

`on_imu()` in `Stereo` mode is invalid. `RejectedInvalidInput` fails the run.
`RejectedNotRunning` is expected only during abort/partial-startup cleanup; otherwise it is
a lifecycle failure. `EngineHealth` retains cumulative image/IMU drops, IMU gaps, invalid
and not-running rejections, output stalls and a sticky fatal-error flag. Health is safe and
non-blocking for the documented output consumer; counters never reset within a run.

The adapter correlates native results with the actual accepted image group. The live stream
contains at most one estimate per group, in accepted image-group order; it need not produce
an estimate for every group. Historical revisions/refined trajectories are outside this
stream. Qualify bounded native/result handling; duplicate or out-of-order estimates at the
engine interface fail the run. `try_get_next_pose()` removes one estimate without blocking.

The adapter declares its native tracked frame and resolves a fixed `T_body_native`, including
any rectification rotation, before processing. Unknown or ambiguous geometry fails startup.
It returns `T_slamworld_body = T_slamworld_native * inverse(T_body_native)`.

The engine establishes one fixed `SlamWorld` per run by the first `Tracking` result. Remain
`Initializing` while it establishes that frame, including native startup gravity alignment
when supported by the selected mode. Stereo-only input has no guaranteed gravity reference
without external information. Relocalization must return to the same frame. Otherwise report
`Lost` while recoverable or raise a fatal error; failure is not another tracking state.
New maps/resets/re-anchoring require a new run.
The application provides the [forced fresh-map reset](#forced-fresh-map-reset) action below.
Loop correction may change poses within the fixed frame, but cannot redefine that frame.

`finish_input()` is idempotent and ends admission. `input_drained()` means all accepted input
is processed and no new estimates can be generated; queued output still needs draining.
Continue output polling during drain to avoid deadlock. `stop()` is idempotent, supports
partial startup/abort, wakes blocked workers, and must not require a functioning output
consumer. Qualify bounded shutdown for native calls that cannot be interrupted.

## Runtime and lifecycle

`PoseProvider` owns local coordination and engine lifecycle. Its executable calls `update()`
from a control loop, following the Manus/OAK/VIVE pattern; `PluginManager` manages the process.
The declarations below illustrate ownership, without prescribing worker or control transport
implementations.

```cpp
class PoseProvider {
public:
    PoseProvider(ProviderConfig config, std::unique_ptr<IPoseEngine> engine);
    void update();
    void request_finish();
    bool finished() const;
    void abort();
    ProviderStatus status() const;

private:
    ProviderConfig config_;
    std::unique_ptr<CameraReader> reader_;
    std::optional<ResolvedPoseSetup> setup_;
    std::unique_ptr<PoseInputAdapter> input_;
    std::unique_ptr<IPoseEngine> engine_;
    std::unique_ptr<PoseOutput> output_;
    std::unique_ptr<mcap::McapWriter> run_log_;  // Metadata only; absent in live-only mode.
};
```

The control thread serializes runtime calls. Idempotent `request_finish()` starts drain or
ends incomplete startup. `finished()` means local drain/run-log finalization and
cleanup succeeded, or failure cleanup finished; the application still confirms episode
completion. Destruction also cleans up partial startup.

| Stage | `PoseProvider` responsibility |
|---|---|
| Prepare | Wait within a startup deadline for endpoint/metadata; subscribe, resolve setup, initialize engine/output and optional run log; report readiness or failure |
| Start | Wait for application start; recorded mode also requires camera-recorder/pose-consumer readiness and excludes pre-boundary input before engine admission |
| Run | Prepare/admit input, drain/publish estimates and report health |
| Finish input | Apply the image/IMU cutoff, detach and drain selected APIs, complete submissions, then call engine `finish_input()` |
| Drain output | Publish until `input_drained()` and the output queue is empty; stop engine, finalize run log and report final output boundary/status |
| Complete handoff | In recorded mode, retain publication until the application acknowledges final-pose receipt or the deadline fails; then clean up locally |
| Abort | Stop new work, wake workers, safely release resources and preserve the original failure |

Each `update()` performs bounded control work; long native calls run in workers. Reader,
staging and log writes cannot block output drain or abort. Keep one output consumer,
bounded queues and interruptible idle waits; qualify cadence and latency rather than
inheriting a device plugin's 90 Hz loop. Metadata-queue timeout/overflow fails recorded mode.

The application uses a bidirectional control channel for identity, semantic readiness,
admission start, finish/abort, status and the recording completion handshake. Its transport
is an integration choice; a running process alone is not ready. Normal finish uses this
channel and captures the terminal process result before releasing the handle.
`Plugin::stop()` has a fixed two-second escalation deadline and can overwrite exit details;
it is a termination fallback, not evidence of graceful completion. Signal handlers only
request control-loop work. Source, engine, storage or termination failure leaves the episode
incomplete; no automatic restart occurs within a run.

### Forced fresh-map reset

The example application's `handle_fresh_map_reset()` implements **Reset pose estimation**
as normal finish → confirmed process exit → normal start of a fresh pose process. It reuses
the current adapter/configuration and the completion rules above; `IPoseEngine` needs no
reset method. The application may remain running. Both live and recorded modes use a new
run, map and collection; recorded mode also starts a new episode. The camera may keep
acquiring if the upstream recorder can establish a qualified new episode boundary while
it runs; otherwise the application restarts the camera through normal startup. Restarting
the camera interrupts all its consumers, including display and streaming readers, and creates
a new source session. Applications requiring continuous camera streaming must qualify the
continuing-camera episode boundary; otherwise recorded reset is unavailable in that
configuration. Live-only reset needs no recorder boundary or camera restart.

Record the reset reason and coalesce repeat requests. A finish timeout invokes abort or
termination and leaves the old episode incomplete; poor tracking alone does not. A fresh
run may follow a failed episode once cleanup and new readiness succeed. If exit or startup
fails, report reset failure and leave the replacement inactive; do not retry automatically.

At run end, clear the application's current-pose selection and announce the ended/replacement
collections. Old samples and acknowledgements remain with the old run. Managed and external
consumers explicitly rebind; current `Se3Tracker` collection and `DeviceIOSession` membership
are fixed at construction, so rebuild affected sessions. Independent sessions and producer
plugins may continue. The
[extended example](#extending-the-oak-d-example-with-native-hand-pose-trackers)
shows the effect on a shared recording.

The new map has no assumed alignment with the old map. Re-establish `T_session_slamworld`
before readiness when `Session` output is requested; valid poses resume only with new
`Tracking` estimates. Qualify camera reconnect/stop semantics and record source boundaries
using the normal cutoff policy. Reset ends a run; it does not preserve a map or guarantee
uninterrupted recording.

## Concrete example: OAK-D + ORB-SLAM3

The application launches an OAK camera plugin exposing the common camera contract and a pose
plugin built with an ORB-SLAM3 adapter. ORB-SLAM3 is a visual/visual-inertial SLAM library;
its library calls stay inside the adapter. The camera process owns the only device pipeline.
The pose process consumes its stereo/optional IMU stream without opening OAK hardware.

```text
camera_plugin_oak --> common camera interface
                              |
                              v
pose_provider_plugin: CameraReader
                              |
                              v
                      PoseInputAdapter
                              |
                              v
                      IPoseEngine implementation
                      (ORB-SLAM3 adapter + library)
                              |
                              v
                      PoseOutput --> se3_tracker
```

This requires the upstream OAK integration to expose the agreed interface; the existing
executable name does not mean that dependency is already implemented. The initial profile
selects streams, `Body`/output frames, pixel preparation, qualified IMU data and measured
bounds.
`StereoImu` must not silently fall back to `Stereo`. Every episode starts a fresh map.

A high-level pose-plugin entry point shows engine selection and the control loop:

```cpp
int main(int argc, char** argv) {
    auto config = parse_provider_config(argc, argv);
    PoseProvider provider(config, make_pose_engine());

    while (!provider.finished()) {
        if (stop_requested()) {
            provider.request_finish();
        }
        provider.update();
    }

    return provider.status().failed ? 1 : 0;
}
```

This wiring sketch omits signal registration, exception handling and control transport.
`stop_requested()` reads a signal-handler flag; normal application finish uses the same
`request_finish()` path. Continue the loop through drain. `PoseProvider` handles engine
lifecycle; the application confirms recording completion independently of the process exit.

The application uses the existing launcher pattern:

```cpp
PluginManager plugins(plugin_search_paths);
auto camera = plugins.start("oak_camera", camera_root_id, camera_arguments);
auto pose = plugins.start("pose_provider_plugin", pose_root_id, provider_arguments);
```

`oak_camera` is the existing manifest name; `camera_plugin_oak` is its executable.
`pose_provider_plugin` is a proposed package/executable name. The application uses the
[lifecycle/reset contract](#runtime-and-lifecycle) above to coordinate these handles.
In recorded mode the application also starts camera recording through the upstream
integration, subscribing before fresh acquisition or applying the reset contract for a
continuing camera. Satisfy recorder readiness and boundary requirements before pose admission.
The sketch does not prescribe recorder hosting or its API.
A different engine uses the same runtime with another linked adapter.

Generic interfaces and examples contain no native SLAM headers or API symbols. Engine-specific
code belongs in its adapter package. The stub is the default contract-test build; ORB-SLAM3
is the functional reference and cuVSLAM the intended production target. Select the real
adapter required for initial qualification before closing the engine integration decision;
the stub cannot satisfy functional release criteria, and a second adapter is not a release
prerequisite. Neither reference is presumed implemented, conformant or approved for
distribution.

Target unmodified upstream ORB-SLAM3. Its adapter may translate native settings, batch prepared
IMU, schedule native calls and normalize results; camera ownership and shared preparation
stay outside it. The [OAK integration reference](pose-provider-backend-survey.md#orb-slam3)
also contains core patches, so its behavior does not establish stock-upstream compatibility.
Qualify public-API workarounds or narrow the supported profile instead of assuming a fork.
The [risks below](#risks-and-mitigations) cover compatibility and distribution approval.

### Extending the OAK-D example with native hand pose trackers

This optional composition example mounts the OAK-D on the head and adds two hand-mounted
6-DoF trackers, such as VIVE devices. Reuse the same camera plugin, ORB-SLAM3 pose plugin
and camera-recording/run-log path. The application adds the native tracker plugins and
consumes all three pose streams through one `DeviceIOSession`. Hand poses bypass Pose
Provider; the camera still supplies its only estimation inputs. These are rigid-body poses,
not hand skeletons. Delivering this expanded application is outside the initial one-camera
example.

```text
OAK camera --> PoseProvider --> head pose -------+
                   |                             |
                   v                             v
              pose_metadata.mcap          DeviceIOSession --> poses.mcap
                                                 ^
                                                 |
Left-hand tracker plugin --> left-hand pose -----+
Right-hand tracker plugin -> right-hand pose ----+

PoseProvider -- status/final boundaries --> application --> episode.json

OAK camera --> camera recorder --> camera.mcap
```

Live-only mode omits writers/files. The camera recording holds original stereo/IMU once;
`pose_metadata.mcap` contains setup and processing metadata. Optional RGB is another camera
recorder stream, with its artifacts/boundaries also included in `episode.json`.
The following uses existing tracker and recording APIs after the application has obtained
collection IDs and initialized the required OpenXR extensions/session handles:

```cpp
auto head = std::make_shared<core::Se3Tracker>(head_collection_id);
auto left = std::make_shared<core::Se3Tracker>(left_hand_collection_id);
auto right = std::make_shared<core::Se3Tracker>(right_hand_collection_id);

std::vector<std::shared_ptr<core::ITracker>> trackers{head, left, right};
core::McapRecordingConfig recording{
    episode_pose_file,
    {{head.get(), "head"}, {left.get(), "left_hand"}, {right.get(), "right_hand"}}
};
auto pose_session =
    core::DeviceIOSession::run(trackers, oxr_handles, std::move(recording));
```

`episode_pose_file` selects `poses.mcap`; the strings are channel base names, not
collection names or coordinate frames. Existing recording helpers may add tracked-state
channels alongside pose records. The application calls `pose_session->update()` throughout
running and provider drain, and uses `finish_episode(...)` for final completion. This snippet
does not implement the upstream camera recorder, run-log records or completion handshake.
Existing [C++ recording](../../examples/mcap_record_replay/cpp/record_full_body.cpp) and
[multi-VIVE recording](../../examples/mcap_record_replay/python/isaaccapture_examples/mcap_record_replay/record_se3_vive.py)
examples demonstrate the session and tracker configuration patterns.

Each stream retains its measurement/availability timestamps and documented frame. Sharing
a file neither aligns coordinates nor guarantees simultaneous samples. Combining hand and
head poses requires known reference-frame alignment, such as a validated fixed
`T_session_slamworld` mapping into the native trackers' reference frame. The `head` stream
initially tracks the configured camera `Body`; an anatomical head frame also needs a known
mounting transform. The same applies to a device pose named for a hand.

A head fresh-map reset ends this extended example's episode and starts another directory
with a new head collection. Rebuild the affected `DeviceIOSession` and its recording file;
all three streams get new recording segments. A new tracker reader may retrieve retained
history from the continuing hand collections, so files are not automatically disjoint
sample ranges.
Record and qualify actual per-stream boundaries, retained-history overlap and transition
gaps in the episode manifest. Hand and camera producer processes can keep running;
camera continuation follows the recorder-boundary condition in the reset contract.
Independent hand sessions need not restart.

## Risks and mitigations

These qualitative levels assess current uncertainty and impact on initial delivery, not
measured probabilities: **High** means an unresolved approval or integration guarantee could
block release; **Medium** means a defined mitigation still needs implementation or validation.
Ratings should be revisited with review decisions and qualification evidence.

| Risk | Level | Why it matters | Mitigation or fallback |
|---|---|---|---|
| Engine adapter licensing and distribution | High | The filed OSRB review must establish the permitted license and distribution boundary for NVIDIA-authored code calling ORB-SLAM3's GPLv3 APIs alongside the Apache-2.0 components. Keeping upstream unchanged does not settle this. | Keep engine-specific code isolated and upstream unmodified. Seek an approved adapter license/package arrangement; if unavailable, omit that adapter and qualify another approved engine. Confirm the selected cuVSLAM SDK's license and review scope separately. |
| Unmodified-engine compatibility | High | Stock ORB-SLAM3 does not yet have demonstrated conformance for stable frames through inertial initialization/recovery or bounded shutdown. A working camera example does not prove these guarantees. | The engine adapter handles native translation, queues and result association. Qualify a pinned configuration using public APIs; narrower modes or ending on frame uncertainty are candidates. Validate normal process exit and all drain, receipt and storage completion requirements; forced termination leaves the episode incomplete. Defer unsupported behavior or select another conformant engine; do not weaken frame or completion guarantees. |
| Upstream camera dependency | High | The camera interface is unsettled; today's OAK executable does not provide the required stereo/IMU, metadata, ownership and end-of-stream contract. API changes or missing guarantees can block integration. | Agree and qualify one OAK-D profile with camera owners. Keep transport details behind the reader integration and validate source identities, buffer lifetime, readiness and loss/end semantics before engine admission. |
| Recording completeness across processes | High | Independent sensor, metadata and pose writers can miss different samples or fail during finish/reset; successful pose publication alone cannot prove a complete episode. | Camera owners provide lossless records with source identities. Agree run-log schemas and coverage validation; core owners add receipt progress and finalization results. The application requires exact source coverage and successful receipt/write/flush/close outcomes, otherwise marks the episode incomplete. |
| Calibration and timing errors | Medium | Incorrect delivered-image calibration, extrinsics, correction state or timestamps can produce plausible but wrong poses. | Qualify one mode/format/model, calibration and clock/anchor policy. Validate the resolved setup and IMU coverage, reject unknown/contradictory metadata, and exercise the selected profile against known geometry and motion. |
| Throughput, memory and latency | Medium | CPU staging, preprocessing, native SLAM and lossless recording can exceed device or host capacity; coverage checks can delay episode finalization/reset. | Measure the selected profile under recording load and through finalization/reset; set finite bounds and latency/drain targets. Reduce resolution/rate or restrict the profile when needed. Keep overload observable and do not silently discard required measurements to meet latency. |
| Offline reconstruction and repeatability | Medium | Reconstructed inputs or trajectories may differ with preprocessing, engine mode/build or execution environment; stock ORB-SLAM3 has no qualified repeatable mode. | Retain original sensors, setup/build identity and ordered processing decisions. Qualify reconstruction and any repeatability claim separately for the selected engine/mode. Full prepared-input recording and generic replay tooling remain deferred. |

Until distribution approval is established, default artifacts exclude ORB-SLAM3 native
source/headers/binaries/assets and ORB-dependent compiled artifacts. Source-only publication
or user-built linking is a proposal for review, not an automatic licensing workaround.
Any fallback engine must satisfy the same functional acceptance criteria; a stub only
supports development. The [backend survey](pose-provider-backend-survey.md) retains the
technical evidence behind the engine risks.

## Remaining decisions and verification

Initial delivery is the one-camera/one-engine application, including recording and forced
fresh-map reset, through a qualified real adapter. Select the initial OAK-D profile and
mode from the supported `Stereo`/`StereoImu` contract before implementation; additional
profiles, engines, formats and the head/hand/RGB composition do not expand this release gate.
A stub alone does not qualify the functional path.

Resolve the dependencies in the [risk table](#risks-and-mitigations) before claiming the
affected path is ready. Also choose application readiness/finish/reset transport, consumer
rebind, output collection naming and startup/drain deadlines compatible with `PluginManager`.

Acceptance verifies the contracts above on that selected path:

- Correct calibration, clocks/source associations, bounded preparation and required IMU
  coverage, including overload and normal-finish cutoff.
- Ordered engine admission/results, stable map/frame behavior and bounded drain/abort.
- Single-copy sensor recording, complete setup/decision metadata, exact source coverage,
  full pose receipt and truthful finalization; exercise recorder startup/tail boundaries,
  missing source references, missing/duplicate/zero poses, raw-anchor matching and
  writer failure.
- Reset isolation, empty-map startup, consumer rebind and renewed `Session` alignment; exercise
  lost/hung engines, pending output, repeat requests and failed replacement startup.
- Measured throughput, memory and measurement-to-availability latency under recording load;
  status distinguishes output silence from stale estimates.

Preserve existing pose schemas and native-device consumption. The extended head/hand/RGB
example illustrates composition; deployments enabling it separately qualify its recording
boundaries and frame alignment. These are acceptance requirements, not existing test claims.

## Canonical input reference

### Canonical CPU image layouts

Prepared image strides are bytes. Validate positive dimensions, overflow-safe size
calculations and these minimum layouts before native ingestion:

| Format | Layout | Stride / minimum buffer size |
|---|---|---|
| `Gray8` | Packed intensity byte per pixel | `stride >= width`; `stride * height` bytes |
| `BGR888` | Packed B, G, R bytes per pixel | `stride >= 3 * width`; `stride * height` bytes |
| `RGBA8` | Packed R, G, B, A bytes per pixel | `stride >= 4 * width`; `stride * height` bytes |
| `NV12` | Y followed by interleaved UV, same stride | Even width/height; `stride >= width`; UV offset `stride * height`; size `stride * (height + height / 2)` |

Channel order, intensity/color semantics and any `NV12` matrix/range must match resolved
setup. Upstream `Gray8`/`Rgba8` map to `Gray8`/`RGBA8`; `Rgb8` needs a declared conversion such
as channel swapping to `BGR888`. Preserve `NV12` BT.709 limited/full range explicitly; a bare
layout tag does not identify colorimetry. These definitions do not require all-format support.

### Canonical calibration

Pinhole pixel origin is the upper-left pixel center. `fx/fy/cx/cy` and the following
coefficients describe the prepared image grid, including any rectification:

| Model | Count | Coefficient order |
|---|---:|---|
| `None` | 0 | Empty |
| `RadTan5` | 5 | `k1, k2, p1, p2, k3` |
| `Rational8` | 8 | `k1, k2, p1, p2, k3, k4, k5, k6` |
| `ThinPrism12` | 12 | `Rational8` followed by `s1, s2, s3, s4` |
| `Tilted14` | 14 | `ThinPrism12` followed by `tau_x, tau_y` |

Tags name models, not counts. Validate finite principal point/coefficients, positive finite
focal lengths, declared coefficient count, positive dimensions, sensor identities/rates and
rigid transforms against resolved setup. Reject unknown tags or unsupported engine models.
An explicitly tagged OpenCV pinhole source with `k1, k2, p1, p2` maps to `RadTan5` with
`k3 = 0`; four-term fisheye is a different, unsupported family. An explicitly tagged
OpenCV/DepthAI pinhole source may use a shorter canonical model only when omitted trailing
terms are exactly zero.
Do not guess from count, truncate nonzero terms or fit another model silently. A rectified
distortion-free grid uses `None`, with extrinsics following its optical frame.
IMU calibration is required in `StereoImu` and absent in `Stereo`; its frame, rates and
noise/timing parameters must satisfy the qualified profile and engine validation. Changes
to `Body`, geometry or rectification require newly resolved setup and a new run.

## References

- [Camera Capture Interface][camera-design] — upstream draft, checked 2026-10-26.
- [Existing Pose Provider design](pose-provider-interface-design.md) — prior architecture and
  historical contract; the canonical input reference for this draft is included above.
- [Backend survey](pose-provider-backend-survey.md) — engine behavior and qualification.
- [Manus runtime](../../src/plugins/manus/core/inc/manus/manus_hand_tracking_plugin.hpp) and
  [entry point](../../src/plugins/manus/app/main.cpp) — executable-plugin pattern.
- [OAK entry point](../../src/plugins/oak/main.cpp) and
  [VIVE entry point](../../src/plugins/vive_se3_tracker/main.cpp) — explicit update loops.
- [OGLO runtime](../../src/plugins/oglo_tactile/oglo_tactile_plugin.cpp) — loop inside `run()`.
- [PluginManager API](../../src/core/plugin_manager/cpp/inc/plugin_manager/plugin_manager.hpp).
- [DeviceIOSession and recording configuration](../../src/core/deviceio_session/cpp/inc/deviceio_session/deviceio_session.hpp).
- [SE(3) schema](../../src/core/schema/fbs/se3_tracker.fbs) and
  [timestamps](../../src/core/schema/fbs/timestamp.fbs).
- [VIVE pose plugin](../../src/plugins/vive_se3_tracker/README.md).

[camera-design]: https://docs.google.com/document/d/1Ese3-J08cWwMvezwUUChCLmPGCW09PGnwZZpaDi8n5A/edit
