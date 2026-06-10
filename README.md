# vm_iot

A lightweight RTSP camera daemon built on top of **GStreamer** and the
**gst-rtsp-server** library. It captures frames from a V4L2 device,
encodes them via a pluggable software backend (`x264` / `openh264` for
H.264, `x265` for H.265), and exposes the stream through a configurable
RTSP mount point.

The project targets Linux IoT / edge boxes where you want a single, small,
config-driven binary instead of a hand-written `gst-launch` shell script.

---

## Features

- **YAML-driven configuration** with full CLI overrides
  (`CLI > YAML > built-in defaults`).
- **Pluggable software encoders**: `x264` / `openh264` (H.264) and
  `x265` (H.265). Hardware backends (VAAPI / NVENC / V4L2 M2M) were
  removed as they are unavailable on the target environment
  (Ubuntu under UTM / aarch64).
- **Single shared pipeline** for all RTSP clients
  (`gst_rtsp_media_factory_set_shared(TRUE)`) — CPU does not scale with
  the number of viewers.
- **Graceful shutdown** on `SIGINT` / `SIGTERM` via a `GMainLoop`.
- **Structured logging** through `spdlog`.
- **Unit tests** with **GoogleTest**, fetched automatically by CMake
  (`FetchContent`); no system-wide install required.
- **Hot-reload friendly**: the build symlinks `config/` next to the
  executable, so editing `config/default.yaml` and restarting the
  process is enough — no rebuild needed.

---

## Repository layout

```
vm_iot/
├── CMakeLists.txt            # Top-level build script
├── config/
│   └── default.yaml          # Default runtime configuration
├── scripts/
│   └── gst_launch/           # Standalone gst-launch helpers (probe / preview / record / rtsp)
├── src/
│   ├── main.cpp              # Entry point: init, parse args, run main loop
│   ├── app/                  # Application layer
│   │   ├── config.{h,cpp}    # YAML loader + CLI merger
│   │   └── signal_handler.* # SIGINT/SIGTERM → GMainLoop quit
│   ├── common/
│   │   └── log.h             # spdlog wrappers (LOGI / LOGW / LOGE …)
│   └── rtsp/
│       ├── rtsp_server.*     # gst-rtsp-server wrapper
│       └── pipeline_builder.* # Builds the gst-launch pipeline string
└── tests/                    # GoogleTest-based unit tests
    ├── CMakeLists.txt
    ├── test_config.cpp
    └── demo/
        └── simple_v4l2_grap.c
```

---

## Requirements

- Linux with a working V4L2 source (`/dev/videoN`)
- **CMake ≥ 4.2**
- A C++17 compiler (GCC or Clang)
- **GStreamer 1.0** + RTSP server plugins
- **yaml-cpp**
- **spdlog**
- (Optional) Internet access on the **first** build — CMake will fetch
  GoogleTest via `FetchContent`.

### Install on Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config git \
    libgstreamer1.0-dev \
    libgstrtspserver-1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad  gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav        gstreamer1.0-tools \
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

After a successful build the layout is:

```
build/
├── vm_iot           # The main binary
└── config -> ../config   # symlink created by POST_BUILD
```

To skip building unit tests:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
```

---

## Run

```bash
./build/vm_iot                                # Uses config/default.yaml
./build/vm_iot -c config/default.yaml         # Explicit config file
./build/vm_iot --port 8555 --bitrate 6000     # CLI overrides
```

By default the server binds to:

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

[config/default.yaml](config/default.yaml):

```yaml
server:
  port: 8554
  mount: /live
capture:
  device: /dev/video0
  width: 1280
  height: 720
  framerate: 30
  pixfmt: UYVY            # raw format coming out of the camera
encoder:
  backend: x264           # x264 | openh264 | x265
  bitrate_kbps: 4000
  gop: 30
  bframes: 0              # only honored by x264; ignored by openh264 / x265
log:
  level: info             # trace | debug | info | warn | err
```

### CLI options

| Flag                 | Overrides             | Example             |
|----------------------|-----------------------|---------------------|
| `-c, --config FILE`  | path to YAML config   | `-c my.yaml`        |
| `-d, --device PATH`  | `capture.device`      | `-d /dev/video2`    |
| `-p, --port N`       | `server.port`         | `-p 9000`           |
| `-b, --bitrate KBPS` | `encoder.bitrate_kbps`| `-b 6000`           |
| `-l, --log-level L`  | `log.level`           | `-l debug`          |
| `-h, --help`         | print usage and exit  |                     |

Override priority: **CLI > YAML > built-in defaults**.

### Hot reload

Because the build creates a symbolic link from `build/config` to the
project's `config/` directory, editing any file under `config/` and
restarting the process picks up the new values immediately — there is
no need to rebuild.

---

## Encoder backends

Only pure-software encoders are supported. The target deployment
(Ubuntu under UTM / aarch64) has no usable hardware encoder, so the
VAAPI / NVENC / V4L2 M2M backends were removed.

| backend    | codec  | GStreamer element | Notes                                                          |
|------------|--------|-------------------|----------------------------------------------------------------|
| `x264`     | H.264  | `x264enc`         | Default. Best compatibility, good speed/quality trade-off.     |
| `openh264` | H.264  | `openh264enc`     | Cisco's H.264 encoder. No B-frames; `bframes` is ignored.      |
| `x265`     | H.265  | `x265enc`         | Lower bitrate at same quality, ~1.5–2× CPU; client must support HEVC. |

`PipelineBuilder::build` ([src/rtsp/pipeline_builder.cpp](src/rtsp/pipeline_builder.cpp))
picks the matching parser and RTP payloader automatically: H.264
backends use `h264parse` + `rtph264pay`, `x265` uses `h265parse` +
`rtph265pay`. The source pixel format requested from the camera comes
from `capture.pixfmt` and is converted to `I420` (the input format all
three software encoders expect) via `videoconvert`.

---

## Logging

All runtime logs go through `spdlog`. The level is taken from
`log.level` (or `--log-level`). Use `debug` or `trace` to see the full
pipeline string and per-client events.

---

## Tests

Tests are built by default and use **GoogleTest**, fetched automatically.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Add a new test by dropping a `test_*.cpp` file into `tests/`; the
`tests/CMakeLists.txt` script picks it up automatically.

---

## Helper scripts

The `scripts/gst_launch/` folder contains small standalone shell scripts
that wrap raw `gst-launch-1.0` pipelines for quick experimentation
(probe a device, preview locally, record to file, run `test-launch`,
etc.). They are independent from the main binary and are useful when
you need to debug capture / encoding outside of the daemon.

---

## Troubleshooting

- **`load config failed: bad file: config/default.yaml`** — run the
  binary from a directory that contains `config/`, or pass an explicit
  path with `-c`. After a normal `cmake --build`, running from `build/`
  works thanks to the symlink.
- **`gst_rtsp_server_attach failed (port 8554 in use?)`** — another
  process is bound to the same port; change `server.port` or `--port`.
- **Black / no video on the client** — verify the camera supports the
  requested `width / height / framerate / pixfmt` combination
  (`v4l2-ctl --list-formats-ext -d /dev/video0`).
- **`unknown encoder backend: ...`** — `encoder.backend` must be one of
  `x264 | openh264 | x265`.
- **HEVC client cannot play the stream** — when `backend: x265`, the
  server emits an H.265 RTP stream (`rtph265pay`); make sure your
  player supports HEVC (recent VLC / `ffplay` work; many browsers and
  mobile players do not).

---

## License

TBD.
