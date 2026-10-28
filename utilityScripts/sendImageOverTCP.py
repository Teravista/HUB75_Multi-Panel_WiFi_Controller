"""
Scale an image to the panel's logical size and send raw RGB over TCP.

Usage example:
    python utilityScripts/sendImageOverTCP.py "sample_image.jpg"
"""

import argparse
import os
import socket
import time
import json
import struct
import zlib
from pathlib import Path
from urllib.request import Request, urlopen

from dotenv import load_dotenv
from PIL import Image, ImageOps

load_dotenv()

PICO_IP = os.environ.get("PICO_IP")
PICO_PORT = os.environ.get("PICO_PORT", "5001")
LOGICAL_WIDTH = 128
LOGICAL_HEIGHT = 256
IMAGE_PATH = "imagesVideos\\private\\apple.jpg"
ACK_BYTES = b"ACK\n"
SIZING_MODE = "crop"  # "fit" saves only the fitted area through the HTTP photo library.
HTTP_PORT = 80


# Crops and resizes a photo into a full-panel RGB image.
#
# @param image_path Path to the source image.
#
# @return value - Full-panel RGB888 image bytes prepared for the photo upload protocol.
def build_logical_image(image_path: str) -> bytes:
    image = Image.open(image_path).convert("RGB")
    image = ImageOps.fit(
        image,
        (LOGICAL_WIDTH, LOGICAL_HEIGHT),
        method=Image.Resampling.LANCZOS,
        centering=(0.5, 0.5),
    )
    return image.tobytes()  # row-major RGB, 3 bytes per pixel


# Encodes a fitted photo with placement metadata and a library thumbnail.
#
# @param image_path Path to the source image.
#
# @return value - Prepared photo descriptor and pixel payload for the fitted image rectangle.
def prepare_fitted_photo(image_path: str):
    with Image.open(image_path) as source:
        source = ImageOps.exif_transpose(source).convert("RGBA")
        scale = min(LOGICAL_WIDTH/source.width, LOGICAL_HEIGHT/source.height)
        size = (max(1, round(source.width*scale)), max(1, round(source.height*scale)))
        scaled = source.resize(size, Image.Resampling.LANCZOS)
        fitted = Image.new("RGB", size, "black")
        fitted.paste(scaled, mask=scaled.getchannel("A"))
    payload = fitted.tobytes()
    preview = Image.new("RGB", (LOGICAL_WIDTH, LOGICAL_HEIGHT), "black")
    preview.paste(fitted, ((LOGICAL_WIDTH-size[0])//2, (LOGICAL_HEIGHT-size[1])//2))
    thumbnail = bytearray(1024)
    for y in range(32):
        for x in range(16):
            r,g,b = preview.getpixel((x*8+4,y*8+4))
            struct.pack_into("<H", thumbnail, (y*16+x)*2, (r>>3)<<11 | (g>>2)<<5 | b>>3)
    descriptor = struct.pack("<4sBBBBHHIIIQI", b"H75M",1,1,1,0,*size,1,0,1,len(payload),zlib.crc32(payload))
    name = Path(image_path).name.encode("ascii", "replace")[:63].ljust(64,b"\0")
    return descriptor + name + thumbnail, payload


# Uploads a prepared fitted photo and waits for the saved result.
#
# @param ip Pico local IPv4 address.
# @param port TCP upload port on the Pico.
# @param prepared Prepared media path or open prepared media stream.
# @param timeout Network request or connection timeout, in seconds.
def send_fitted_photo(ip, port, prepared, timeout=120):
    """Use the same saved-photo transaction as the web page, including SD verification."""
    header, payload = prepared
    # Sends a fitted-photo upload request and validates its JSON reply.
    #
    # @param op Protocol operation identifier.
    # @param session Upload session identifier returned by the Pico.
    # @param offset Byte offset within the prepared payload or requested upload.
    # @param data Binary payload or encoded descriptor bytes.
    #
    # @return value - Validated API response object; HTTP, transport and controller errors raise
    #                 an exception.
    def request(op, session=0, offset=0, data=b""):
        body = struct.pack("<BII",op,session,offset) + data
        query = Request(f"http://{ip}:{port}/api/video",data=body,
                        headers={"Content-Type":"application/octet-stream"})
        with urlopen(query,timeout=30) as response:
            result = json.load(response)
        if result.get("error"):
            raise RuntimeError(result["error"])
        return result
    session = 0
    try:
        ack = request(5,data=header)
        session = ack["id"]
        if ack["state"] != "receiving" or ack["total"] != len(payload) or ack["offset"]:
            raise RuntimeError("Photo start was not confirmed")
        for offset in range(0,len(payload),6144):
            chunk = payload[offset:offset+6144]
            ack = request(2,session,offset,chunk)
            if ack["offset"] != offset+len(chunk):
                raise RuntimeError("Photo chunk was not confirmed")
        deadline = time.monotonic()+timeout
        while ack["state"] == "verifying":
            if time.monotonic() >= deadline:
                raise TimeoutError("Photo SD verification timed out")
            time.sleep(0.1)
            ack = request(3,session)
        if ack["state"] != "saved":
            raise RuntimeError("Photo save was not confirmed")
        print(f"Saved and verified: photo {ack['photoId']}, {len(payload)} bytes, fitted with black borders")
    except BaseException:
        if session:
            try:
                request(4,session)
            except Exception:
                pass
        raise


# Prepares and uploads the selected photo using the requested transport.
def main() -> None:

    # Collect command-line overrides before applying the editable defaults.
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
    parser.add_argument("--sizing", choices=("crop", "fit"), default=SIZING_MODE)
    parser.add_argument("--http-port", type=int, default=HTTP_PORT)
    args = parser.parse_args()

    if not args.ip:
        parser.error("Set PICO_IP in the environment/.env or supply --ip")
    if args.sizing == "fit":
        send_fitted_photo(args.ip, args.http_port, prepare_fitted_photo(args.image))
        return
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
