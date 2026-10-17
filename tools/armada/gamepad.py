#!/usr/bin/env python3
"""Drive a temporary Linux gamepad using JSON lines on stdin during hardware tests."""
import fcntl
import json
import os
import struct
import sys
import time

BUTTONS = {"a": 0x130, "b": 0x131, "x": 0x133, "y": 0x134,
           "lb": 0x136, "rb": 0x137, "back": 0x13A, "start": 0x13B,
           "ls": 0x13D, "rs": 0x13E}
AXES = {"lx": 0, "ly": 1, "lt": 2, "rx": 3, "ry": 4, "rt": 5, "dx": 16, "dy": 17}

def main():
    descriptor = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    created = False
    def event(kind, code, value):
        os.write(descriptor, struct.pack("llHHi", 0, 0, kind, code, value))
    try:
        for kind in (1, 3):
            fcntl.ioctl(descriptor, 0x40045564, kind)
        for button in BUTTONS.values():
            fcntl.ioctl(descriptor, 0x40045565, button)
        minimum, maximum = [0] * 64, [0] * 64
        for name, axis in AXES.items():
            fcntl.ioctl(descriptor, 0x40045567, axis)
            minimum[axis], maximum[axis] = ((0, 255) if name in ("lt", "rt") else
                                           (-1, 1) if name in ("dx", "dy") else (-32768, 32767))
        setup = struct.pack("80sHHHHI", b"Fable II native test gamepad", 3, 0x045E, 0x028E, 1, 0)
        setup += struct.pack("256i", *(maximum + minimum + [0] * 128))
        os.write(descriptor, setup)
        fcntl.ioctl(descriptor, 0x5501)
        created = True
        time.sleep(1)
        print("GAMEPAD_READY", flush=True)
        for line in sys.stdin:
            command = json.loads(line)
            if command.get("quit"):
                break
            button = command.get("button")
            axis = command.get("axis")
            if button:
                event(1, BUTTONS[button.lower()], 1)
            if axis:
                event(3, AXES[axis], int(command.get("value", 0)))
            event(0, 0, 0)
            time.sleep(float(command.get("hold", 0.2)))
            if button:
                event(1, BUTTONS[button.lower()], 0)
            if axis:
                event(3, AXES[axis], 0)
            event(0, 0, 0)
            print("GAMEPAD_OK", flush=True)
    finally:
        if created:
            fcntl.ioctl(descriptor, 0x5502)
        os.close(descriptor)

if __name__ == "__main__":
    main()
