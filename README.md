# OpenDisplay Receiver for Linux

Turn an Ubuntu (or other Linux) machine into a display for an
[OpenDisplay](https://github.com/peetzweg/opendisplay) sender. The receiver
listens on TCP :9000, advertises itself as `_opensidecar._tcp` over Bonjour,
decodes the sender's H.264 Annex-B stream with FFmpeg, shows it in a window
(or fullscreen), and forwards mouse/touch/scroll input back to the sender.

It speaks the [OpenDisplay wire protocol](../opendisplay/PROTOCOL.md) (`pv`
3) — the same protocol as the official iOS receiver and the
[Android port](https://github.com/josepacelli/opendisplay-android).

## Ubuntu 24.04 dependencies

```sh
sudo apt install cmake g++ qt6-base-dev libavahi-client-dev ffmpeg
```

Bonjour discovery needs a running Avahi daemon (installed and enabled by
default on desktop Ubuntu):

```sh
sudo systemctl enable --now avahi-daemon.service
avahi-browse -rt _opensidecar._tcp   # verify the advertisement
```

If a firewall is enabled, allow the ports:

```sh
sudo ufw allow 9000/tcp 9001/udp
```

For hardware decode, install the GPU's VA-API driver (`intel-media-driver`
for modern Intel, `mesa-va-drivers` for AMD, `nvidia-vaapi-driver` for NVIDIA)
and check `vainfo`; the process must be able to open `/dev/dri/renderD128`.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```sh
./build/opendisplay-receiver              # windowed
./build/opendisplay-receiver --fullscreen # display mode (F11 toggles, Esc exits)
```

Open the OpenDisplay app on the sending Mac; the machine appears in its WiFi
device picker under the host name (override with `--name`). Useful options:

```sh
opendisplay-receiver --panel 1920x1080 --scale 1   # override announced panel
opendisplay-receiver --decoder vaapi --vaapi-device /dev/dri/renderD128
opendisplay-receiver --no-input --no-cursor-channel
opendisplay-receiver --port 9000 --verbose
```

## Smoke test without a Mac

`tools/fake_sender.py` dials the receiver and streams an FFmpeg `testsrc2`
pattern with a moving cursor:

```sh
python3 tools/fake_sender.py --host 127.0.0.1 --port 9000
```

## Debian package

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
(cd build && cpack -G DEB)
sudo dpkg -i build/opendisplay-receiver-0.1.0-Linux.deb
```

## How it works

- **Transport** — the receiver listens; the sender dials. One TCP connection
  carries video (sender→receiver) and JSON control messages both ways, with
  the section-4 demux heuristic (`isControlFrame` in `src/wire.cpp`).
- **Video** — each wire frame is one Annex-B access unit; `src/annexb.cpp`
  strips the telemetry prefix and classifies NALUs, `src/h264_sps.cpp` reads
  the coded size from the SPS, and `src/ffmpeg_decoder.cpp` feeds a low-delay
  `ffmpeg` subprocess that returns RGBA frames.
- **Input** — pointer events map onto the letterboxed video rect and travel
  as `touch`/`scroll` messages; the remote cursor rides `cursor`/`cursorImg`
  (TCP, plus the UDP :9001 side channel) and the local pointer hides while
  connected.
- **Discovery** — `src/advertise.cpp` publishes the service via Avahi with a
  stable per-install `id` and `pv` TXT records.

## Current limits

No audio, clipboard, or encryption; keyboard input has no wire message and is
not forwarded. The UDP cursor channel is WiFi-only (the USB/usbmuxd binding is
for Apple receivers). `tools/fake_sender.py` is a test peer, not a sender —
use the real Mac or Linux sender for actual sessions.
