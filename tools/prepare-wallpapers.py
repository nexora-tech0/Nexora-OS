#!/usr/bin/env python3
"""Convert supported wallpaper images to small, uncompressed BMP boot assets."""

import sys
from pathlib import Path

from PIL import Image, ImageOps


SUPPORTED = {".bmp", ".jpeg", ".jpg", ".png", ".webp"}
MAX_WALLPAPERS = 256
MAX_SIZE = (1920, 1280)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: prepare-wallpapers.py INPUT_DIR OUTPUT_DIR", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    for stale in output.glob("wallpaper-*.bmp"):
        stale.unlink()

    images = sorted(
        (path for path in source.iterdir() if path.is_file() and path.suffix.lower() in SUPPORTED),
        key=lambda path: path.name.casefold(),
    )
    if len(images) > MAX_WALLPAPERS:
        print(f"too many wallpapers: {len(images)} (maximum {MAX_WALLPAPERS})", file=sys.stderr)
        return 1

    for index, path in enumerate(images):
        try:
            with Image.open(path) as image:
                image = ImageOps.exif_transpose(image).convert("RGB")
                image.thumbnail(MAX_SIZE, Image.Resampling.LANCZOS)
                image.save(output / f"wallpaper-{index:03}.bmp", format="BMP")
        except (OSError, ValueError) as error:
            print(f"cannot prepare wallpaper {path}: {error}", file=sys.stderr)
            return 1

    (output / "count.txt").write_text(f"{len(images)}\n", encoding="ascii")
    print(f"wallpapers: {len(images)} image(s) prepared")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
