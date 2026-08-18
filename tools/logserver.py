#!/usr/bin/env python3
"""
Captures the debug plugin's log output.

The debug build sends its log lines as UDP broadcasts on port 4405 (WHBLogUdp).
This listens for them, prints them with a local timestamp, and tees everything to
a file so a measurement run can be pasted somewhere afterwards.

    python tools/logserver.py                  # print + write wiiu-log.txt
    python tools/logserver.py -o run480p.txt   # choose the file
    python tools/logserver.py --filter fps     # only lines containing "fps"

Windows will almost certainly pop a Defender Firewall prompt the first time.
Allow it on Private networks, or nothing will ever arrive.
"""

import argparse
import datetime
import socket
import sys

PORT = 4405
# Log lines are short; 2 KB is well past anything WHBLogUdp emits in one datagram.
BUFFER = 2048


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture Wii U plugin debug logs over UDP.")
    parser.add_argument("-o", "--output", default="wiiu-log.txt",
                        help="file to tee the log into (default: wiiu-log.txt)")
    parser.add_argument("-p", "--port", type=int, default=PORT,
                        help=f"UDP port to listen on (default: {PORT})")
    parser.add_argument("--filter", default=None,
                        help="only show lines containing this substring")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # The console broadcasts, so the socket has to accept broadcast traffic and
    # must not fight another listener for the port.
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    try:
        sock.bind(("0.0.0.0", args.port))
    except OSError as e:
        print(f"Could not bind UDP {args.port}: {e}", file=sys.stderr)
        print("Something else may already be listening (another copy of this script?).", file=sys.stderr)
        return 1

    print(f"Listening for Wii U debug logs on UDP {args.port}.")
    print(f"Writing to {args.output}. Press Ctrl+C to stop.")
    print("If nothing appears: allow Python through the Windows firewall, and make sure")
    print("the console is running the DEBUG build (the release build logs nothing).")
    print("-" * 72)

    count = 0
    with open(args.output, "a", encoding="utf-8", errors="replace") as out:
        out.write(f"\n===== capture started {datetime.datetime.now():%Y-%m-%d %H:%M:%S} =====\n")
        out.flush()
        try:
            while True:
                data, addr = sock.recvfrom(BUFFER)
                text = data.decode("utf-8", errors="replace").rstrip("\r\n")
                if not text:
                    continue
                if args.filter and args.filter not in text:
                    continue

                stamp = datetime.datetime.now().strftime("%H:%M:%S")
                line = f"[{stamp}] {addr[0]}  {text}"
                print(line, flush=True)
                # Flushed per line: a crash or an unplugged console should never
                # cost the measurements already captured.
                out.write(line + "\n")
                out.flush()
                count += 1
        except KeyboardInterrupt:
            print("-" * 72)
            print(f"Stopped. Captured {count} lines into {args.output}.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
