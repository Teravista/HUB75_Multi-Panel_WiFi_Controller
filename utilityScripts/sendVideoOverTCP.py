"""Convert a video to RGB565, RGB888 or JPEG frames, upload to SD, then loop on the Pico.

    python utilityScripts/sendVideoOverTCP.py clip.mp4 --ip 192.168.1.48 --fps 3
    python utilityScripts/sendVideoOverTCP.py clip.mp4 --prepare-only --output clip.h75v
    python utilityScripts/sendVideoOverTCP.py clip.h75v --ip 192.168.1.48

Port 5002 uses stop-and-wait chunks. SAVED confirms complete SD readback verification.
The PC never keeps the whole converted video in RAM. Audio is not transferred.
"""
from __future__ import annotations

import argparse
import io
from array import array
import sys
from dataclasses import dataclass
from fractions import Fraction
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib

# Edit these conversion defaults; --fps, --seconds and --ping-pong override them.
# --no-ping-pong forces ordinary looping. Prepared files keep their mode unless overridden.
DEFAULT_VIDEO_PATH = ""       # Set a source MP4 path to run without a positional argument.
DEFAULT_PICO_IP = ""          # Blank uses PICO_IP from the local .env file.
DEFAULT_FPS = 30                # Playback/conversion FPS, from 1 to 60.
MAX_VIDEO_SECONDS = 10       # None = whole video; e.g. 20 = first 20 seconds.
PIXEL_FORMAT = "rgb565"      # "rgb565", "rgb888", or "jpeg" for new conversions.
JPEG_QUALITY = 85            # Baseline JPEG quality, 1..95.
PING_PONG = False            # True = forward/backward; False = restart at frame 0.

WIDTH, HEIGHT = 128, 256
PIXEL_BYTES = 3
DESCRIPTOR_VERSION = 1
VIDEO_MEDIA_TYPE = 2
RGB888_FORMAT_ID = 1
PIXEL_FORMAT_IDS = {"rgb888": 1, "rgb565": 2, "jpeg": 3}
JPEG_INDEX = struct.Struct("<III")
JPEG_RECORD = struct.Struct("<II")
MAX_JPEG_BYTES = 128 * 1024
MAXIMUM_FPS = 60
FILE_HEADER_BYTES = 64
FRAME_CHECKSUM_BYTES = 4
CRC32_MASK = 0xFFFFFFFF
BYTES_PER_MIB = 1024 * 1024
MAX_REPLY_BYTES = 128
FRAME_BYTES = WIDTH * HEIGHT * PIXEL_BYTES
CHUNK_BYTES = 1536
BULK_CHUNK_BYTES = 6144
PORT = 5002
DESCRIPTOR = struct.Struct("<4sBBBBHHIIIQ")
REQUEST_BYTES = DESCRIPTOR.size + 4
MAX_FILE_BYTES = 0xFFFFFFFF
PING_PONG_FLAG = 1


@dataclass(frozen=True)
class VideoMetadata:
    frames: int
    fps: Fraction
    ping_pong: bool = False
    pixel_format: str = "rgb888"
    encoded_bytes: int = 0

    # Returns the stored byte size of one raw frame, or zero for JPEG.
    #
    # @param self Instance on which this method operates.
    #
    # @return value - Uncompressed bytes per frame for the selected raw format.
    # @return 0 - JPEG frames have variable encoded lengths.
    @property
    def frame_bytes(self) -> int:
        if self.pixel_format == "jpeg":
            return 0  # Variable encoded frame lengths are described by the index.
        if self.pixel_format == "rgb565":
            pixel_bytes = 2
        else:
            pixel_bytes = 3
        return WIDTH * HEIGHT * pixel_bytes

    # Returns the complete raw or indexed JPEG payload size.
    #
    # @param self Instance on which this method operates.
    #
    # @return value - Total encoded or raw video payload size in bytes.
    @property
    def payload_bytes(self) -> int:
        if self.pixel_format == "jpeg":
            return self.encoded_bytes
        else:
            return self.frames * self.frame_bytes

    # Checks video metadata against supported panel, format, and transfer limits.
    #
    # Rejects unsupported encodings, inconsistent raw/JPEG sizes and rates that
    # cannot fit the wire descriptor or the firmware's FAT32-compatible file limit.
    # Validation completes before conversion or connection can publish any data.
    #
    # @param self Instance on which this method operates.
    def validate(self) -> None:
        if self.pixel_format not in PIXEL_FORMAT_IDS:
            raise ValueError("Pixel format must be rgb888, rgb565 or jpeg")
        if not isinstance(self.ping_pong, bool):
            raise ValueError("Ping-pong must be True or False")
        if not 1 <= self.frames <= 0xFFFFFFFF:
            raise ValueError("Video must contain at least one frame")
        if not 1 <= self.fps <= MAXIMUM_FPS:
            raise ValueError("FPS must be between 1 and 60")
        if self.fps.numerator > 0xFFFFFFFF or self.fps.denominator > 0xFFFFFFFF:
            raise ValueError("FPS numerator/denominator exceeds the metadata format")
        if self.pixel_format == "jpeg":
            if self.encoded_bytes < self.frames * 24 or self.encoded_bytes > MAX_FILE_BYTES-64:
                raise ValueError("Invalid indexed JPEG payload length")
        elif self.encoded_bytes:
            raise ValueError("Raw videos cannot specify an encoded payload size")
        if self.pixel_format == "jpeg":
            frame_checksum_bytes = 0
        else:
            frame_checksum_bytes = self.frames * 4
        if 64 + self.payload_bytes + frame_checksum_bytes > MAX_FILE_BYTES:
            raise ValueError("Converted video exceeds this firmware's FAT32-compatible 4 GiB limit")

    # Serializes validated video metadata into its transfer descriptor.
    #
    # Chooses the raw or indexed-JPEG descriptor version and writes every field
    # explicitly, including rational FPS, loop behavior and the exact payload size.
    # The payload CRC follows separately in prepared files and TCP requests.
    #
    # @param self Instance on which this method operates.
    #
    # @return value - Serialized media descriptor bytes, after validating all metadata fields.
    def encode(self) -> bytes:
        self.validate()
        if self.pixel_format == "jpeg":
            descriptor_version = 2
        else:
            descriptor_version = 1
        if self.ping_pong:
            flags = PING_PONG_FLAG
        else:
            flags = 0
        return DESCRIPTOR.pack(b"H75M", descriptor_version, 2, PIXEL_FORMAT_IDS[self.pixel_format], flags, WIDTH, HEIGHT, self.frames,
                                       self.fps.numerator, self.fps.denominator, self.payload_bytes)

    # Validates and decodes a stored video transfer descriptor.
    #
    # Checks framing, dimensions, format/version agreement and reserved flags
    # before constructing a temporary description. Recomputed payload bounds must
    # agree with the descriptor before callers can use it for storage or upload.
    #
    # @param cls Metadata class used to construct the decoded result.
    # @param data Binary payload or encoded descriptor bytes.
    #
    # @return value - Validated VideoMetadata instance reconstructed from descriptor bytes.
    @classmethod
    def decode(cls, data: bytes) -> VideoMetadata:
        if len(data) != DESCRIPTOR.size:
            raise ValueError("Truncated video descriptor")
        magic, version, kind, fmt, flags, width, height, frames, num, den, payload = DESCRIPTOR.unpack(data)
        invalid_descriptor = (magic, kind, width, height) != (b"H75M", 2, WIDTH, HEIGHT)
        if not invalid_descriptor:
            invalid_descriptor = fmt not in PIXEL_FORMAT_IDS.values()
        if not invalid_descriptor:
            invalid_descriptor = flags & ~PING_PONG_FLAG
        if not invalid_descriptor:
            invalid_descriptor = not den
        if not invalid_descriptor:
            if fmt == 3:
                expected_version = 2
            else:
                expected_version = 1
            invalid_descriptor = version != (expected_version)
        if invalid_descriptor:
            raise ValueError("Unsupported video descriptor")
        if fmt == 3:
            pixel_format = "jpeg"
        else:
            if fmt == 2:
                pixel_format = "rgb565"
            else:
                pixel_format = "rgb888"
        if fmt == 3:
            encoded_bytes = payload
        else:
            encoded_bytes = 0
        metadata = cls(frames, Fraction(num, den), bool(flags & PING_PONG_FLAG),
                               pixel_format,
                               encoded_bytes)
        metadata.validate()
        if payload != metadata.payload_bytes:
            raise ValueError("Video payload length does not match its frame count")
        return metadata


# Finds an explicit, installed or bundled FFmpeg executable.
#
# @param explicit Optional executable path supplied by the user.
#
# @return path - Resolved FFmpeg executable; raises if none is available.
def find_ffmpeg(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    executable = shutil.which("ffmpeg")
    if executable:
        return executable
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except (ImportError, RuntimeError) as error:
        raise RuntimeError("Install utilityScripts/requirements.txt or supply --ffmpeg PATH") from error


# Reads one complete RGB888 frame from FFmpeg output.
#
# Combines short reads until the frame is complete. EOF before any bytes ends conversion; a partial trailing frame is rejected.
#
# @param stream Readable raw-frame stream.
#
# @return bytes - One complete raw RGB888 frame.
# @return b"" - The stream reached EOF before starting another frame.
def read_frame(stream) -> bytes:
    chunks = bytearray()
    while len(chunks) < FRAME_BYTES:
        part = stream.read(FRAME_BYTES - len(chunks))
        if not part:
            if chunks:
                raise RuntimeError("Decoder returned a partial RGB frame")
            return b""
        chunks.extend(part)
    return bytes(chunks)


# Packs complete RGB888 pixels into canonical little-endian RGB565 words.
#
# The uploader preserves logical channel order; panel wiring correction is applied
# on the Pico. Incomplete source pixels are rejected before encoding.
#
# @param frame Complete logical RGB888 source pixels.
#
# @return bytes - Packed two-byte pixels in little-endian R5G6B5 format.
def pack_rgb565(frame: bytes) -> bytes:
    if len(frame) % 3:
        raise ValueError("RGB888 input must contain complete pixels")
    words = array("H", ((frame[i] >> 3) << 11 | (frame[i+1] >> 2) << 5 | frame[i+2] >> 3
                        for i in range(0, len(frame), 3)))
    if sys.byteorder != "little":
        words.byteswap()
    return words.tobytes()

# Converts source video into a disk-backed raw or indexed JPEG upload.
#
# FFmpeg scales and crops each frame on the PC. Only one frame is held at a time; payload CRC and frame count are accumulated before atomically publishing the prepared file. Failures remove only this conversion's temporary output.
#
# @param source Source video path.
# @param output Destination prepared-file path.
# @param fps Requested rational conversion/playback rate.
# @param ffmpeg Optional explicit FFmpeg executable path.
# @param overwrite Whether an existing prepared output may be replaced.
# @param seconds Optional maximum source duration to convert.
# @param ping_pong Whether the converted clip plays forward and backward.
# @param pixel_format Raw RGB888, little-endian RGB565, or indexed baseline JPEG.
# @param jpeg_quality JPEG encoder quality from 1 to 95; ignored for raw formats.
#
# @return metadata - Description of the successfully published prepared file.
def prepare_video(source: Path, output: Path, fps: Fraction, *, ffmpeg: str | None = None,
                  overwrite: bool = False, seconds: float | None = None,
                  ping_pong: bool = False, pixel_format: str = "rgb888",
                  jpeg_quality: int = JPEG_QUALITY) -> VideoMetadata:
    if pixel_format == "jpeg":
        minimum_encoded_bytes = 24
    else:
        minimum_encoded_bytes = 0

    # Validate conversion settings and output ownership before starting the decoder process.
    VideoMetadata(1, fps, ping_pong, pixel_format, minimum_encoded_bytes).validate()
    if not isinstance(jpeg_quality, int) or not 1 <= jpeg_quality <= 95:
        raise ValueError("JPEG quality must be an integer from 1 to 95")
    if seconds is not None and (not 0 < seconds < float("inf")):
        raise ValueError("--seconds must be a positive finite duration")
    if not source.is_file():
        raise FileNotFoundError(source)
    if source.resolve() == output.resolve():
        raise ValueError("Converted output must differ from the source video")
    if output.exists() and not overwrite:
        raise FileExistsError(f"{output} already exists; use --overwrite to replace it")
    output.parent.mkdir(parents=True, exist_ok=True)

    # Scale and crop on the PC; firmware receives only panel-sized frames.
    filters = (f"fps={fps.numerator}/{fps.denominator},"
               f"scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=increase,"
               f"crop={WIDTH}:{HEIGHT}")
    command = [find_ffmpeg(ffmpeg), "-hide_banner", "-loglevel", "error", "-nostdin", "-i", str(source),
               "-map", "0:v:0", "-vf", filters, "-an", "-sn", "-dn"]
    if seconds is not None:
        command += ["-t", str(seconds)]
    command += ["-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"]
    temporary: Path | None = None
    process = None
    try:
        with tempfile.TemporaryFile() as jpeg_records, \
                tempfile.NamedTemporaryFile(dir=output.parent, suffix=".part", delete=False) as converted, \
                tempfile.TemporaryFile() as diagnostics:
            temporary = Path(converted.name)
            converted.write(bytes(REQUEST_BYTES))
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=diagnostics)

            # Keep frame payloads on disk while collecting the metadata needed for the final descriptor.
            count = checksum = 0
            jpeg_offsets = []
            try:
                while frame := read_frame(process.stdout):
                    if pixel_format == "rgb565":
                        frame = pack_rgb565(frame)
                    count += 1

                    # Store independent JPEG records; raw formats stream directly to the prepared payload.
                    if pixel_format == "jpeg":
                        from PIL import Image
                        encoded = io.BytesIO()
                        Image.frombytes("RGB", (WIDTH, HEIGHT), frame).save(encoded, format="JPEG",
                            quality=jpeg_quality, subsampling=2, progressive=False, optimize=False)
                        frame = encoded.getvalue()
                        if len(frame) > MAX_JPEG_BYTES:
                            raise ValueError("JPEG frame exceeds firmware's 128 KiB bound")
                        jpeg_offsets.append((jpeg_records.tell(), len(frame)))
                        jpeg_records.write(JPEG_RECORD.pack(len(frame), zlib.crc32(frame) & 0xffffffff))
                        jpeg_records.write(frame)
                        VideoMetadata(count, fps, ping_pong, pixel_format,
                                      count * JPEG_INDEX.size + jpeg_records.tell()).validate()
                    else:
                        VideoMetadata(count, fps, ping_pong, pixel_format).validate()
                        checksum = zlib.crc32(frame, checksum)
                        converted.write(frame)
                return_code = process.wait()
            finally:
                process.stdout.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
            if return_code:
                diagnostics.seek(0, os.SEEK_END)
                diagnostics.seek(max(0, diagnostics.tell() - 4096))
                raise RuntimeError("Video conversion failed: " + diagnostics.read().decode(errors="replace").strip())

            # Write the completed index ahead of its temporary JPEG records and checksum both parts.
            if pixel_format == "jpeg":
                metadata = VideoMetadata(count, fps, ping_pong, pixel_format,
                                         count * JPEG_INDEX.size + jpeg_records.tell())
                metadata.validate()
                for index, (offset, length) in enumerate(jpeg_offsets):
                    offset += count * JPEG_INDEX.size
                    check = zlib.crc32(struct.pack("<III", offset, length, index)) & 0xffffffff
                    entry = JPEG_INDEX.pack(offset, length, check)
                    converted.write(entry)
                    checksum = zlib.crc32(entry, checksum)
                jpeg_records.seek(0)
                while part := jpeg_records.read(1024*1024):
                    converted.write(part)
                    checksum = zlib.crc32(part, checksum)
            else:
                metadata = VideoMetadata(count, fps, ping_pong, pixel_format)
            converted.seek(0)
            converted.write(metadata.encode() + struct.pack("<I", checksum & 0xFFFFFFFF))

        # Publish the prepared file only after conversion and its metadata have completed successfully.
        if overwrite:
            os.replace(temporary, output)
        else:
            # Exclusive creation also protects against an output appearing during conversion.
            try:
                with output.open("xb") as destination, temporary.open("rb") as prepared:
                    shutil.copyfileobj(prepared, destination, 1024 * 1024)
            except FileExistsError:
                raise
            except BaseException:
                output.unlink(missing_ok=True)
                raise
            temporary.unlink()
        temporary = None
        return metadata
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


# Checks a prepared video descriptor, file size and payload CRC.
#
# The complete payload is streamed from disk to detect corruption before establishing a network connection.
#
# @param path Prepared file to inspect or upload.
#
# @return pair - Validated metadata and its canonical descriptor/checksum request.
def inspect_prepared(path: Path) -> tuple[VideoMetadata, bytes]:
    with path.open("rb") as prepared:
        request = prepared.read(REQUEST_BYTES)
        if len(request) != REQUEST_BYTES:
            raise ValueError("Truncated prepared video")
        metadata = VideoMetadata.decode(request[:DESCRIPTOR.size])
        if path.stat().st_size != REQUEST_BYTES + metadata.payload_bytes:
            raise ValueError("Prepared video's file size does not match its metadata")
        checksum = 0
        while part := prepared.read(1024 * 1024):
            checksum = zlib.crc32(part, checksum)
        if checksum & 0xFFFFFFFF != struct.unpack("<I", request[DESCRIPTOR.size:])[0]:
            raise ValueError("Prepared video checksum mismatch; convert the source again")
        if metadata.pixel_format == "jpeg":
            inspect_jpeg_records(prepared, metadata)
    return metadata, request


# Validates every indexed JPEG record and its encoded frame checksum.
#
# Requires contiguous offsets, valid index and record CRCs, expected dimensions
# and baseline JPEG coding. Pillow fully decodes each bounded frame; malformed
# input raises before any network connection or Pico storage transaction begins.
#
# @param prepared Open prepared-file stream supporting read and seek.
# @param metadata Frame count and exact indexed-JPEG payload length.
def inspect_jpeg_records(prepared, metadata: VideoMetadata) -> None:
    from PIL import Image

    # Track the next permitted record offset so aliases, gaps and trailing data are rejected.
    expected = metadata.frames * JPEG_INDEX.size
    for index in range(metadata.frames):
        prepared.seek(REQUEST_BYTES + index*JPEG_INDEX.size)
        offset, length, check = JPEG_INDEX.unpack(prepared.read(JPEG_INDEX.size))
        if offset != expected or not 4 <= length <= MAX_JPEG_BYTES or offset+8+length > metadata.payload_bytes:
            raise ValueError("Invalid JPEG frame offset/length")
        if check != zlib.crc32(struct.pack("<III",offset,length,index)) & 0xffffffff:
            raise ValueError("JPEG index checksum mismatch")

        # Cross-check the selected record length and encoded CRC before decoding its pixels.
        prepared.seek(REQUEST_BYTES + offset)
        count, crc = JPEG_RECORD.unpack(prepared.read(JPEG_RECORD.size))
        data = prepared.read(length)
        if count != length or len(data) != length or zlib.crc32(data) & 0xffffffff != crc or not data.endswith(b"\xff\xd9"):
            raise ValueError("JPEG frame checksum/length mismatch")

        # Require a baseline frame with the expected dimensions, then force full image decoding.
        with Image.open(io.BytesIO(data)) as image:
            if image.format != "JPEG" or image.size != (WIDTH,HEIGHT) or image.info.get("progressive"):
                raise ValueError("JPEG frames must be baseline 128x256")
            image.load()
        expected += JPEG_RECORD.size + length
    if expected != metadata.payload_bytes:
        raise ValueError("JPEG payload has extra or missing records")

# Reads one bounded newline-terminated protocol response.
#
# Reports disconnects and unexpected responses instead of claiming save success.
#
# @param sock Connected TCP socket.
# @param expected Required response, or None to inspect the received line.
#
# @return bytes - Complete response matching expected when supplied; failures raise.
def read_reply(sock, expected: bytes | None) -> bytes:
    reply = bytearray()
    while len(reply) < MAX_REPLY_BYTES:
        byte = sock.recv(1)
        if not byte:
            raise ConnectionError("Pico disconnected before confirming the video transaction")
        reply.extend(byte)
        if byte == b"\n":
            if expected is not None and reply != expected:
                raise RuntimeError(f"Pico replied {bytes(reply)!r}; expected {expected!r}")
            return bytes(reply)
    raise RuntimeError("Invalid oversized Pico reply")


# Negotiates a bounded chunk upload with the Pico.
#
# Prefers 6144-byte chunks and can fall back to the legacy 1536-byte handshake before payload transfer. No media bytes are sent until the READY response.
#
# @param host Pico address on the local network.
# @param port TCP video listener port.
# @param timeout Socket-operation deadline in seconds.
# @param request Canonical metadata descriptor followed by its payload CRC32.
# @param legacy_chunks Whether to force the smaller legacy chunk handshake.
#
# @return pair - Connected socket and negotiated chunk byte count.
def connect_video(host: str, port: int, timeout: float, request: bytes,
                  legacy_chunks: bool) -> tuple[socket.socket, int]:
    modes = [True, False]
    if legacy_chunks:
        modes = [False]

    # Try legacy negotiation only after explicit bulk rejection, before transmitting any payload.
    for bulk in modes:
        wire_request = request
        expected_reply = b"READY\n"
        chunk_bytes = CHUNK_BYTES
        if bulk:
            wire_request = b"H75V" + request[4:]
            expected_reply = b"READY 6144\n"
            chunk_bytes = BULK_CHUNK_BYTES
        sock = socket.create_connection((host, port), timeout=timeout)
        try:
            sock.settimeout(timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.sendall(wire_request)
            reply = read_reply(sock, None)
            # Fallback is allowed only before payload, after explicit rejection.
            if bulk and reply == b"ERR METADATA\n":
                sock.close()
                continue
            if reply != expected_reply:
                raise RuntimeError(f"Pico replied {reply!r}; expected {expected_reply!r}")
            return sock, chunk_bytes
        except BaseException:
            sock.close()
            raise
    raise RuntimeError("Video negotiation failed")


# Uploads a prepared clip and waits for verified SD publication.
#
# Checks the local prepared file, applies an optional loop override only to the request descriptor and delegates ordered chunk transfer. SAVED confirms the Pico completed its own SD readback.
#
# @param path Prepared file to inspect or upload.
# @param host Pico address on the local network.
# @param port TCP video listener port.
# @param timeout Socket-operation deadline in seconds.
# @param verification_timeout SD verification wait deadline in seconds.
# @param progress Callback receiving human-readable progress messages.
# @param legacy_chunks Whether to force the smaller legacy chunk handshake.
# @param ping_pong Loop mode override; None preserves a prepared file setting.
#
# @return metadata - Uploaded clip description after the Pico confirms SAVED.
def upload_prepared(path: Path, host: str, port: int = PORT, *, timeout: float = 60,
                    verification_timeout: float = 1800, progress=print,
                    legacy_chunks: bool = False, ping_pong: bool | None = None) -> VideoMetadata:

    # Validate local data before connecting so corrupt prepared files never start a Pico upload.
    metadata, request = inspect_prepared(path)
    if not host or not 1 <= port <= 65535 or timeout <= 0 or verification_timeout <= 0:
        raise ValueError("A Pico address, valid port and positive timeouts are required")
    start = time.monotonic()
    with path.open("rb") as prepared:
        # Detect replacement between inspection and transfer before sending a request.
        if prepared.read(REQUEST_BYTES) != request:
            raise ValueError("Prepared video changed before upload")
        # Override only the wire descriptor; pixels/CRC and the prepared file
        # stay unchanged. None preserves the mode already in a prepared clip.
        if ping_pong is not None:
            metadata = VideoMetadata(metadata.frames, metadata.fps, ping_pong,
                                 metadata.pixel_format, metadata.encoded_bytes)
            request = metadata.encode() + request[DESCRIPTOR.size:]
        if metadata.ping_pong:
            conditional_value_21 = 'ping-pong'
        else:
            conditional_value_21 = 'loop'
        progress(f"Video playback: {conditional_value_21}")
        sock, chunk_bytes = connect_video(host, port, timeout, request, legacy_chunks)
        with sock:
            return transfer_video(sock, prepared, metadata, chunk_bytes, verification_timeout, progress, start)


# Sends acknowledged chunks and waits for SD verification.
#
# Waits for NEXT after each nonfinal chunk and VERIFY after the final payload. A separate, longer deadline covers SD verification; an unexpected or failed response never reports success.
#
# @param sock Connected TCP socket.
# @param prepared Open prepared payload stream.
# @param metadata Validated clip metadata.
# @param chunk_bytes Negotiated transfer chunk size.
# @param verification_timeout SD verification wait deadline in seconds.
# @param progress Callback receiving human-readable progress messages.
# @param start Overall upload start time.
#
# @return metadata - Clip description after complete transfer and SD verification acknowledgement.
def transfer_video(sock, prepared, metadata, chunk_bytes, verification_timeout, progress, start):
    progress(f"Video transfer chunks: {chunk_bytes} bytes")
    upload_started = time.monotonic()
    sent = 0
    last_percent = -1
    while sent < metadata.payload_bytes:
        part = prepared.read(min(chunk_bytes, metadata.payload_bytes - sent))
        if not part:
            raise ValueError("Prepared video was truncated during upload")
        # Local reads normally fill the chunk; enforce protocol length if they don't.
        needed = min(chunk_bytes, metadata.payload_bytes - sent)
        while len(part) < needed:
            extra = prepared.read(needed - len(part))
            if not extra:
                raise ValueError("Prepared video was truncated during upload")
            part += extra
        sock.sendall(part)
        sent += len(part)
        if sent == metadata.payload_bytes:
            conditional_value_31 = b"VERIFY\n"
        else:
            conditional_value_31 = b"NEXT\n"
        read_reply(sock, conditional_value_31)
        percent = sent * 100 // metadata.payload_bytes
        if percent >= last_percent + 5 or percent == 100:
            progress(f"Upload {percent}% ({sent / BYTES_PER_MIB:.1f} MiB)")
            last_percent = percent
    verification_started = time.monotonic()
    upload_seconds = verification_started - upload_started
    rate = metadata.payload_bytes / 1024 / max(upload_seconds, 0.000001)
    progress(f"All frames received in {upload_seconds:.1f}s ({rate:.1f} KiB/s); "
             "Pico is verifying the SD copy...")
    sock.settimeout(verification_timeout)
    read_reply(sock, b"SAVED\n")
    progress(f"SD verification completed in {time.monotonic() - verification_started:.1f}s.")
    progress(f"Saved and verified: {metadata.frames} frames at {float(metadata.fps):g} FPS "
             f"in {time.monotonic() - start:.1f}s. The Pico will loop the saved video.")
    return metadata


# Reads conversion defaults and command-line overrides, then prepares or uploads.
#
# Source path, IP, FPS, maximum duration and ping-pong defaults are configurable near the top. Prepared files retain their encoded settings unless an explicit loop override is requested.
def main() -> None:
    from dotenv import load_dotenv
    load_dotenv(Path(__file__).resolve().parents[1] / ".env")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    default_video = None
    if DEFAULT_VIDEO_PATH:
        default_video = Path(DEFAULT_VIDEO_PATH)
    default_ip = DEFAULT_PICO_IP
    if not default_ip:
        default_ip = os.environ.get("PICO_IP")
    parser.add_argument("video", nargs="?", type=Path, default=default_video,
                        help="Source video or prepared .h75v; defaults to DEFAULT_VIDEO_PATH")
    parser.add_argument("--ip", default=default_ip, help="Pico address; defaults to .env PICO_IP")
    parser.add_argument("--port", type=int, default=PORT, help="Video port, separate from photo TCP port")
    parser.add_argument("--fps", type=Fraction, default=Fraction(DEFAULT_FPS),
                        help=f"Converted FPS, 1-60 (default {DEFAULT_FPS}); accepts 30000/1001")
    parser.add_argument("--output", type=Path, help="Keep converted .h75v here; otherwise use a temporary file")
    parser.add_argument("--prepare-only", action="store_true", help="Convert without connecting to a Pico")
    parser.add_argument("--overwrite", action="store_true", help="Replace an existing converted output")
    if MAX_VIDEO_SECONDS is not None:
        conditional_value_41 = MAX_VIDEO_SECONDS
    else:
        conditional_value_41 = 'whole video'
    parser.add_argument("--seconds", type=float, default=MAX_VIDEO_SECONDS,
                            help=f"Convert at most N seconds (default: {conditional_value_41})")
    parser.add_argument("--ffmpeg", help="Optional path to FFmpeg executable")
    parser.add_argument("--timeout", type=float, default=60, help="Per-chunk reply timeout")
    parser.add_argument("--verification-timeout", type=float, default=1800, help="Final verification reply timeout")
    parser.add_argument("--legacy-chunks", action="store_true", help="Force 1536-byte chunks for older firmware")
    parser.add_argument("--ping-pong", action=argparse.BooleanOptionalAction, default=None,
                        help=f"Play forward then backward; --no-ping-pong restarts at frame 0 "
                             f"(source default: {PING_PONG}; prepared files keep their saved mode)")
    parser.add_argument("--pixel-format", choices=tuple(PIXEL_FORMAT_IDS), default=None,
                        help=f"Source format (default: {PIXEL_FORMAT}); prepared files retain their format")
    parser.add_argument("--jpeg-quality", type=int, default=JPEG_QUALITY,
                        help=f"Baseline JPEG quality, 1..95 (default {JPEG_QUALITY})")
    args = parser.parse_args()
    if args.video is None:
        parser.error("Provide a video path or set DEFAULT_VIDEO_PATH at the top of this script")
    if not args.prepare_only and not args.ip:
        parser.error("Set .env PICO_IP or supply --ip")

    # Prepared files retain their encoding; source conversion options must not reinterpret their bytes.
    if args.video.suffix.lower() == ".h75v":
        metadata, _ = inspect_prepared(args.video)
        if args.pixel_format is not None and args.pixel_format != metadata.pixel_format:
            parser.error("Prepared video retains its format; convert the source again to change it")
        print(f"Prepared video: {metadata.frames} frames, {float(metadata.fps):g} FPS")
        if not args.prepare_only:
            upload_prepared(args.video, args.ip, args.port, timeout=args.timeout,
                            verification_timeout=args.verification_timeout, legacy_chunks=args.legacy_chunks,
                            ping_pong=args.ping_pong)
        return

    # Keep an explicitly requested prepared file, otherwise remove the temporary conversion after upload.
    if args.output or args.prepare_only:
        output = args.output or args.video.with_suffix(".h75v")
        if args.ping_pong is None:
            conditional_value_51 = PING_PONG
        else:
            conditional_value_51 = args.ping_pong
        metadata = prepare_video(args.video, output, args.fps, ffmpeg=args.ffmpeg,
                                         overwrite=args.overwrite, seconds=args.seconds,
                                         ping_pong=conditional_value_51,
                                         pixel_format=args.pixel_format or PIXEL_FORMAT,
                                         jpeg_quality=args.jpeg_quality)
        print(f"Prepared {output}: {metadata.frames} frames, {metadata.payload_bytes / BYTES_PER_MIB:.1f} MiB")
        if not args.prepare_only:
            upload_prepared(output, args.ip, args.port, timeout=args.timeout,
                            verification_timeout=args.verification_timeout, legacy_chunks=args.legacy_chunks)
    else:
        with tempfile.TemporaryDirectory(prefix="hub75-video-") as directory:
            output = Path(directory) / "video.h75v"
            if args.ping_pong is None:
                conditional_value_61 = PING_PONG
            else:
                conditional_value_61 = args.ping_pong
            metadata = prepare_video(args.video, output, args.fps, ffmpeg=args.ffmpeg, seconds=args.seconds,
                                                 ping_pong=conditional_value_61,
                                                 pixel_format=args.pixel_format or PIXEL_FORMAT,
                                                 jpeg_quality=args.jpeg_quality)
            print(f"Prepared {metadata.frames} frames at {float(args.fps):g} FPS; uploading...")
            upload_prepared(output, args.ip, args.port, timeout=args.timeout,
                            verification_timeout=args.verification_timeout, legacy_chunks=args.legacy_chunks)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(f"Video upload failed: {error}") from error
