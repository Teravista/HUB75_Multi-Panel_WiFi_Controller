"""Convert an image directly into a C/C++ RGB565 byte-array header.

Usage example:
    python utilityScripts/imageToRGB565ArrayConverter.py \
        --input "sample_image.jpg" \
        --output-folder "imagesVideos" \
        --width 128 --height 256

"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image


DEFAULT_WIDTH = 128
DEFAULT_HEIGHT = 256
DEFAULT_INPUT = "imagesVideos\\private\\apple.jpg"
DEFAULT_OUTPUT_FOLDER = "imagesVideos"


# ------------------------------ Input and scaling ------------------------------

# Parses image conversion paths, sizing, and C header options.
#
# @return value - Parsed command-line option namespace.
def parse_arguments() -> argparse.Namespace:

    # Collect command-line overrides before applying the editable defaults.
    parser = argparse.ArgumentParser(
        description="Convert an image to a C/C++ RGB565 byte-array header"
    )
    parser.add_argument("--input", default=DEFAULT_INPUT, help="Input image file")
    parser.add_argument(
        "--output-folder",
        default=DEFAULT_OUTPUT_FOLDER,
        help="Folder for generated header",
    )
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    return parser.parse_args()


# Converts an output name into a valid C identifier.
#
# @param name Text to convert into a valid C identifier.
#
# @return value - Valid C identifier derived from the supplied name.
def sanitize_c_identifier(name: str) -> str:
    identifier = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not identifier or identifier[0].isdigit():
        identifier = f"image_{identifier}"
    return identifier


# Resolves the output header path and generated C symbol names.
#
# @param input_path Path to the source image or video.
# @param width Image width in pixels.
# @param height Image height in pixels.
# @param output_folder Destination folder for generated headers.
#
# @return value - Output header path and generated C identifier derived from the source file.
def derive_output_names(
    input_path: Path,
    width: int,
    height: int,
    output_folder: str,
) -> tuple[Path, str]:
    input_stem = input_path.stem
    image_name = f"{input_stem}_{width}x{height}ByteArray"
    header_path = Path(output_folder) / f"{image_name}.h"
    array_name = sanitize_c_identifier(image_name)
    return header_path, array_name


# Center-crops an image to the requested display aspect ratio.
#
# @param image Pillow image to convert or resize.
# @param target_width Requested output width in pixels.
# @param target_height Requested output height in pixels.
#
# @return value - Image cropped centrally to match the requested output aspect ratio.
def crop_to_aspect(image: Image.Image, target_width: int, target_height: int) -> Image.Image:
    """Center-crop an image to the requested aspect ratio."""
    source_width, source_height = image.size
    source_aspect = source_width / source_height
    target_aspect = target_width / target_height

    if source_aspect > target_aspect:
        cropped_width = int(source_height * target_aspect)
        left = (source_width - cropped_width) // 2
        crop_box = (left, 0, left + cropped_width, source_height)
    else:
        cropped_height = int(source_width / target_aspect)
        top = (source_height - cropped_height) // 2
        crop_box = (0, top, source_width, top + cropped_height)

    return image.crop(crop_box)


# Loads a photo and resizes its crop to the logical panel dimensions.
#
# @param input_path Path to the source image or video.
# @param target_width Requested output width in pixels.
# @param target_height Requested output height in pixels.
#
# @return value - RGB Pillow image resized to the requested logical dimensions.
def load_and_resize_image(
    input_path: Path,
    target_width: int,
    target_height: int,
) -> Image.Image:
    """Load, RGB-normalize, center-crop, and resize the source image."""
    if not input_path.exists():
        raise SystemExit(f"Image not found: {input_path}")

    image = Image.open(input_path).convert("RGB")
    image = crop_to_aspect(image, target_width, target_height)
    return image.resize((target_width, target_height), Image.Resampling.LANCZOS)


# ------------------------------ RGB565 conversion ------------------------------

# Serializes image pixels in the RGB byte order expected by the firmware.
#
# @param image Pillow image to convert or resize.
#
# @return value - Row-major little-endian RGB565 pixels.
def PackImagePixels(image: Image.Image) -> bytes:
    """Return two little-endian bytes per RGB565 pixel."""
    pixels = bytearray()
    for red, green, blue in image.getdata():
        value = (red >> 3) << 11 | (green >> 2) << 5 | blue >> 3
        pixels.extend((value & 255, value >> 8))
    return bytes(pixels)


# ------------------------------ C header output ------------------------------

# Writes the converted image bytes and dimensions as a C header.
#
# @param output_path Path of the generated C header.
# @param variable_name C identifier used for the generated image array.
# @param width Image width in pixels.
# @param height Image height in pixels.
# @param pixelData Row-major RGB565 pixel bytes to write into the generated header.
def WriteImageHeader(
    output_path: Path,
    variable_name: str,
    width: int,
    height: int,
    pixelData: bytes,
) -> None:
    """Write RGB565 data as a compilable C/C++ byte-array header."""
    expected_size = width * height * 2
    if len(pixelData) != expected_size:
        raise SystemExit(
            f"Unexpected data size: {len(pixelData)} bytes, expected {expected_size}"
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="ascii") as output:
        output.write("#pragma once\n")
        output.write(f"// Auto-generated RGB565 image: {width}x{height}\n")
        output.write(f"#define GENERATED_IMAGE_DATA {variable_name}\n\n")
        output.write(
            f"static const unsigned char {variable_name}[{expected_size}] = {{\n"
        )

        for index, byte in enumerate(pixelData):
            if index % 12 == 0:
                output.write("\n    ")
            output.write(f"0x{byte:02x},")

        output.write("\n};\n")


# ------------------------------------ Main ------------------------------------

# Runs image conversion and reports the generated header.
#
# @return 0 - Conversion completed successfully; failures raise an exception.
def main() -> int:
    arguments = parse_arguments()
    input_path = Path(arguments.input)
    output_path, variable_name = derive_output_names(
        input_path,
        arguments.width,
        arguments.height,
        arguments.output_folder,
    )
    resized_image = load_and_resize_image(
        input_path, arguments.width, arguments.height
    )
    pixelData = PackImagePixels(resized_image)
    WriteImageHeader(
        output_path,
        variable_name,
        arguments.width,
        arguments.height,
        pixelData,
    )
    print(f"Wrote {output_path} ({arguments.width}x{arguments.height} RGB565)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())