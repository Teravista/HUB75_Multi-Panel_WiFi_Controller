"""
Scale an image to the panel's logical size and send raw RGB over TCP.

Usage example:
    python utilityScripts/sendImageOverTCP.py "sample_image.jpg"
"""

import argparse
import os
import socket
import time

from dotenv import load_dotenv
from PIL import Image, ImageOps

load_dotenv()

PICO_IP = os.environ.get("PICO_IP")
PICO_PORT = os.environ.get("PICO_PORT")
LOGICAL_WIDTH = 128
LOGICAL_HEIGHT = 256
IMAGE_PATH = "imagesVideos\\private\\apple.jpg"
ACK_BYTES = b"ACK\n"


def build_logical_image(image_path: str) -> bytes:
    image = Image.open(image_path).convert("RGB")
    image = ImageOps.fit(
        image,
        (LOGICAL_WIDTH, LOGICAL_HEIGHT),
        method=Image.Resampling.LANCZOS,
        centering=(0.5, 0.5),
    )
    return image.tobytes()  # row-major RGB, 3 bytes per pixel


def main() -> None:
    if not PICO_IP or not PICO_PORT:
        raise RuntimeError("Set PICO_IP and PICO_PORT in the local .env file")

    parser = argparse.ArgumentParser()
    parser.add_argument("image", nargs="?", default=IMAGE_PATH,
                        help=f"image file to send (default: {IMAGE_PATH})")
    parser.add_argument("--ip", default=PICO_IP, help="Pico IP address")
    parser.add_argument("--port", type=int, default=int(PICO_PORT))
    parser.add_argument("--retries", type=int, default=20,
                        help="max upload attempts before giving up")
    parser.add_argument("--retry-delay", type=float, default=2.0,
                        help="seconds between retry attempts")
    parser.add_argument("--connect-timeout", type=float, default=4.0,
                        help="socket connect timeout in seconds")
    parser.add_argument("--ack-timeout", type=float, default=4.0,
                        help="seconds to wait for device ACK after send")
    args = parser.parse_args()

    frame = build_logical_image(args.image)
    for attempt in range(1, args.retries + 1):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(args.connect_timeout)
                sock.connect((args.ip, args.port))
                sock.sendall(frame)

                sock.settimeout(args.ack_timeout)
                ack = sock.recv(16)
                if ack != ACK_BYTES:
                    raise RuntimeError(
                        f"unexpected ACK payload: {ack!r} (expected {ACK_BYTES!r})")

            print(f"Sent {len(frame)} bytes to {args.ip}:{args.port} (ACK ok)")
            return
        except Exception as exc:
            if attempt == args.retries:
                raise RuntimeError(
                    f"upload failed after {args.retries} attempts: {exc}") from exc
            print(f"Attempt {attempt}/{args.retries} failed: {exc}")
            time.sleep(args.retry_delay)


if __name__ == "__main__":
    main()