#!/usr/bin/env python3
"""Build the curated README water animation from dense, fixed-step BMP captures."""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "captures" / "ocean_30fps_sequence"
DEFAULT_OUTPUT = ROOT / "docs" / "media" / "water-bounce-30fps.gif"
CAPTURE_FRAMES = tuple(range(30, 361, 2))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--fps", type=int, choices=(30,), default=30,
                        help="the source is captured at every other 60 Hz physics step")
    parser.add_argument("--max-width", type=int, default=516,
                        help="maximum encoded width; aspect ratio is preserved (default: 516 px)")
    parser.add_argument("--colors", type=int, default=192,
                        help="shared GIF palette size from 32 to 256 (default: 192)")
    args = parser.parse_args()
    if args.max_width < 64 or not 32 <= args.colors <= 256:
        parser.error("--max-width must be at least 64 and --colors must be from 32 to 256")

    paths = [args.input_dir / f"frame_{frame}.bmp" for frame in CAPTURE_FRAMES]
    missing = [path for path in paths if not path.is_file()]
    if missing:
        parser.error(f"missing dense capture {missing[0]}; run ocean_capture_runner.py --gif-30fps first")

    expected_size: tuple[int, int] | None = None
    for path in paths:
        with Image.open(path) as source:
            frame = source.convert("RGB")
        if expected_size is None:
            expected_size = frame.size
        elif frame.size != expected_size:
            parser.error(f"capture dimensions differ at {path.name}: {frame.size} != {expected_size}")
        del frame

    assert expected_size is not None
    scale = min(1.0, args.max_width / expected_size[0])
    output_size = (max(1, round(expected_size[0] * scale)), max(1, round(expected_size[1] * scale)))
    # Derive one shared palette from thumbnails spanning the full sequence to
    # reduce palette flicker between consecutive frames.
    thumbnail_size = (min(172, output_size[0]), min(112, output_size[1]))
    columns = 8
    rows = (len(paths) + columns - 1) // columns
    palette_source = Image.new("RGB", (columns * thumbnail_size[0], rows * thumbnail_size[1]))
    for index, path in enumerate(paths):
        with Image.open(path) as source:
            thumbnail = source.convert("RGB").resize(output_size, Image.Resampling.BILINEAR)
            thumbnail.thumbnail(thumbnail_size, Image.Resampling.BILINEAR)
        palette_source.paste(thumbnail, ((index % columns) * thumbnail_size[0],
                                         (index // columns) * thumbnail_size[1]))
        del thumbnail
    shared_palette = palette_source.quantize(colors=args.colors, method=Image.Quantize.MEDIANCUT)
    del palette_source
    indexed_frames: list[Image.Image] = []
    for path in paths:
        with Image.open(path) as source:
            resized = source.convert("RGB").resize(output_size, Image.Resampling.BILINEAR)
            indexed_frames.append(resized.quantize(
                palette=shared_palette, dither=Image.Dither.NONE))

    # GIF delays have 10 ms granularity. 30/30/40 ms repeats average exactly
    # 33.33 ms, preserving a nominal 30 fps cadence over the whole animation.
    durations = [30 if index % 3 != 2 else 40 for index in range(len(indexed_frames))]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    indexed_frames[0].save(
        args.output,
        format="GIF",
        save_all=True,
        append_images=indexed_frames[1:],
        duration=durations,
        loop=0,
        optimize=True,
        disposal=2,
    )
    total_duration_ms = sum(durations)
    average_fps = len(indexed_frames) * 1000.0 / total_duration_ms
    print(f"Wrote {args.output} ({len(indexed_frames)} frames, {output_size[0]}x{output_size[1]}, "
          f"{average_fps:.2f} fps average, {args.output.stat().st_size:,} bytes).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
