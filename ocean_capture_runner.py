#!/usr/bin/env python3
"""Exercise the separate Jolt ocean scene and validate its ray-traced capture."""
from __future__ import annotations
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LOG = ROOT / "engine_run_log.txt"
CAPTURE = ROOT / "captures" / "08_ocean_buoyancy.bmp"


def compare_reflection_captures(with_reflections: bytes, without_reflections: bytes) -> tuple[float, float]:
    width, signed_height = struct.unpack_from("<ii", with_reflections, 18)
    height = abs(signed_height)
    off_width, off_signed_height = struct.unpack_from("<ii", without_reflections, 18)
    if (width, signed_height) != (off_width, off_signed_height):
        raise ValueError("reflection captures have different dimensions")
    offset = struct.unpack_from("<I", with_reflections, 10)[0]
    off_offset = struct.unpack_from("<I", without_reflections, 10)[0]
    row_stride = ((width * 3 + 3) // 4) * 4
    total_delta = 0
    total_channels = width * height * 3
    changed_pixels = 0
    for y in range(height):
        on_row = offset + y * row_stride
        off_row = off_offset + y * row_stride
        for x in range(width):
            pixel_offset = x * 3
            on_pixel = with_reflections[on_row + pixel_offset:on_row + pixel_offset + 3]
            off_pixel = without_reflections[off_row + pixel_offset:off_row + pixel_offset + 3]
            delta = sum(abs(a - b) for a, b in zip(on_pixel, off_pixel))
            total_delta += delta
            changed_pixels += delta != 0
    return total_delta / total_channels, changed_pixels / (width * height)


def compare_common_water_pixels(first_path: Path, second_path: Path) -> tuple[float, float, int]:
    first, second = first_path.read_bytes(), second_path.read_bytes()
    width, signed_height = struct.unpack_from("<ii", first, 18)
    second_width, second_signed_height = struct.unpack_from("<ii", second, 18)
    height = abs(signed_height)
    if (width, signed_height) != (second_width, second_signed_height):
        raise ValueError("water sequence frames have different dimensions or orientation")
    first_offset, second_offset = struct.unpack_from("<I", first, 10)[0], struct.unpack_from("<I", second, 10)[0]
    row_stride = ((width * 3 + 3) // 4) * 4
    total_delta = changed_pixels = sample_count = 0
    for y in range(height // 2, height):
        row = y if signed_height < 0 else height - 1 - y
        first_row, second_row = first_offset + row * row_stride, second_offset + row * row_stride
        for x in range(width):
            offset = x * 3
            p = first[first_row + offset:first_row + offset + 3]
            q = second[second_row + offset:second_row + offset + 3]
            # BMP pixels are BGR. Restrict the temporal difference to pixels
            # classified as blue water in both frames, excluding moving props.
            if p[0] <= p[2] * 1.2 or p[0] <= p[1] * 0.95:
                continue
            if q[0] <= q[2] * 1.2 or q[0] <= q[1] * 0.95:
                continue
            delta = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])
            total_delta += delta
            changed_pixels += delta > 15
            sample_count += 1
    channel_delta = total_delta / max(1, sample_count * 3)
    changed_ratio = changed_pixels / max(1, sample_count)
    return channel_delta, changed_ratio, sample_count

def main() -> int:
    args = sys.argv[1:]
    gif_30fps = "--gif-30fps" in args
    sequence_mode = "--sequence" in args or gif_30fps
    args = [arg for arg in args if arg not in {"--sequence", "--gif-30fps"}]
    no_reflections = "--no-reflections" in args
    compare_reflections = "--compare-reflections" in args
    args = [arg for arg in args if arg not in {"--no-reflections", "--compare-reflections"}]
    if len(args) > 1:
        print("usage: ocean_capture_runner.py [--no-reflections | --compare-reflections] [--sequence | --gif-30fps] [ENGINE.exe]", file=sys.stderr)
        return 2
    if no_reflections and compare_reflections:
        print("[Ocean Runner Error] Choose either --no-reflections or --compare-reflections.", file=sys.stderr)
        return 2
    if gif_30fps and (no_reflections or compare_reflections):
        print("[Ocean Runner Error] --gif-30fps cannot be combined with a reflection override.", file=sys.stderr)
        return 2
    exe = Path(args[0]) if args else Path("build/Release/CustomEngine.exe")
    if not exe.is_absolute():
        exe = ROOT / exe
    exe = exe.resolve()
    if not exe.is_file():
        print(f"[Ocean Runner Error] Engine executable not found: {exe}", file=sys.stderr)
        return 2

    if compare_reflections:
        script = Path(__file__).resolve()

        def run_variant(disabled: bool) -> int:
            child_args = [sys.executable, str(script)]
            if disabled:
                child_args.append("--no-reflections")
            child_args.append(str(exe))
            return subprocess.run(child_args, cwd=ROOT, timeout=120, check=False).returncode

        if run_variant(False) != 0:
            return 1
        with_reflections = CAPTURE.read_bytes()
        if run_variant(True) != 0:
            run_variant(False)  # Leave the normal reflection-enabled capture in place after a failed comparison.
            return 1
        without_reflections = CAPTURE.read_bytes()
        try:
            mean_delta, changed_ratio = compare_reflection_captures(with_reflections, without_reflections)
        except ValueError as error:
            run_variant(False)
            print(f"[Ocean Runner Error] {error}", file=sys.stderr)
            return 1
        restore_result = run_variant(False)
        if restore_result != 0:
            return 1
        print(f"[Ocean Runner] Reflection toggle comparison: mean channel delta={mean_delta:.3f}; changed pixels={changed_ratio:.2%}.")
        if mean_delta < 0.20 or changed_ratio < 0.005:
            print("[Ocean Runner Error] Ray-traced reflection toggle produced no meaningful image difference.", file=sys.stderr)
            return 1
        return 0
    started = time.time_ns()
    engine_args = [str(exe), "--ocean-test"]
    if gif_30fps:
        engine_args.append("--ocean-gif-30fps")
    elif sequence_mode:
        engine_args.append("--ocean-sequence")
    if no_reflections:
        engine_args.append("--ocean-no-reflections")
    print(f"[Ocean Runner] Launching {' '.join(engine_args)}")
    try:
        result = subprocess.run(engine_args, cwd=ROOT, timeout=120, check=False)
    except subprocess.TimeoutExpired:
        print("[Ocean Runner Error] Ocean run exceeded 120 seconds.", file=sys.stderr)
        return 1
    log = LOG.read_text(encoding="utf-8", errors="replace") if LOG.exists() else ""
    if not LOG.exists() or LOG.stat().st_mtime_ns < started:
        print("[Ocean Runner Error] The engine did not write a fresh log.", file=sys.stderr)
        return 1
    if result.returncode != 0:
        print(f"[Ocean Runner Error] Engine exited with code {result.returncode}.\n{log[-6000:]}", file=sys.stderr)
        return 1
    if "Jolt bodies=6" not in log or "Ocean buoyancy capture completed successfully" not in log:
        print(f"[Ocean Runner Error] Jolt initialization or capture milestone missing.\n{log[-6000:]}", file=sys.stderr)
        return 1
    if sequence_mode:
        impact_count = re.search(r"\[Ocean Test\] Frame 360:.*?impacts=(\d+)", log)
        sequence_events = {int(frame): int(count) for frame, count in re.findall(
            r"\[Ocean Sequence\] Frame (\d+): simulated=[\d.]+ seconds, impacts=(\d+)", log)}
        if not impact_count or int(impact_count.group(1)) < 7:
            print("[Ocean Runner Error] The drop simulation did not report six initial entries plus a repeat impact.", file=sys.stderr)
            return 1
        early_count = sequence_events.get(90)
        late_count = sequence_events.get(360)
        if early_count is None or late_count is None or late_count <= early_count:
            print("[Ocean Runner Error] No new water-entry event was measured after the initial drop/bounce interval.", file=sys.stderr)
            return 1
        expected_frames = tuple(range(30, 361, 2)) if gif_30fps else (
            30, 45, 60, 75, 90, 105, 120, 150, 180, 240, 300, 360)
        sequence_dir = ROOT / "captures" / ("ocean_30fps_sequence" if gif_30fps else "ocean_sequence")
        sequence_paths = [sequence_dir / f"frame_{frame}.bmp" for frame in expected_frames]
        if any(not path.is_file() or path.stat().st_mtime_ns < started for path in sequence_paths):
            print("[Ocean Runner Error] The drop/wake image sequence is incomplete or stale.", file=sys.stderr)
            return 1
        try:
            water_delta, water_changed_ratio, water_samples = compare_common_water_pixels(
                sequence_dir / "frame_90.bmp", sequence_dir / "frame_180.bmp")
        except (OSError, ValueError) as error:
            print(f"[Ocean Runner Error] Could not compare water sequence frames: {error}", file=sys.stderr)
            return 1
        if water_samples < 2000 or water_delta < 2.0 or water_changed_ratio < 0.05:
            print("[Ocean Runner Error] Blue-water pixels did not show a strong temporal response between bounce frames.", file=sys.stderr)
            return 1
        capture_rate = "30 fps" if gif_30fps else "sampled"
        print(f"[Ocean Runner] Validated {len(sequence_paths)} fresh {capture_rate} water-drop frames; impacts increased from {early_count} by frame 90 to {late_count} by frame 360.")
        print(f"[Ocean Runner] Shared blue-water region changed by {water_delta:.2f} mean channel levels ({water_changed_ratio:.1%} of {water_samples:,} common water pixels) between frames 90 and 180.")
    if "[DXR] Selected hardware adapter supports Tier" not in log or "[DXR] DispatchRays recorded" not in log:
        print("[Ocean Runner Error] Ocean capture did not confirm active hardware DXR dispatch.", file=sys.stderr)
        return 1
    simulated = re.search(r"\[Ocean Test\] Frame 360:.*?simulated=([\d.]+) seconds", log)
    if not simulated or float(simulated.group(1)) < 5.9:
        print("[Ocean Runner Error] Jolt did not advance through the six-second simulation window.", file=sys.stderr)
        return 1
    displacement = re.search(r"max body Y displacement=([\d.]+) m", log)
    if not displacement or float(displacement.group(1)) < 0.02:
        print("[Ocean Runner Error] Jolt bodies were created and stepped, but no buoyancy motion was measured.", file=sys.stderr)
        return 1
    body_y = re.search(r"final body Y range=([-\d.]+)\.\.([-\d.]+) m", log)
    if not body_y:
        print("[Ocean Runner Error] Final floating-body heights were not reported.", file=sys.stderr)
        return 1
    body_y_min, body_y_max = (float(value) for value in body_y.groups())
    if body_y_min < -1.5 or body_y_max > 2.0:
        print(f"[Ocean Runner Error] Bodies did not settle near the water surface ({body_y_min}..{body_y_max} m).", file=sys.stderr)
        return 1
    try:
        data = CAPTURE.read_bytes()
    except OSError:
        print(f"[Ocean Runner Error] Missing capture: {CAPTURE}", file=sys.stderr)
        return 1
    if CAPTURE.stat().st_mtime_ns < started:
        print("[Ocean Runner Error] Ocean BMP was not refreshed by this run.", file=sys.stderr)
        return 1
    if len(data) < 54 or data[:2] != b"BM":
        print("[Ocean Runner Error] Ocean capture is not a valid BMP.", file=sys.stderr)
        return 1
    width, signed_height = struct.unpack_from("<ii", data, 18)
    height = abs(signed_height)
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    row_stride = ((width * 3 + 3) // 4) * 4
    if width <= 0 or height <= 0 or len(data) != pixel_offset + row_stride * height:
        print("[Ocean Runner Error] Ocean capture has invalid dimensions.", file=sys.stderr)
        return 1
    lower_samples = []
    for y in range(height // 2, height, max(1, height // 32)):
        row_start = pixel_offset + (y if signed_height < 0 else height - 1 - y) * row_stride
        for x in range(0, width, max(1, width // 64)):
            blue, green, red = data[row_start + x * 3:row_start + x * 3 + 3]
            lower_samples.append((red, green, blue))
    blue_water_ratio = sum(blue > red * 1.2 and blue > green * 0.95 for red, green, blue in lower_samples) / max(1, len(lower_samples))
    if blue_water_ratio < 0.25:
        print("[Ocean Runner Error] Lower-half capture does not contain the expected blue ocean surface.", file=sys.stderr)
        return 1
    fps = re.search(r"\[Ocean Test\] Frame 360:.*?\(([\d.]+) FPS\)", log)
    print(f"[Ocean Runner] Validated {width}x{abs(height)} ocean render with six Jolt bodies.")
    print(f"[Ocean Runner] Jolt simulation advanced {simulated.group(1)} seconds.")
    print(f"[Ocean Runner] Maximum measured body vertical displacement: {displacement.group(1)} m.")
    print(f"[Ocean Runner] Body center heights at capture: {body_y.group(1)}..{body_y.group(2)} m.")
    print(f"[Ocean Runner] Blue-water pixels in lower half: {blue_water_ratio:.1%}.")
    if fps:
        print(f"[Ocean Runner] Ocean test average: {fps.group(1)} FPS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
