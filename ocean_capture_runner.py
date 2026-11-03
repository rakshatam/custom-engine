#!/usr/bin/env python3
"""Exercise the separate Jolt ocean scene and validate its ray-traced capture."""
from __future__ import annotations
import re
import math
import shutil
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


def compare_water_reflection_captures(
    with_reflections: bytes, without_reflections: bytes, mask_reference: bytes | None = None,
) -> tuple[float, float, int]:
    """Measure DXR changes on a fixed low-reflectance blue-water mask."""
    width, signed_height = struct.unpack_from("<ii", without_reflections, 18)
    on_width, on_signed_height = struct.unpack_from("<ii", with_reflections, 18)
    height = abs(signed_height)
    if (width, signed_height) != (on_width, on_signed_height):
        raise ValueError("reflection captures have different dimensions or orientation")
    if mask_reference is None:
        mask_reference = without_reflections
    mask_width, mask_signed_height = struct.unpack_from("<ii", mask_reference, 18)
    if (width, signed_height) != (mask_width, mask_signed_height):
        raise ValueError("water mask and reflection captures have different dimensions or orientation")
    offset = struct.unpack_from("<I", with_reflections, 10)[0]
    off_offset = struct.unpack_from("<I", without_reflections, 10)[0]
    mask_offset = struct.unpack_from("<I", mask_reference, 10)[0]
    row_stride = ((width * 3 + 3) // 4) * 4
    total_delta = changed_pixels = water_pixels = 0
    # Use a separate 1x/no-DXR reference when comparing higher strengths. This
    # prevents the reflectance control itself changing the blue-water mask.
    for y in range(height // 2, height):
        row = y if signed_height < 0 else height - 1 - y
        on_row = offset + row * row_stride
        off_row = off_offset + row * row_stride
        mask_row = mask_offset + row * row_stride
        for x in range(width):
            pixel = x * 3
            blue, green, red = mask_reference[mask_row + pixel:mask_row + pixel + 3]
            if blue <= red * 1.2 or blue <= green * 0.95:
                continue
            on_pixel = with_reflections[on_row + pixel:on_row + pixel + 3]
            off_pixel = without_reflections[off_row + pixel:off_row + pixel + 3]
            delta = sum(abs(a - b) for a, b in zip(on_pixel, off_pixel))
            total_delta += delta
            changed_pixels += delta > 3  # Ignore one-code output quantization changes.
            water_pixels += 1
    mean_delta = total_delta / max(1, water_pixels * 3)
    changed_ratio = changed_pixels / max(1, water_pixels)
    return mean_delta, changed_ratio, water_pixels


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


def compare_body_neighborhood_water(first_path: Path, second_path: Path) -> tuple[float, float, int]:
    """Compare shared blue-water pixels in fixed test-scene neighborhoods around the six props."""
    first, second = first_path.read_bytes(), second_path.read_bytes()
    width, signed_height = struct.unpack_from("<ii", first, 18)
    second_width, second_signed_height = struct.unpack_from("<ii", second, 18)
    height = abs(signed_height)
    if (width, signed_height) != (second_width, second_signed_height):
        raise ValueError("body-displacement captures have different dimensions or orientation")
    if width <= 0 or height <= 0:
        raise ValueError(f"body-neighborhood test requires positive capture dimensions, got {width}x{height}")
    first_offset, second_offset = struct.unpack_from("<I", first, 10)[0], struct.unpack_from("<I", second, 10)[0]
    row_stride = ((width * 3 + 3) // 4) * 4
    # Centers were calibrated in a 688x448 view. Scale by height to preserve
    # the same vertical-FOV projection when testing at native 1920x1080; this
    # maps horizontal offsets correctly if the aspect ratio changes.
    reference_width, reference_height = 688.0, 448.0
    pixel_scale = height / reference_height
    body_centers = tuple(
        (width * 0.5 + (x - reference_width * 0.5) * pixel_scale, y * pixel_scale)
        for x, y in ((231, 318), (314, 296), (377, 315), (429, 291), (464, 351), (339, 391))
    )
    radius_x = max(1, round(34 * pixel_scale))
    radius_y = max(1, round(34 * pixel_scale))
    total_delta = changed_pixels = sample_count = 0
    for center_x_float, center_y_float in body_centers:
        center_x, center_y = round(center_x_float), round(center_y_float)
        for y in range(max(height // 2, center_y - radius_y), min(height, center_y + radius_y + 1)):
            row = y if signed_height < 0 else height - 1 - y
            first_row = first_offset + row * row_stride
            second_row = second_offset + row * row_stride
            for x in range(max(0, center_x - radius_x), min(width, center_x + radius_x + 1)):
                dx = (x - center_x) / radius_x
                dy = (y - center_y) / radius_y
                if dx * dx + dy * dy > 1.0:
                    continue
                offset = x * 3
                p = first[first_row + offset:first_row + offset + 3]
                q = second[second_row + offset:second_row + offset + 3]
                # BMP is BGR; retain only pixels that classify as blue water in both captures.
                if p[0] <= p[2] * 1.2 or p[0] <= p[1] * 0.95:
                    continue
                if q[0] <= q[2] * 1.2 or q[0] <= q[1] * 0.95:
                    continue
                delta = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])
                total_delta += delta
                changed_pixels += delta > 15
                sample_count += 1
    return (total_delta / max(1, sample_count * 3),
            changed_pixels / max(1, sample_count), sample_count)

def main() -> int:
    args = sys.argv[1:]
    native_resolution = "--native-resolution" in args
    args = [arg for arg in args if arg != "--native-resolution"]
    reflection_strength = 2.5
    for arg in args[:]:
        if arg.startswith("--reflection-strength="):
            try:
                reflection_strength = float(arg.partition("=")[2])
            except ValueError:
                print("[Ocean Runner Error] Reflection strength must be numeric.", file=sys.stderr)
                return 2
            args.remove(arg)
            break
    if not math.isfinite(reflection_strength) or not 1.0 <= reflection_strength <= 4.0:
        print("[Ocean Runner Error] Reflection strength must be finite and between 1.0 and 4.0.", file=sys.stderr)
        return 2
    gif_30fps = "--gif-30fps" in args
    sequence_mode = "--sequence" in args or gif_30fps
    args = [arg for arg in args if arg not in {"--sequence", "--gif-30fps"}]
    no_reflections = "--no-reflections" in args
    compare_reflections = "--compare-reflections" in args
    no_body_displacement = "--no-body-displacement" in args
    compare_body_displacement = "--compare-body-displacement" in args
    args = [arg for arg in args if arg not in {
        "--no-reflections", "--compare-reflections", "--no-body-displacement", "--compare-body-displacement"}]
    if len(args) > 1:
        print("usage: ocean_capture_runner.py [--native-resolution] [--reflection-strength=1..4] [--no-reflections | --compare-reflections] [--no-body-displacement | --compare-body-displacement] [--sequence | --gif-30fps] [ENGINE.exe]", file=sys.stderr)
        return 2
    if no_reflections and compare_reflections:
        print("[Ocean Runner Error] Choose either --no-reflections or --compare-reflections.", file=sys.stderr)
        return 2
    if no_body_displacement and compare_body_displacement:
        print("[Ocean Runner Error] Choose either --no-body-displacement or --compare-body-displacement.", file=sys.stderr)
        return 2
    if (no_reflections or compare_reflections) and (no_body_displacement or compare_body_displacement):
        print("[Ocean Runner Error] Compare one ocean rendering feature at a time.", file=sys.stderr)
        return 2
    if gif_30fps and (no_reflections or compare_reflections or no_body_displacement or compare_body_displacement):
        print("[Ocean Runner Error] --gif-30fps cannot be combined with a rendering override.", file=sys.stderr)
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
        capture_dir = ROOT / "captures"
        reflection_on_path = capture_dir / "ocean_reflections_on.bmp"
        reflection_off_path = capture_dir / "ocean_reflections_off.bmp"
        reflection_restored_path = capture_dir / "ocean_reflections_restored.bmp"

        def run_variant(disabled: bool, strength: float | None = None) -> int:
            selected_strength = reflection_strength if strength is None else strength
            child_args = [sys.executable, str(script), f"--reflection-strength={selected_strength:g}"]
            if native_resolution:
                child_args.append("--native-resolution")
            if disabled:
                child_args.append("--no-reflections")
            child_args.append(str(exe))
            return subprocess.run(child_args, cwd=ROOT, timeout=120, check=False).returncode

        if run_variant(True, 1.0) != 0:
            return 1
        water_mask_reference = CAPTURE.read_bytes()
        mask_path = capture_dir / "ocean_reflection_water_mask.bmp"
        capture_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CAPTURE, mask_path)

        if run_variant(False) != 0:
            return 1
        with_reflections = CAPTURE.read_bytes()
        capture_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CAPTURE, reflection_on_path)
        if run_variant(True) != 0:
            run_variant(False)  # Leave the normal reflection-enabled capture in place after a failed comparison.
            return 1
        without_reflections = CAPTURE.read_bytes()
        shutil.copy2(CAPTURE, reflection_off_path)
        try:
            mean_delta, changed_ratio = compare_reflection_captures(with_reflections, without_reflections)
            water_mean_delta, water_changed_ratio, water_pixels = compare_water_reflection_captures(
                with_reflections, without_reflections, water_mask_reference)
        except ValueError as error:
            run_variant(False)
            print(f"[Ocean Runner Error] {error}", file=sys.stderr)
            return 1
        restore_result = run_variant(False)
        if restore_result != 0:
            return 1
        shutil.copy2(CAPTURE, reflection_restored_path)
        print(f"[Ocean Runner] Reflection toggle comparison: full-frame mean channel delta={mean_delta:.3f}; changed pixels={changed_ratio:.2%}.")
        print(f"[Ocean Runner] Blue-water-only DXR contribution: mean channel delta={water_mean_delta:.3f}; changed pixels={water_changed_ratio:.2%} of {water_pixels:,} water pixels.")
        if mean_delta < 0.20 or changed_ratio < 0.005:
            print("[Ocean Runner Error] Ray-traced reflection toggle produced no meaningful image difference.", file=sys.stderr)
            return 1
        if water_pixels < 1000 or water_mean_delta < 0.20 or water_changed_ratio < 0.005:
            print("[Ocean Runner Error] DXR reflection toggle did not produce a meaningful change on the water surface.", file=sys.stderr)
            return 1
        if not (reflection_on_path.is_file() and reflection_off_path.is_file() and reflection_restored_path.is_file()
                and mask_path.is_file()):
            print("[Ocean Runner Error] Water reflection on/off/restored evidence captures were not preserved.", file=sys.stderr)
            return 1
        return 0
    if compare_body_displacement:
        script = Path(__file__).resolve()

        def run_variant(disabled: bool) -> int:
            child_args = [sys.executable, str(script), f"--reflection-strength={reflection_strength:g}"]
            if native_resolution:
                child_args.append("--native-resolution")
            if disabled:
                child_args.append("--no-body-displacement")
            child_args.append(str(exe))
            return subprocess.run(child_args, cwd=ROOT, timeout=120, check=False).returncode

        if run_variant(False) != 0:
            return 1
        with_displacement = CAPTURE.read_bytes()
        if run_variant(True) != 0:
            run_variant(False)
            return 1
        without_displacement = CAPTURE.read_bytes()
        try:
            # Compare the newly captured pair, not the final restored image.
            temp_on = ROOT / "captures" / "ocean_body_displacement_on.bmp"
            temp_off = ROOT / "captures" / "ocean_body_displacement_off.bmp"
            temp_on.write_bytes(with_displacement)
            temp_off.write_bytes(without_displacement)
            mean_delta, changed_ratio, water_samples = compare_body_neighborhood_water(temp_on, temp_off)
        except (OSError, ValueError) as error:
            run_variant(False)
            print(f"[Ocean Runner Error] Could not compare body-displacement captures: {error}", file=sys.stderr)
            return 1
        restore_result = run_variant(False)
        if restore_result != 0:
            return 1
        print(f"[Ocean Runner] Water around floating bodies: displacement delta={mean_delta:.3f}; changed={changed_ratio:.2%}; samples={water_samples:,}.")
        if water_samples < 1000 or mean_delta < 0.35 or changed_ratio < 0.02:
            print("[Ocean Runner Error] Floating-body neighborhoods did not show a measurable water displacement response.", file=sys.stderr)
            return 1
        return 0
    started = time.time_ns()
    engine_args = [str(exe), "--ocean-test", f"--ocean-reflection-strength={reflection_strength:g}"]
    if native_resolution:
        engine_args.append("--native-resolution")
    if gif_30fps:
        engine_args.append("--ocean-gif-30fps")
    elif sequence_mode:
        engine_args.append("--ocean-sequence")
    if no_reflections:
        engine_args.append("--ocean-no-reflections")
    if no_body_displacement:
        engine_args.append("--ocean-no-body-displacement")
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
        sequence_events = {}
        event_pattern = re.compile(
            r"\[Ocean Sequence\] Frame (\d+): simulated=[\d.]+ seconds, impacts=(\d+), "
            r"crossings=(\d+)/(\d+), bodyY=([-\d.]+)\.\.([-\d.]+) m, bodyYs=([-\d.,]+)")
        for match in event_pattern.finditer(log):
            frame, impacts, entries, exits, min_y, max_y, body_ys = match.groups()
            sequence_events[int(frame)] = {
                "impacts": int(impacts), "entries": int(entries), "exits": int(exits),
                "min_y": float(min_y), "max_y": float(max_y),
                "body_ys": tuple(float(value) for value in body_ys.split(",")),
            }
        if not sequence_events or sequence_events.get(360, {}).get("impacts", 0) < 6:
            print("[Ocean Runner Error] The six-body drop did not produce six initial water impacts.", file=sys.stderr)
            return 1
        early_count = sequence_events.get(90)
        rebound = sequence_events.get(150)
        followup = sequence_events.get(180)
        late_count = sequence_events.get(360)
        if (early_count is None or rebound is None or followup is None or late_count is None or
                early_count["entries"] < 6 or late_count["entries"] < early_count["entries"]):
            print("[Ocean Runner Error] The six bodies did not register all initial water-entry crossings.", file=sys.stderr)
            return 1
        if not (len(early_count["body_ys"]) == len(rebound["body_ys"]) == len(followup["body_ys"]) == 6):
            print("[Ocean Runner Error] Per-body water sequence telemetry is incomplete.", file=sys.stderr)
            return 1
        rebound_then_drop = any(
            rebound["body_ys"][index] - early_count["body_ys"][index] > 0.8 and
            rebound["body_ys"][index] - followup["body_ys"][index] > 0.4
            for index in range(6))
        if not rebound_then_drop:
            print("[Ocean Runner Error] No individual Jolt body showed the measured rise-and-fall rebound.", file=sys.stderr)
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
        print(f"[Ocean Runner] Validated {len(sequence_paths)} fresh {capture_rate} water-drop frames; all six bodies entered the water, and per-body telemetry confirms a rise then fall across frames 90, 150, and 180.")
        print(f"[Ocean Runner] Registered impacts={late_count['impacts']}; downward/upward clearance crossings={late_count['entries']}/{late_count['exits']}. Bodies remained partly submerged, so repeated full-exit splash events are not expected by this scene's event definition.")
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
    if native_resolution and (width, height) != (1920, 1080):
        print(f"[Ocean Runner Error] Native-resolution ocean run rendered at {width}x{height}, expected 1920x1080.", file=sys.stderr)
        return 1
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
