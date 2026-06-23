# vm_iot

A small RTSP camera daemon. It reads frames from a V4L2 device, runs an
optional GLSL post-process filter, encodes the result with a software
H.264 / H.265 backend, and serves the stream through `gst-rtsp-server`.

It is meant for Linux IoT and edge boxes where you want one
config-driven binary instead of a hand-maintained `gst-launch` shell
script. A second binary, `vm_iot_ctl`, talks to the running daemon over
a pair of FIFOs to switch effects, take snapshots, or query status
without restarting the stream.

---

## Features

- **YAML config with CLI overrides.** Priority: `CLI > YAML > built-in defaults`.
- **Auto V4L2 capability negotiation.** `V4L2Prober` enumerates what the
  camera actually supports; `CapsRanker` picks the closest
  `(fmt, w, h, fps)` to the requested config (with a `prefer_jpeg`
  preference for USB webcams).
- **Three software encoder backends:** `x264`, `openh264` (both H.264),
  and `x265` (H.265). Hardware backends (VAAPI / NVENC / V4L2 M2M) are
  not included; the target environment (Ubuntu under UTM on aarch64)
  has no usable hardware encoder.
- **GL shader filter, hot-switchable at runtime.** A single
  `effects.frag` file holds all variants (passthrough / mosaic /
  invert / ...); `filter_type` selects which branch runs. The shader
  program is *not* recompiled on switch, so there is no frame hiccup.
- **Live control over a FIFO.** A daemon-side `ControlChannel` listens
  on a named pipe; `vm_iot_ctl` is the matching client. Commands
  include `filter`, `reload`, `status`, `snapshot`.
- **Snapshot.** A `tee` + `valve` branch sits next to the encoder; the
  `snapshot` command opens the valve for one frame and writes a JPEG.
- **One shared pipeline for all RTSP clients**
  (`gst_rtsp_media_factory_set_shared(TRUE)`), so CPU does not grow
  with the viewer count.
- **Clean shutdown** on `SIGINT` / `SIGTERM` through a `GMainLoop`.
- **Logging** via `spdlog`.
- **Unit tests** with GoogleTest. CMake fetches it through
  `FetchContent`, so no system-wide install is needed.
- The build symlinks `assets/` next to the executable, so the runtime
  resources (`config/`, `pag/`, `shaders/`) sit beside the binary.
  Editing those files and restarting the binary is enough; you don't
  need to rebuild.

---

## Repository layout

```
vm_iot/
├── CMakeLists.txt              # Top-level build script
├── assets/                     # Runtime resources (single symlink target)
│   ├── config/
│   │   └── default.yaml        # Default runtime configuration
│   ├── pag/                    # .pag assets consumed by pagfilter / selftest
│   └── shaders/
│       └── effects.frag        # GL fragment shader; holds all filter variants
├── docs/
│   └── gstreamer/              # Per-element notes for every gst element used
├── scripts/
│   ├── gst_launch/             # Standalone gst-launch helpers (probe / preview / record / rtsp)
│   └── bench/                  # Encoder benchmarks (e.g. h265_compare.sh)
├── src/
│   ├── main.cpp                # Entry point: init, parse args, run main loop
│   ├── app/                    # YAML loader + CLI merge + signal handler
│   ├── common/                 # Logging + pretty pipeline-string printer
│   ├── pipeline/               # PipelineBuilder: builds the full gst-launch string
│   ├── filter/                 # ShaderFilter: hot-switchable glshader wrapper
│   ├── branches/
│   │   └── snapshot/           # Snapshot branch (tee + valve + jpegenc)
│   ├── control/                # ControlChannel: FIFO-based command server
│   ├── rtsp/                   # gst-rtsp-server wrapper
│   ├── util/                   # V4L2Prober, CapsRanker
│   └── cli/
│       └── iotcamctl.cpp       # vm_iot_ctl source (no business logic, just protocol)
└── tests/                      # GoogleTest-based unit tests + developer helpers
    ├── test_config.cpp
    ├── test_caps_ranker.cpp
    ├── test_pipeline_builder.cpp
    ├── test_v4l2_prober.cpp
    ├── tools/
    │   └── probe_dev.cpp       # Standalone V4L2 capability dump (probe_dev binary)
    └── demo/simple_v4l2_grap.c
```

---

## Requirements

- Linux with a working V4L2 source (`/dev/videoN`)
- CMake ≥ 3.22
- A C++17 compiler (GCC or Clang)
- GStreamer 1.0 with the RTSP server, GL, and base/good/bad/ugly plugins
- `yaml-cpp`
- `spdlog`
- Internet access on the first build, so CMake can fetch GoogleTest
  through `FetchContent`.

### Install on Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config git \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstrtspserver-1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad  gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav        gstreamer1.0-tools \
    gstreamer1.0-gl \
    libyaml-cpp-dev libspdlog-dev
```

---

## Build

```bash
git clone <this-repo> vm_iot
cd vm_iot
cmake -S . -B build
cmake --build build -j
```

The build produces three binaries:

| Binary       | Source                       | Purpose                                                   |
|--------------|------------------------------|-----------------------------------------------------------|
| `vm_iot`     | `src/` (everything but `src/cli`) | The RTSP daemon.                                     |
| `vm_iot_ctl` | `src/cli/iotcamctl.cpp`      | Thin client. Talks to the daemon over FIFOs.              |
| `probe_dev`  | `tests/tools/probe_dev.cpp`  | Stand-alone V4L2 capability dump; useful for cross-checks against `v4l2-ctl --list-formats-ext`. |

After the build:

```
build/
├── vm_iot
├── vm_iot_ctl
├── probe_dev
└── assets -> ../assets      # single symlink covers config/ pag/ shaders/
```

`make install` (or the install step of CPack) copies `vm_iot` and
`vm_iot_ctl` into `bin/`. `probe_dev` is a debug helper and is not
installed.

To skip the unit tests:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
```

---

## Run

```bash
./build/vm_iot                                # Uses assets/config/default.yaml
./build/vm_iot -c assets/config/default.yaml  # Explicit config file
./build/vm_iot --port 8555 --bitrate 6000     # CLI overrides
```

The server binds to:

```
rtsp://<host>:8554/live
```

Quick playback:

```bash
# H.264 (backend: x264 / openh264)
gst-launch-1.0 -v rtspsrc location=rtsp://127.0.0.1:8554/live ! \
    rtph264depay ! avdec_h264 ! videoconvert ! autovideosink

# H.265 (backend: x265)
gst-launch-1.0 -v rtspsrc location=rtsp://127.0.0.1:8554/live ! \
    rtph265depay ! avdec_h265 ! videoconvert ! autovideosink

# Codec-agnostic (decodebin auto-detects):
ffplay rtsp://127.0.0.1:8554/live
```

---

## Configuration

[assets/config/default.yaml](assets/config/default.yaml):

```yaml
server:
  port: 8554
  mount: /live
capture:
  device: /dev/video0
  # The width/height/framerate below are *requested* values. At startup,
  # V4L2Prober enumerates what the camera actually supports and
  # CapsRanker picks the closest (fmt, w, h, fps) combination.
  width: 1280
  height: 720
  framerate: 30
  pixfmt: I420            # I420 | YUY2 | NV12 ... — the format fed to the encoder
  prefer_jpeg: true       # true = prefer MJPG from camera; false = prefer raw
encoder:
  backend: x264           # x264 | openh264 | x265
  bitrate_kbps: 4000
  gop: 30
  bframes: 0              # only honored by x264; ignored by openh264 / x265
filter:
  enabled: true                   # false = no GL filter stage in the pipeline
  shader: effects.frag            # path is resolved relative to assets/shaders/
  filter_type: 0                  # startup variant: 0=passthrough 1=mosaic 2=invert
  max_type: 2                     # upper bound for `filter next` cycling
control:
  request_fifo: /tmp/vm_iot.ctl   # command FIFO;  empty string = disabled
  reply_fifo:   /tmp/vm_iot.reply # reply FIFO;    empty string = no reply
snapshot:
  dir: /tmp/vm_iot/snapshots      # default JPEG output dir if `snapshot` has no PATH
  quality: 90
  timeout_ms: 1500                # how long to wait for one frame
log:
  level: info             # trace | debug | info | warn | err
```

### CLI options (daemon)

| Flag                 | Overrides             | Example             |
|----------------------|-----------------------|---------------------|
| `-c, --config FILE`  | path to YAML config   | `-c my.yaml`        |
| `-d, --device PATH`  | `capture.device`      | `-d /dev/video2`    |
| `-p, --port N`       | `server.port`         | `-p 9000`           |
| `-b, --bitrate KBPS` | `encoder.bitrate_kbps`| `-b 6000`           |
| `-l, --log-level L`  | `log.level`           | `-l debug`          |
| `-h, --help`         | print usage and exit  |                     |

Override priority: `CLI > YAML > built-in defaults`.

### Hot reload

The build creates a single symlink from `build/assets` to `assets/`,
so `build/assets/config`, `build/assets/pag`, and `build/assets/shaders`
are all live views into the repo. Edit any file under those directories
and restart the process; the new values are picked up without a
rebuild. For shader-only edits, you can skip the restart and run
`vm_iot_ctl reload` instead.

---

## Encoder backends

Only software encoders are supported. The target deployment (Ubuntu
under UTM on aarch64) has no usable hardware encoder, so the VAAPI /
NVENC / V4L2 M2M backends were dropped.

| backend    | codec  | GStreamer element | Notes                                                          |
|------------|--------|-------------------|----------------------------------------------------------------|
| `x264`     | H.264  | `x264enc`         | Default. Best compatibility, good speed/quality trade-off.     |
| `openh264` | H.264  | `openh264enc`     | Cisco's H.264 encoder. No B-frames; `bframes` is ignored.      |
| `x265`     | H.265  | `x265enc`         | Lower bitrate at the same quality, ~1.5–2× CPU; the client must support HEVC. |

`PipelineBuilder::build` ([src/pipeline/pipeline_builder.cpp](src/pipeline/pipeline_builder.cpp))
picks the matching parser and RTP payloader based on the backend: the
H.264 backends use `h264parse` + `rtph264pay`, `x265` uses `h265parse`
+ `rtph265pay`. The pixel format negotiated with the camera comes from
`capture.pixfmt` and is converted to `I420` (the input format all three
software encoders expect) through `videoconvert`. When `filter.enabled`
is true, a GL stage (`glupload ! glcolorconvert ! glshader !
gldownload`) is inserted between capture and encode.

---

## Live control: `vm_iot_ctl`

`vm_iot_ctl` is a stand-alone CLI client. It does no business logic on
its own: it translates a friendly subcommand into one line of the
`ControlChannel` protocol, writes that line to the request FIFO, reads
the reply from the reply FIFO, and maps the result to an exit code.
Cold start is under 100 ms; the binary depends only on libc and
libstdc++.

```bash
# Switch shader effect
./build/vm_iot_ctl filter 2          # set filter_type = 2
./build/vm_iot_ctl filter next       # cycle to next variant
./build/vm_iot_ctl filter prev
./build/vm_iot_ctl filter get        # query current variant

# Reload the shader file from disk (no daemon restart)
./build/vm_iot_ctl reload

# Print runtime status (uptime, clients, encoder, filter, ...)
./build/vm_iot_ctl status

# One-shot JPEG snapshot
./build/vm_iot_ctl snapshot                       # daemon picks the path
./build/vm_iot_ctl snapshot /tmp/frame.jpg        # explicit path

# Send a raw protocol line (for debugging)
./build/vm_iot_ctl raw "filter set 1"
```

### Global options

| Flag             | Purpose                                                        | Default                        |
|------------------|----------------------------------------------------------------|--------------------------------|
| `--ctl PATH`     | Request FIFO (`$IOTCAM_CTL` overrides the default)             | `/tmp/vm_iot.ctl`              |
| `--reply PATH`   | Reply FIFO (`$IOTCAM_REPLY` overrides the default)             | `/tmp/vm_iot.reply`            |
| `--timeout MS`   | How long to wait for the daemon's reply                        | `2000`                         |
| `--json`         | Print the reply as JSON instead of plain text                  | off                            |
| `-q, --quiet`    | Suppress body output; rely on the exit code                    | off                            |
| `--no-lock`      | Skip `flock` on the reply FIFO. Use only if you know nothing else is reading it. | off          |
| `-h, --help`     | Show usage                                                     |                                |

The two FIFO paths must match the daemon's `control.request_fifo` and
`control.reply_fifo` config entries. If you change those in
`assets/config/default.yaml`, point `vm_iot_ctl` at them with `--ctl` /
`--reply` or by exporting `IOTCAM_CTL` / `IOTCAM_REPLY`.

### Exit codes

| Code | Meaning                                                                  |
|------|--------------------------------------------------------------------------|
| 0    | Daemon replied `ok`.                                                     |
| 1    | Daemon replied `err`. Reason is printed to stderr (or `error` in JSON).  |
| 2    | Bad CLI arguments.                                                       |
| 10   | A FIFO is missing or unopenable. Likely the daemon is not running.       |
| 11   | Another `vm_iot_ctl` already holds the reply-FIFO lock. Retry, or pass `--no-lock`. |
| 124  | Timed out waiting for the reply.                                         |

### Wire protocol (if you want to skip the client)

The daemon listens on a plain text FIFO. One command per line, terminated
by `\n`. The reply is one `ok <cmd>` or `err <cmd> <reason>` line, then
zero or more `key=value` body lines, then a single `.` line:

```text
ok filter set 2
type=2
.
```

```bash
# Manual driving without vm_iot_ctl:
cat /tmp/vm_iot.reply &
echo "status" > /tmp/vm_iot.ctl
```

The full protocol is documented at the top of
[src/control/control_channel.h](src/control/control_channel.h).

---

## V4L2 capability probing

Before building the pipeline, the daemon enumerates the camera through
`V4L2Prober` and feeds the result to `CapsRanker`, which scores each
`(fmt, w, h, fps)` combination against the requested config and picks
the winner. The same code is exposed as a stand-alone tool:

```bash
./build/probe_dev /dev/video0
```

Use this to sanity-check what the daemon will pick, or to diff against
`v4l2-ctl --list-formats-ext -d /dev/video0`.

---

## Logging

All logs go through `spdlog`. The level comes from `log.level` (or
`--log-level`). Use `debug` or `trace` to see the full pipeline string
(pretty-printed by `src/common/launch_pretty.h`) and per-client events.

---

## Tests

Tests are built by default and use GoogleTest, fetched automatically.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Current coverage: `test_config`, `test_caps_ranker`,
`test_pipeline_builder`, `test_v4l2_prober`. To add a test, drop a
`test_*.cpp` file into `tests/`. The `tests/CMakeLists.txt` script
picks it up automatically.

---

## Helper scripts

- `scripts/gst_launch/` — small standalone shell scripts that wrap raw
  `gst-launch-1.0` pipelines for quick experiments: probe a device,
  preview locally, record to a file, run `test-launch`, dmabuf/GL
  probe, etc. They are independent from the main binary and useful
  when you want to debug capture or encoding outside the daemon.
- `scripts/bench/` — encoder benchmarks (for example
  `h265_compare.sh`).

---

## GStreamer element notes

`docs/gstreamer/` holds a one-page reference for every element used in
the pipeline (`v4l2src`, `videoconvert`, `glshader`, `x264enc`,
`rtph264pay`, ...). Start at
[docs/gstreamer/README.md](docs/gstreamer/README.md). Whenever a new
element is introduced into the pipeline, a matching `<element>.md` is
added there.

---

## Troubleshooting

- `load config failed: bad file: assets/config/default.yaml` — run the
  binary from a directory that contains `assets/`, or pass an explicit
  path with `-c`. After a normal `cmake --build`, running from `build/`
  works because of the symlink.
- `gst_rtsp_server_attach failed (port 8554 in use?)` — another process
  is bound to the same port; change `server.port` or pass `--port`.
- Black screen or no video on the client — check that the camera
  actually supports the requested `width / height / framerate / pixfmt`
  combination. Run `./build/probe_dev /dev/video0` or
  `v4l2-ctl --list-formats-ext -d /dev/video0`.
- `unknown encoder backend: ...` — `encoder.backend` must be one of
  `x264 | openh264 | x265`.
- HEVC client cannot play the stream — when `backend: x265`, the server
  emits an H.265 RTP stream (`rtph265pay`). Make sure the player
  supports HEVC. Recent VLC and `ffplay` work; many browsers and mobile
  players do not.
- `vm_iot_ctl` exits with code `10` — the request or reply FIFO does
  not exist. Check that the daemon is running and that
  `control.request_fifo` / `control.reply_fifo` in the YAML match the
  paths `vm_iot_ctl` is looking at (`--ctl` / `--reply` or `IOTCAM_CTL`
  / `IOTCAM_REPLY`).
- `vm_iot_ctl` exits with code `11` — another `vm_iot_ctl` is holding
  the reply-FIFO lock. Wait or pass `--no-lock` if you understand the
  trade-off (you may read another instance's reply).
- `vm_iot_ctl` exits with code `124` — the daemon did not answer in
  time. Increase `--timeout` or check the daemon logs.

---

## License

TBD.
