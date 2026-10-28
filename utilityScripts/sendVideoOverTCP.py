"""Convert a video to JPEG/RGB888/RGB565 frames, upload to SD, then loop on the Pico.

    python utilityScripts/sendVideoOverTCP.py
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
from dataclasses import dataclass, replace
from fractions import Fraction
import os
from pathlib import Path
import shutil
import socket
import struct
import sys
import subprocess
import tempfile
import time
import zlib

# Edit these defaults to run this script without arguments. Relative default
# paths start at the repository folder; an explicit CLI path uses your current folder.
DEFAULT_VIDEO_PATH = ""       # Set a source path to run without a positional argument.
DEFAULT_PICO_IP = ""          # Blank uses .env/environment PICO_IP.
DEFAULT_VIDEO_NAME = ""          # Blank/None = source filename; e.g. "Forest walk".
# --fps, --seconds and --ping-pong override the conversion defaults below.
# --no-ping-pong forces ordinary looping. Prepared files keep their mode unless overridden.
DEFAULT_FPS = 30                # Playback/conversion FPS, from 1 to 60.
MAX_VIDEO_SECONDS = 600       # None = whole video; e.g. 20 = first 20 seconds.
PING_PONG = False            # True = forward/backward; False = restart at frame 0.
PIXEL_FORMAT = "rgb565"       # "rgb565", "rgb888", or "jpeg" for new conversions.
SIZING_MODE = "crop"          # "crop" fills screen; "fit" keeps the whole image with black borders.
JPEG_QUALITY = 85             # Baseline JPEG quality for newly converted JPEG clips, 1..95.

WIDTH, HEIGHT = 128, 256
FRAME_BYTES = WIDTH * HEIGHT * 3
CHUNK_BYTES = 1536
BULK_CHUNK_BYTES = 6144
PORT = 5002
DESCRIPTOR = struct.Struct("<4sBBBBHHIIIQ")
REQUEST_BYTES = DESCRIPTOR.size + 4
MAX_FILE_BYTES = 0xFFFFFFFF
PING_PONG_FLAG = 1
PIXEL_FORMAT_IDS = {"rgb888": 1, "rgb565": 2, "jpeg": 3}
JPEG_INDEX = struct.Struct("<III")
JPEG_RECORD = struct.Struct("<II")
MAX_JPEG_BYTES = 128 * 1024


@dataclass(frozen=True)
class VideoMetadata:
    frames: int
    fps: Fraction
    ping_pong: bool = False
    pixel_format: str = "rgb888"
    encoded_bytes: int = 0
    jpeg_quality: int = 0  # Unknown for previously prepared JPEG clips.

    width: int = WIDTH
    height: int = HEIGHT
    offset_x: int | None = None
    offset_y: int | None = None

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
            bytes_per_pixel = 2
        else:
            bytes_per_pixel = 3
        return self.width * self.height * (bytes_per_pixel)

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
        if any(not isinstance(n, int) or isinstance(n, bool) for n in (self.width,self.height)) or not 1 <= self.width <= WIDTH or not 1 <= self.height <= HEIGHT:
            raise ValueError("Stored dimensions must fit within 128x256")
        if self.offset_x is not None or self.offset_y is not None:
            if any(not isinstance(n,int) or isinstance(n,bool) for n in (self.offset_x,self.offset_y)) or not 0 <= self.offset_x <= WIDTH-self.width or not 0 <= self.offset_y <= HEIGHT-self.height:
                raise ValueError("Stored placement must stay inside the panel")
        if self.pixel_format not in PIXEL_FORMAT_IDS:
            raise ValueError("Pixel format must be rgb888, rgb565 or jpeg")
        if not isinstance(self.jpeg_quality, int) or isinstance(self.jpeg_quality, bool) or not 0 <= self.jpeg_quality <= 95:
            raise ValueError("Stored JPEG quality must be an integer from 0 (unknown) to 95")
        if self.pixel_format != "jpeg" and self.jpeg_quality:
            raise ValueError("Raw videos cannot specify JPEG quality")
        if not isinstance(self.ping_pong, bool):
            raise ValueError("Ping-pong must be True or False")
        if not 1 <= self.frames <= 0xFFFFFFFF:
            raise ValueError("Video must contain at least one frame")
        if not 1 <= self.fps <= 60:
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
        if 64 + self.payload_bytes + (frame_checksum_bytes) > MAX_FILE_BYTES:
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
            if self.jpeg_quality:
                version = 3
            else:
                version = 2
        else:
            version = 1
        if self.offset_x is not None:
            version = 4
        if version != 4:
            width = self.width
        else:
            width = (self.width-1) | (self.offset_x<<7)
        if version != 4:
            height = self.height
        else:
            height = (self.height-1) | (self.offset_y<<8)
        if self.ping_pong:
            loop_flags = PING_PONG_FLAG
        else:
            loop_flags = 0
        flags = (self.jpeg_quality << 1) | (loop_flags)
        return DESCRIPTOR.pack(b"H75M", version, 2, PIXEL_FORMAT_IDS[self.pixel_format], flags, width, height, self.frames,
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
        invalid_descriptor = (magic, kind) != (b"H75M", 2)
        if not invalid_descriptor:
            invalid_descriptor = fmt not in PIXEL_FORMAT_IDS.values()
        if not invalid_descriptor:
            invalid_descriptor = version not in (3,4) and flags & ~PING_PONG_FLAG
        if not invalid_descriptor:
            invalid_descriptor = not den
        if not invalid_descriptor:
            if fmt == 3:
                supported_versions = (2,3,4)
            else:
                supported_versions = (1,4)
            invalid_descriptor = version not in (supported_versions)
        if invalid_descriptor:
                    raise ValueError("Unsupported video descriptor")
        offset_x = offset_y = None
        if version == 4:
            if width & 0xc000:
                raise ValueError("Invalid packed placement")
            offset_x,offset_y = width>>7,height>>8
            width,height = (width & 127)+1,(height & 255)+1
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
        if version in (3,4):
            jpeg_quality = flags >> 1
        else:
            jpeg_quality = 0
        metadata = cls(frames, Fraction(num, den), bool(flags & PING_PONG_FLAG),
                               pixel_format,
                               encoded_bytes, jpeg_quality, width, height, offset_x, offset_y)
        metadata.validate()
        if payload != metadata.payload_bytes:
            raise ValueError("Video payload length does not match its frame count")
        return metadata


# Locates an FFmpeg executable for video frame extraction.
#
# @param explicit Optional explicit FFmpeg executable path.
#
# @return value - Path to the selected FFmpeg executable; raises an error when it cannot be
#                 found.
def find_ffmpeg_executable(explicit: str | None = None) -> str:
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


# Reads exactly one raw video frame from the conversion process.
#
# @param stream Open binary stream from which frame bytes are read.
# @param frame_bytes Exact number of RGB888 bytes expected for one raw frame.
#
# @return bytes - A complete RGB888 frame; truncated frames raise an exception.
# @return b"" - The stream ended cleanly before the next frame started.
def read_rgb888_frame(stream, frame_bytes: int = FRAME_BYTES) -> bytes:
    chunks = bytearray()
    while len(chunks) < frame_bytes:
        part = stream.read(frame_bytes - len(chunks))
        if not part:
            if chunks:
                raise RuntimeError("Decoder returned a partial RGB frame")
            return b""
        chunks.extend(part)
    return bytes(chunks)


# Reads a fitted frame and captures its dimensions from the frame stream.
#
# @param stream Open binary stream from which frame bytes are read.
#
# @return tuple - Frame bytes, fitted width and fitted height in pixels.
# @return None - The stream ended cleanly before the next fitted frame.
def read_fitted_rgb888_frame(stream):
    """Read one FFmpeg P6 frame with its scaled dimensions, without buffering a video."""
    header_bytes = 0
    # Reads the next non-comment token from the fitted-frame stream.
    #
    # @return bytes - Next non-comment token from the fitted-frame stream.
    # @return b"" - No further token is available at a clean end of input.
    def token():
        nonlocal header_bytes
        value = bytearray()
        while True:
            byte = stream.read(1)
            if not byte:
                if value:
                    raise RuntimeError("Truncated fitted frame header")
                return b""
            header_bytes += 1
            if header_bytes > 1024:
                raise RuntimeError("Oversized fitted frame header")
            if byte == b"#" and not value:
                while byte and byte != b"\n":
                    byte = stream.read(1)
                    header_bytes += 1
                    if header_bytes > 1024:
                        raise RuntimeError("Oversized fitted frame header")
                continue
            if byte in b" \t\r\n":
                if value:
                    return bytes(value)
            else:
                value.extend(byte)
    magic = token()
    if not magic:
        return None
    if magic != b"P6":
        raise RuntimeError("Unsupported fitted frame encoding")
    width, height, maximum = int(token()), int(token()), int(token())
    if not 1 <= width <= WIDTH or not 1 <= height <= HEIGHT or maximum != 255:
        raise RuntimeError("Invalid fitted frame dimensions")
    frame = read_rgb888_frame(stream, width*height*3)
    if not frame:
        raise RuntimeError("Missing fitted frame pixels")
    return frame, width, height


# Packs RGB888 frame bytes into little-endian RGB565 pixels.
#
# @param frame Raw RGB888 frame or encoded JPEG frame being processed.
#
# @return value - Little-endian RGB565 pixel bytes converted from the RGB888 source.
def convert_rgb888_to_rgb565(frame: bytes) -> bytes:
    """Canonical RGB -> little-endian R5G6B5, without changing wiring correction."""
    if len(frame) % 3:
        raise ValueError("RGB888 input must contain complete pixels")
    words = array("H", ((frame[i] >> 3) << 11 | (frame[i+1] >> 2) << 5 | frame[i+2] >> 3
                        for i in range(0, len(frame), 3)))
    if sys.byteorder != "little":
        words.byteswap()
    return words.tobytes()


# Converts source video into a bounded-memory prepared media file.
#
# @param source Source media object, path or open byte stream used by this operation.
# @param output Destination path for the prepared video file.
# @param fps Requested video frames per second.
# @param ffmpeg Optional path to the FFmpeg executable.
# @param overwrite Whether an existing prepared output file may be replaced.
# @param seconds Maximum source duration in seconds; None extracts the available video without
#                a duration cap.
# @param ping_pong Whether playback reverses at its endpoints; None preserves the prepared
#                  setting.
# @param pixel_format Stored pixel format: rgb888, rgb565 or jpeg.
# @param jpeg_quality JPEG encoder quality, from 1 to 95.
# @param sizing Aspect-ratio mode: crop fills the panel; fit preserves the entire source
#               image.
#
# @return value - Descriptor of the validated prepared video written to the output file.
def prepare_video(source: Path, output: Path, fps: Fraction, *, ffmpeg: str | None = None,
                  overwrite: bool = False, seconds: float | None = None,
                  ping_pong: bool = False, pixel_format: str = "rgb888",
                  jpeg_quality: int = JPEG_QUALITY, sizing: str = "crop") -> VideoMetadata:
    if sizing not in ("crop", "fit"):
        raise ValueError("Sizing must be crop or fit")
    if pixel_format == "jpeg":
        minimum_jpeg_payload = 24
    else:
        minimum_jpeg_payload = 0
    VideoMetadata(1, fps, ping_pong, pixel_format, minimum_jpeg_payload).validate()
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
    if sizing == 'fit':
        scale_direction = 'decrease'
    else:
        scale_direction = 'increase'
    if sizing == "fit":
        placement_filter = ",setsar=1"
    else:
        placement_filter = f",crop={WIDTH}:{HEIGHT}"
    filters = (f"fps={fps.numerator}/{fps.denominator},"
                   f"scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio={scale_direction}"
                   + (placement_filter))
    command = [find_ffmpeg_executable(ffmpeg), "-hide_banner", "-loglevel", "error", "-nostdin", "-i", str(source),
               "-map", "0:v:0", "-vf", filters, "-an", "-sn", "-dn"]
    if seconds is not None:
        command += ["-t", str(seconds)]
    command += ["-pix_fmt", "rgb24"]
    if sizing == "fit":
        output_options = ["-c:v", "ppm", "-f", "image2pipe"]
    else:
        output_options = ["-f", "rawvideo"]
    command += (output_options) + ["pipe:1"]
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
            width, height = WIDTH, HEIGHT
            count = checksum = 0
            jpeg_offsets = []
            try:
                while True:
                    if sizing == "fit":
                        item = read_fitted_rgb888_frame(process.stdout)
                        if item is None:
                            break
                        frame, frame_width, frame_height = item
                        if count and (width, height) != (frame_width, frame_height):
                            raise ValueError("Video dimensions changed during conversion")
                        width, height = frame_width, frame_height
                    else:
                        frame = read_rgb888_frame(process.stdout)
                        if not frame:
                            break
                    if pixel_format == "rgb565":
                        frame = convert_rgb888_to_rgb565(frame)
                    count += 1
                    if pixel_format == "jpeg":
                        from PIL import Image
                        encoded = io.BytesIO()
                        Image.frombytes("RGB", (width, height), frame).save(encoded, format="JPEG",
                            quality=jpeg_quality, subsampling=2, progressive=False, optimize=False)
                        frame = encoded.getvalue()
                        if len(frame) > MAX_JPEG_BYTES:
                            raise ValueError("JPEG frame exceeds firmware's 128 KiB bound")
                        jpeg_offsets.append((jpeg_records.tell(), len(frame)))
                        jpeg_records.write(JPEG_RECORD.pack(len(frame), zlib.crc32(frame) & 0xffffffff))
                        jpeg_records.write(frame)
                        VideoMetadata(count, fps, ping_pong, pixel_format,
                                      count * JPEG_INDEX.size + jpeg_records.tell(), width=width, height=height).validate()
                    else:
                        VideoMetadata(count, fps, ping_pong, pixel_format, width=width, height=height).validate()
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
                                         count * JPEG_INDEX.size + jpeg_records.tell(), jpeg_quality, width, height)
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
                metadata = VideoMetadata(count, fps, ping_pong, pixel_format, width=width, height=height)
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


# Validates a prepared video file and reads its metadata and checksum.
#
# @param path Path of the prepared media file.
#
# @return value - Tuple containing validated VideoMetadata and the original upload request
#                 bytes.
def inspect_prepared_video(path: Path) -> tuple[VideoMetadata, bytes]:
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
# @param prepared Prepared media path or open prepared media stream.
# @param metadata Media dimensions, pixel format, placement and playback settings.
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
        with Image.open(io.BytesIO(data)) as image:
            if image.format != "JPEG" or image.size != (metadata.width,metadata.height) or image.info.get("progressive"):
                raise ValueError("JPEG frames must be baseline and match their stored dimensions")
            image.load()
        expected += JPEG_RECORD.size + length
    if expected != metadata.payload_bytes:
        raise ValueError("JPEG payload has extra or missing records")


# Reads one complete video protocol acknowledgement from the Pico.
#
# @param sock Connected TCP socket used for the video upload.
# @param expected Expected acknowledgement bytes; None accepts the next protocol reply.
#
# @return value - Complete protocol reply bytes; missing, failed or unexpected replies raise
#                 an error.
def receive_upload_reply(sock, expected: bytes | None) -> bytes:
    reply = bytearray()
    while len(reply) < 128:
        byte = sock.recv(1)
        if not byte:
            raise ConnectionError("Pico disconnected before confirming the video transaction")
        reply.extend(byte)
        if byte == b"\n":
            if expected is not None and reply != expected:
                raise RuntimeError(f"Pico replied {bytes(reply)!r}; expected {expected!r}")
            return bytes(reply)
    raise RuntimeError("Invalid oversized Pico reply")


# Opens the video TCP connection and negotiates a supported upload descriptor.
#
# @param host Pico host name or IPv4 address.
# @param port TCP upload port on the Pico.
# @param timeout Network request or connection timeout, in seconds.
# @param request Prepared protocol request or callback that performs one API operation.
# @param legacy_chunks Whether to use the legacy transport chunk size and handshake.
# @param video_name Optional uploaded video label; omitted uses the source filename.
#
# @return value - Connected socket and accepted transfer chunk size from the Pico handshake.
def connect_video_upload_socket(host: str, port: int, timeout: float, request: bytes,
                  legacy_chunks: bool, video_name: str | None = None) -> tuple[socket.socket, int]:
    # Retry only an explicit rejection of the new request magic, before sending
    # any pixels. Never retry a storage error or an uncertain transfer outcome.
    if video_name is not None:
        modes = [not legacy_chunks]
    else:
        if legacy_chunks:
            modes = [False]
        else:
            modes = [True, False]

    # Connect to the Pico before exchanging the upload descriptor and acknowledgements.
    for bulk in modes:
        sock = socket.create_connection((host, port), timeout=timeout)
        try:
            sock.settimeout(timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if video_name is not None:
                if bulk:
                    magic = b"H75N"
                else:
                    magic = b"H75n"
            else:
                if bulk:
                    magic = b"H75V"
                else:
                    magic = b"H75M"
            sock.sendall(magic + request[4:])
            reply = receive_upload_reply(sock, None)
            if video_name is not None:
                if reply in (b"ERR METADATA\n", b"ERR NAME\n"):
                    raise RuntimeError("Named video uploads require firmware r58 or newer; no video bytes sent")
                if reply != b"NAME 64\n":
                    raise RuntimeError(f"Pico replied {reply!r}; expected b'NAME 64\\n'")
                # Match the browser's bounded ASCII label (non-ASCII becomes '_').
                name_characters = []
                for character in video_name:
                    if 32 <= ord(character) <= 126:
                        name_characters.append(character)
                    else:
                        name_characters.append("_")
                name = "".join(name_characters)[:63].encode("ascii")
                if not name:
                    raise ValueError("Video name cannot be empty")
                sock.sendall(name.ljust(64, b"\0"))
                reply = receive_upload_reply(sock, None)
            if bulk and reply == b"ERR METADATA\n":
                sock.close()
                continue
            if bulk:
                expected = b"READY 6144\n"
            else:
                expected = b"READY\n"
            if reply != expected:
                if reply == b"ERR METADATA\n" and request[6] == 3:
                    raise RuntimeError("Pico rejected JPEG metadata; flash firmware r36 or newer")
                if reply == b"ERR METADATA\n" and request[6] == 2:
                    raise RuntimeError("Pico rejected RGB565 metadata; flash firmware r35 or newer")
                if reply == b"ERR METADATA\n" and request[7] & PING_PONG_FLAG:
                    raise RuntimeError("Pico rejected ping-pong metadata; flash firmware r23 or newer")
                raise RuntimeError(f"Pico replied {reply!r}; expected {expected!r}")
            if bulk:
                accepted_chunk_bytes = BULK_CHUNK_BYTES
            else:
                accepted_chunk_bytes = CHUNK_BYTES
            return sock, accepted_chunk_bytes
        except BaseException:
            sock.close()
            raise
    raise RuntimeError("Video negotiation failed")


# Uploads a prepared media file and reports transfer and verification timings.
#
# @param path Path of the prepared media file.
# @param host Pico host name or IPv4 address.
# @param port TCP upload port on the Pico.
# @param timeout Network request or connection timeout, in seconds.
# @param verification_timeout Maximum time to wait for Pico-side SD verification, in seconds.
# @param progress Callback used to report upload and verification progress.
# @param legacy_chunks Whether to use the legacy transport chunk size and handshake.
# @param ping_pong Whether playback reverses at its endpoints; None preserves the prepared
#                  setting.
# @param video_name Optional uploaded video label; omitted uses the source filename.
#
# @return value - Upload and verification result produced by transfer_video.
def upload_prepared(path: Path, host: str, port: int = PORT, *, timeout: float = 60,
                    verification_timeout: float = 1800, progress=print,
                    legacy_chunks: bool = False, ping_pong: bool | None = None,
                    video_name: str | None = None) -> VideoMetadata:
    metadata, request = inspect_prepared_video(path)
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
            metadata = replace(metadata, ping_pong=ping_pong)
            request = metadata.encode() + request[DESCRIPTOR.size:]
        if metadata.ping_pong:
            playback_mode = 'ping-pong'
        else:
            playback_mode = 'loop'
        progress(f"Video playback: {playback_mode}; format={metadata.pixel_format.upper()}")
        saved_name = (video_name or "").strip() or path.name
        progress(f"Video name: {saved_name}")
        sock, chunk_bytes = connect_video_upload_socket(host, port, timeout, request, legacy_chunks, saved_name)
        with sock:
            return transfer_video(sock, prepared, metadata, chunk_bytes, verification_timeout, progress, start)


# Sends acknowledged video chunks and waits for SD verification.
#
# @param sock Connected TCP socket used for the video upload.
# @param prepared Prepared media path or open prepared media stream.
# @param metadata Media dimensions, pixel format, placement and playback settings.
# @param chunk_bytes Number of payload bytes sent per transfer chunk.
# @param verification_timeout Maximum time to wait for Pico-side SD verification, in seconds.
# @param progress Callback used to report upload and verification progress.
# @param start Monotonic timestamp when the upload started, in seconds.
#
# @return value - Completed upload result after all frames are saved and verified.
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
            expected_reply = b"VERIFY\n"
        else:
            expected_reply = b"NEXT\n"
        receive_upload_reply(sock, expected_reply)
        percent = sent * 100 // metadata.payload_bytes
        if percent >= last_percent + 5 or percent == 100:
            progress(f"Upload {percent}% ({sent / 1048576:.1f} MiB)")
            last_percent = percent
    verification_started = time.monotonic()
    upload_seconds = verification_started - upload_started
    rate = metadata.payload_bytes / 1024 / max(upload_seconds, 0.000001)
    progress(f"All frames received in {upload_seconds:.1f}s ({rate:.1f} KiB/s); "
             "Pico is verifying the SD copy...")
    sock.settimeout(verification_timeout)
    receive_upload_reply(sock, b"SAVED\n")
    progress(f"SD verification completed in {time.monotonic() - verification_started:.1f}s.")
    progress(f"Saved and verified: {metadata.frames} frames at {float(metadata.fps):g} FPS "
             f"in {time.monotonic() - start:.1f}s. The Pico will loop the saved video.")
    return metadata


# Resolves conversion defaults and runs video preparation or upload.
def main() -> None:
    from dotenv import load_dotenv
    repository = Path(__file__).resolve().parents[1]
    load_dotenv(repository / ".env")
    default_video = Path(DEFAULT_VIDEO_PATH).expanduser()
    if not default_video.is_absolute():
        default_video = repository / default_video

    # Collect command-line overrides before applying the editable defaults.
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("video", type=Path, nargs="?", default=default_video,
                        help="Source video or previously prepared .h75v; defaults to DEFAULT_VIDEO_PATH")
    parser.add_argument("--ip", default=DEFAULT_PICO_IP or os.environ.get("PICO_IP"),
                        help="Pico address; defaults to DEFAULT_PICO_IP, then .env/environment PICO_IP")
    parser.add_argument("--port", type=int, default=PORT, help="Video port, separate from photo TCP port")
    parser.add_argument("--name", default=DEFAULT_VIDEO_NAME,
                        help="Saved video name; defaults to DEFAULT_VIDEO_NAME or the source filename when blank")
    parser.add_argument("--fps", type=Fraction, default=Fraction(DEFAULT_FPS),
                        help=f"Converted FPS, 1-60 (default {DEFAULT_FPS}); accepts 30000/1001")
    parser.add_argument("--output", type=Path, help="Keep converted .h75v here; otherwise use a temporary file")
    parser.add_argument("--prepare-only", action="store_true", help="Convert without connecting to a Pico")
    parser.add_argument("--overwrite", action="store_true", help="Replace an existing converted output")
    if MAX_VIDEO_SECONDS is not None:
        duration_description = MAX_VIDEO_SECONDS
    else:
        duration_description = 'whole video'
    parser.add_argument("--seconds", type=float, default=MAX_VIDEO_SECONDS,
                            help=f"Convert at most N seconds (default: {duration_description})")
    parser.add_argument("--ffmpeg", help="Optional path to FFmpeg executable")
    parser.add_argument("--timeout", type=float, default=60, help="Per-chunk reply timeout")
    parser.add_argument("--verification-timeout", type=float, default=1800, help="Final verification reply timeout")
    parser.add_argument("--legacy-chunks", action="store_true", help="Force 1536-byte chunks for older firmware")
    parser.add_argument("--ping-pong", action=argparse.BooleanOptionalAction, default=None,
                        help=f"Play forward then backward; --no-ping-pong restarts at frame 0 "
                             f"(source default: {PING_PONG}; prepared files keep their saved mode)")
    parser.add_argument("--pixel-format", choices=tuple(PIXEL_FORMAT_IDS), default=None,
                        help=f"Source conversion format (default: {PIXEL_FORMAT}); prepared files retain their format")
    parser.add_argument("--jpeg-quality", type=int, default=JPEG_QUALITY,
                        help=f"New JPEG source conversion quality, 1-95 (default {JPEG_QUALITY})")
    parser.add_argument("--sizing", choices=("crop", "fit"), default=SIZING_MODE,
                        help="crop fills screen; fit saves the whole image without stored black borders")
    args = parser.parse_args()
    video_name = (args.name or "").strip() or args.video.name
    if not args.prepare_only and not args.ip:
        parser.error("Set DEFAULT_PICO_IP, .env PICO_IP or supply --ip")
    if args.video.suffix.lower() == ".h75v":
        metadata, _ = inspect_prepared_video(args.video)
        if args.pixel_format is not None and args.pixel_format != metadata.pixel_format:
            parser.error("Prepared video retains its pixel format; convert the source again to change it")
        print(f"Prepared video: {metadata.frames} frames, {float(metadata.fps):g} FPS, {metadata.pixel_format.upper()}")
        if not args.prepare_only:
            upload_prepared(args.video, args.ip, args.port, timeout=args.timeout,
                            verification_timeout=args.verification_timeout, legacy_chunks=args.legacy_chunks,
                            ping_pong=args.ping_pong, video_name=video_name)
        return
    if args.output or args.prepare_only:
        output = args.output or args.video.with_suffix(".h75v")
        if args.ping_pong is None:
            ping_pong = PING_PONG
        else:
            ping_pong = args.ping_pong
        if args.pixel_format is None:
            pixel_format = PIXEL_FORMAT
        else:
            pixel_format = args.pixel_format
        metadata = prepare_video(args.video, output, args.fps, ffmpeg=args.ffmpeg,
                                         overwrite=args.overwrite, seconds=args.seconds,
                                         ping_pong=ping_pong,
                                         pixel_format=pixel_format,
                                         jpeg_quality=args.jpeg_quality, sizing=args.sizing)
        print(f"Prepared {output}: {metadata.frames} frames, {metadata.payload_bytes / 1048576:.1f} MiB")
        if not args.prepare_only:
            upload_prepared(output, args.ip, args.port, timeout=args.timeout,
                            verification_timeout=args.verification_timeout, legacy_chunks=args.legacy_chunks,
                            video_name=video_name)
    else:
        with tempfile.TemporaryDirectory(prefix="hub75-video-") as directory:
            output = Path(directory) / "video.h75v"
            if args.ping_pong is None:
                ping_pong = PING_PONG
            else:
                ping_pong = args.ping_pong
            if args.pixel_format is None:
                pixel_format = PIXEL_FORMAT
            else:
                pixel_format = args.pixel_format
            metadata = prepare_video(args.video, output, args.fps, ffmpeg=args.ffmpeg, seconds=args.seconds,
                                                 ping_pong=ping_pong,
                                                 pixel_format=pixel_format,
                                                 jpeg_quality=args.jpeg_quality, sizing=args.sizing)
            print(f"Prepared {metadata.frames} frames at {float(args.fps):g} FPS; uploading...")
            upload_prepared(output, args.ip, args.port, timeout=args.timeout,
                            verification_timeout=args.verification_timeout, legacy_chunks=args.legacy_chunks,
                            video_name=video_name)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(f"Video upload failed: {error}") from error
