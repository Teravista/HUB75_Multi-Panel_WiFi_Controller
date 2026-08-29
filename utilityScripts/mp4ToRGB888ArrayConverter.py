"""Convert an MP4 into a multi-frame RGB888 C header.

Usage example:
    python utilityScripts/mp4ToRGB888ArrayConverter.py \
        --input "sample_video.mp4" \
        --output-folder "imagesVideos" \
        --width 128 --height 256 \
        --target-fps 12 --max-frames 12 \
        --delay-ms 80

Notes:
- Keep max frame count low so the generated header still fits MCU flash.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


# -------------------------------- Configuration --------------------------------

DEFAULT_INPUT = "imagesVideos\\private\\forst_walkTrim.mp4"
DEFAULT_OUTPUT_FOLDER = "imagesVideos"
DEFAULT_FRAME_WIDTH = 128
DEFAULT_FRAME_HEIGHT = 256
#how many frames per second to extract from the source video
DEFAULT_TARGET_FPS = 2
DEFAULT_FRAME_STEP = 0
#how many diferent frames will be created, 38 frames is roughly maximum flash capacity for RGB888 for 4 MB of Raspbery pi pico 2
DEFAULT_MAX_FRAMES = 38
#the delay between frames in milliseconds for the animation playback on the device
DEFAULT_FRAME_DELAY_MS = 200
DEFAULT_TRIM_BOTTOM = 0

RGB_CHANNEL_COUNT = 3
DEFAULT_SOURCE_FPS = 30.0
HEADER_BYTES_PER_LINE = 12

import imageio.v2 as iio
import numpy as np
from PIL import Image


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert MP4 to C header animation array")
    parser.add_argument("--input", default=DEFAULT_INPUT, help="Input MP4 file")
    parser.add_argument(
        "--output-folder",
        default=DEFAULT_OUTPUT_FOLDER,
        help="Folder for generated header",
    )
    parser.add_argument("--width", type=int, default=DEFAULT_FRAME_WIDTH, help="Frame width")
    parser.add_argument("--height", type=int, default=DEFAULT_FRAME_HEIGHT, help="Frame height")
    parser.add_argument("--target-fps", type=float, default=DEFAULT_TARGET_FPS, help="Target extracted FPS")
    parser.add_argument("--frame-step", type=int, default=DEFAULT_FRAME_STEP, help="Keep every Nth source frame")
    parser.add_argument("--max-frames", type=int, default=DEFAULT_MAX_FRAMES, help="Maximum frames to export")
    parser.add_argument("--delay-ms", type=int, default=DEFAULT_FRAME_DELAY_MS, help="Playback delay per frame")
    parser.add_argument(
        "--trim-bottom",
        type=int,
        default=DEFAULT_TRIM_BOTTOM,
        help="Trim N pixels from the bottom of each source frame before crop/resize.",
    )
    return parser.parse_args()


def derive_output_names(
    input_path: Path,
    width: int,
    height: int,
    output_folder: str,
) -> tuple[Path, str]:
    input_stem = input_path.stem
    if f"{width}x{height}" in input_stem:
        anim_name = f"{input_stem}_anim"
    else:
        anim_name = f"{input_stem}_{width}x{height}_anim"

    header_path = Path(output_folder) / f"{anim_name}.h"
    array_name = sanitize_c_identifier(anim_name)
    return header_path, array_name


def crop_to_aspect(img: Image.Image, target_w: int, target_h: int) -> Image.Image:
    """Center-crop image to target aspect ratio."""
    w, h = img.size
    src_aspect = w / h
    target_aspect = target_w / target_h
    if src_aspect > target_aspect:
        new_w = int(h * target_aspect)
        left = (w - new_w) // 2
        right = left + new_w
        top = 0
        bottom = h
    else:
        new_h = int(w / target_aspect)
        top = (h - new_h) // 2
        bottom = top + new_h
        left = 0
        right = w
    return img.crop((left, top, right, bottom))


def sanitize_c_identifier(name: str) -> str:
    identifier = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not identifier or identifier[0].isdigit():
        identifier = f"animation_{identifier}"
    return identifier


def get_resample_lanczos() -> int:
    if hasattr(Image, "Resampling"):
        return Image.Resampling.LANCZOS
    return Image.LANCZOS


def preprocess_frame(
    frame_rgb: np.ndarray,
    width: int,
    height: int,
    trim_bottom: int,
    resample_lanczos: int,
) -> np.ndarray:
    if frame_rgb.ndim == 2:
        frame_rgb = np.stack([frame_rgb, frame_rgb, frame_rgb], axis=-1)
    if frame_rgb.shape[2] > RGB_CHANNEL_COUNT:
        frame_rgb = frame_rgb[:, :, :RGB_CHANNEL_COUNT]

    if trim_bottom > 0:
        if trim_bottom >= frame_rgb.shape[0]:
            raise SystemExit(
                f"--trim-bottom ({trim_bottom}) must be smaller than frame height ({frame_rgb.shape[0]})"
            )
        frame_rgb = frame_rgb[: frame_rgb.shape[0] - trim_bottom, :, :]

    img = Image.fromarray(frame_rgb, mode="RGB")
    img = crop_to_aspect(img, width, height)
    img = img.resize((width, height), resample_lanczos)
    return np.asarray(img, dtype=np.uint8)


def processed_frame_to_bytes(processed_frame: np.ndarray) -> bytes:
    """Return row-major RGB888 bytes: red, green, blue for each pixel."""
    return processed_frame.tobytes()


def extract_processed_frames(
    input_path: Path,
    width: int,
    height: int,
    target_fps: float,
    frame_step: int,
    max_frames: int,
    trim_bottom: int,
) -> tuple[list[np.ndarray], float, int]:
    try:
        reader = iio.get_reader(str(input_path), format="ffmpeg")
    except Exception as exc:
        raise SystemExit(f"Could not open video: {input_path}\nOriginal error: {exc}")

    meta = reader.get_meta_data()
    src_fps = float(meta.get("fps", DEFAULT_SOURCE_FPS) or DEFAULT_SOURCE_FPS)

    if frame_step > 0:
        effective_step = frame_step
    else:
        if target_fps <= 0:
            effective_step = 1
        else:
            effective_step = max(1, int(round(src_fps / target_fps)))

    resample_lanczos = get_resample_lanczos()
    frames: list[np.ndarray] = []
    source_frame_idx = 0
    for frame in reader:
        if source_frame_idx % effective_step == 0:
            frames.append(
                preprocess_frame(
                    frame_rgb=frame,
                    width=width,
                    height=height,
                    trim_bottom=trim_bottom,
                    resample_lanczos=resample_lanczos,
                )
            )
            if len(frames) >= max_frames:
                break

        source_frame_idx += 1

    reader.close()

    if not frames:
        raise SystemExit("No frames extracted. Check input video or extraction parameters.")

    return frames, src_fps, effective_step


def write_c_header(
    output_path: Path,
    input_path: Path,
    var_name: str,
    width: int,
    height: int,
    delay_ms: int,
    frames: list[bytes],
) -> None:
    frame_bytes = width * height * RGB_CHANNEL_COUNT

    with output_path.open("w", encoding="utf-8", newline="\n") as out:
        out.write("#pragma once\n")
        out.write("// Auto-generated animation header.\n")
        out.write(f"// Source video: {input_path.name}\n")
        out.write(f"// Output format: {width}x{height}, RGB888 bytes in RGB order\n")
        out.write("// Keep frame count small enough for your available flash.\n\n")

        out.write(f"#define GENERATED_ANIMATION_WIDTH {width}\n")
        out.write(f"#define GENERATED_ANIMATION_HEIGHT {height}\n")
        out.write(
            f"#define GENERATED_ANIMATION_FRAME_BYTES (GENERATED_ANIMATION_WIDTH * GENERATED_ANIMATION_HEIGHT * "
            f"{RGB_CHANNEL_COUNT})\n"
        )
        out.write(f"#define GENERATED_ANIMATION_FRAME_COUNT {len(frames)}\n")
        out.write(f"#define GENERATED_ANIMATION_FRAME_DELAY_MS {delay_ms}\n")
        out.write(f"#define GENERATED_ANIMATION_DATA {var_name}\n\n")

        out.write(
            f"static const unsigned char {var_name}[GENERATED_ANIMATION_FRAME_COUNT][GENERATED_ANIMATION_FRAME_BYTES] = {{\n"
        )

        for frame in frames:
            out.write("    {\n")
            for i, b in enumerate(frame):
                if i % HEADER_BYTES_PER_LINE == 0:
                    out.write("        ")
                out.write(f"0x{b:02x},")
                if i % HEADER_BYTES_PER_LINE == HEADER_BYTES_PER_LINE - 1:
                    out.write("\n")
            if len(frame) % HEADER_BYTES_PER_LINE != 0:
                out.write("\n")
            out.write("    },\n")

        out.write("};\n")

    actual_size = output_path.stat().st_size
    print(
        f"Wrote {output_path} with {len(frames)} frames "
        f"({frame_bytes * len(frames)} raw bytes, {actual_size} file bytes)"
    )


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)

    if not input_path.exists():
        raise SystemExit(f"Input video not found: {input_path}")

    if args.width <= 0 or args.height <= 0:
        raise SystemExit("--width and --height must be > 0")
    if args.max_frames <= 0:
        raise SystemExit("--max-frames must be > 0")
    if args.delay_ms <= 0:
        raise SystemExit("--delay-ms must be > 0")
    if args.trim_bottom < 0:
        raise SystemExit("--trim-bottom must be >= 0")

    output_path, var_name = derive_output_names(
        input_path=input_path,
        width=args.width,
        height=args.height,
        output_folder=args.output_folder,
    )

    processed_frames, src_fps, step = extract_processed_frames(
        input_path=input_path,
        width=args.width,
        height=args.height,
        target_fps=args.target_fps,
        frame_step=args.frame_step,
        max_frames=args.max_frames,
        trim_bottom=args.trim_bottom,
    )

    frames = [processed_frame_to_bytes(frame) for frame in processed_frames]
    write_c_header(
        output_path=output_path,
        input_path=input_path,
        var_name=var_name,
        width=args.width,
        height=args.height,
        delay_ms=args.delay_ms,
        frames=frames,
    )

    effective_fps = src_fps / step
    print(
        f"Source FPS: {src_fps:.2f}, extraction step: {step}, "
        f"effective exported FPS: {effective_fps:.2f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
