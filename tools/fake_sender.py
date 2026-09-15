#!/usr/bin/env python3
"""Minimal OpenDisplay sender for receiver smoke tests.

Connects to a receiver on TCP :9000, answers its `hello` with `welcome`,
streams a libx264 testsrc video as framed Annex-B access units, responds to
`ping` with `pong`, sends a synthetic cursor sprite + orbiting `cursor`
positions, and restarts the encoder (fresh IDR) on `kf`. No Bonjour
advertisement — point the receiver's peer at it by address if needed.
"""

import argparse
import base64
import json
import math
import socket
import struct
import subprocess
import sys
import threading
import time

START_CODE = b"\x00\x00\x00\x01"
# 1x1 white PNG — stands in for a real cursor sprite.
TINY_PNG = base64.b64encode(
    bytes.fromhex(
        "89504e470d0a1a0a0000000d4948445200000001000000010806"
        "0000001f15c4890000000a49444154789c63000100000500010d"
        "0a2db40000000049454e44ae426082"
    )
).decode()

# reader_loop, cursor_loop, and the video loop all write to the same
# socket — a sendall() may span several syscalls, so frame writes
# must be serialised or the byte stream interleaves and corrupts framing.
SEND_LOCK = threading.Lock()


def send_message(connection: socket.socket, message: dict) -> None:
    payload = json.dumps(message, separators=(",", ":")).encode()
    with SEND_LOCK:
        connection.sendall(struct.pack(">I", len(payload)) + payload)


def send_frame(connection: socket.socket, payload: bytes) -> None:
    with SEND_LOCK:
        connection.sendall(struct.pack(">I", len(payload)) + payload)


def annexb_access_units(data: bytes):
    """Yield complete access units from an Annex B byte stream.

    A new AU starts at each AUD (type 9, we ask x264 to emit them) or at a
    VCL NALU following a VCL NALU (x264 writes one slice per frame). SPS/PPS
    ahead of an IDR ride in the same AU as their slice.
    """
    # Split into NALUs on 4-byte start codes.
    nalus = []
    position = 0
    while True:
        start = data.find(START_CODE, position)
        if start == -1:
            break
        end = data.find(START_CODE, start + 4)
        nalus.append(data[start : end if end != -1 else len(data)])
        position = end if end != -1 else len(data)
    units = []
    current = bytearray()
    seen_vcl = False
    for nalu in nalus:
        if len(nalu) <= 4:
            continue
        nalu_type = nalu[4] & 0x1F
        boundary = nalu_type == 9 or (
            seen_vcl and 1 <= nalu_type <= 5
        )
        if boundary and current:
            units.append(bytes(current))
            current = bytearray()
            seen_vcl = False
        current += nalu
        if 1 <= nalu_type <= 5:
            seen_vcl = True
    if current:
        units.append(bytes(current))
    return units


def encoder_process(width: int, height: int, fps: int) -> subprocess.Popen:
    return subprocess.Popen(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
            "-re", "-f", "lavfi",
            "-i", f"testsrc2=size={width}x{height}:rate={fps}",
            "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
            "-bf", "0", "-g", "300",
            "-x264-params", "repeat-headers=1:aud=1",
            "-f", "h264", "pipe:1",
        ],
        stdout=subprocess.PIPE,
    )


def reader_loop(connection: socket.socket, state: dict) -> None:
    """Handle receiver -> sender control messages."""
    buffer = bytearray()
    while True:
        chunk = connection.recv(65536)
        if not chunk:
            break
        buffer += chunk
        while len(buffer) >= 4:
            size = struct.unpack(">I", buffer[:4])[0]
            if size == 0 or size >= 1 << 20 or len(buffer) < 4 + size:
                break
            payload = bytes(buffer[4 : 4 + size])
            del buffer[: 4 + size]
            try:
                message = json.loads(payload)
            except (ValueError, UnicodeDecodeError):
                continue
            kind = message.get("type")
            if kind == "hello":
                print(f"hello: {message}", flush=True)
                state["cursor_port"] = message.get("cursorPort")
                send_message(connection, {"type": "welcome", "pv": 3, "min": 1})
                send_message(connection, {
                    "type": "cursorImg", "nw": 0.02, "nh": 0.03,
                    "ax": 0.1, "ay": 0.05, "png": TINY_PNG,
                })
            elif kind == "ping":
                send_message(connection, {
                    "type": "pong", "t": message["t"],
                    "mt": time.time() * 1000,
                })
            elif kind == "kf":
                print("keyframe requested — restarting encoder", flush=True)
                state["restart"] = True
            elif kind in ("touch", "scroll", "stats", "cursorAck"):
                print(f"{kind}: {message}", flush=True)


def cursor_loop(
    connection: socket.socket, state: dict, peer: tuple[str, int]
) -> None:
    """Orbit the remote cursor so overlay rendering is visible.

    Once the receiver's hello advertises `cursorPort`, positions move to
    that UDP channel as bare JSON datagrams (protocol section 6.3); until
    then they ride TCP as unsequenced `cursor` messages.
    """
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 0
    udp_flow = False
    while True:
        angle = time.monotonic()
        message = {
            "type": "cursor",
            "x": 0.5 + 0.3 * math.cos(angle),
            "y": 0.5 + 0.3 * math.sin(angle),
            "v": 1,
        }
        cursor_port = state.get("cursor_port")
        if cursor_port:
            if not udp_flow:
                # The UDP sequence is per-flow and starts at 1.
                udp_flow, seq = True, 0
            seq += 1
            message["s"] = seq
            udp.sendto(
                json.dumps(message, separators=(",", ":")).encode(),
                (peer[0], cursor_port),
            )
        else:
            send_message(connection, message)
        time.sleep(1 / 30)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=800)
    parser.add_argument("--fps", type=int, default=30)
    args = parser.parse_args()

    state = {"restart": False, "cursor_port": None}
    with socket.create_connection((args.host, args.port)) as connection:
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"connected to {args.host}:{args.port}", flush=True)
        threading.Thread(
            target=reader_loop, args=(connection, state), daemon=True).start()
        threading.Thread(
            target=cursor_loop,
            args=(connection, state, (args.host, args.port)),
            daemon=True,
        ).start()

        process = encoder_process(args.width, args.height, args.fps)
        pending = bytearray()
        try:
            while True:
                if state["restart"]:
                    state["restart"] = False
                    process.terminate()
                    process.wait()
                    process = encoder_process(args.width, args.height, args.fps)
                    pending.clear()
                chunk = process.stdout.read1(64 * 1024)
                if not chunk:
                    print("encoder exited", flush=True)
                    break
                pending += chunk
                # Emit complete AUs, keeping the tail (a partial AU) buffered.
                last_start = pending.rfind(START_CODE)
                if last_start <= 0:
                    continue
                for unit in annexb_access_units(bytes(pending[:last_start])):
                    telemetry = json.dumps({
                        "cap": int(time.time() * 1000),
                        "snd": int(time.time() * 1000),
                    }, separators=(",", ":")).encode()
                    send_frame(connection, telemetry + unit)
                del pending[:last_start]
        except BrokenPipeError:
            print("receiver closed", flush=True)
        finally:
            process.terminate()


if __name__ == "__main__":
    main()
