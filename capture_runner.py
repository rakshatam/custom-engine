#!/usr/bin/env python3
"""Run the engine's frame-driven capture test and validate its BMP outputs."""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import time
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent
LOG_PATH = PROJECT_ROOT / "engine_run_log.txt"
CAPTURES = [
    "01_pathtracing_gi.bmp",
    "02_direct_shadows.bmp",
    "03_golden_hour_soft_shadows_gi.bmp",
    "04_reinhard_exposure.bmp",
    "05_surface_normals_debug.bmp",
    "06_full_editor_ui.bmp",
    "07_camera_motion_taa.bmp",
    "08_material_upload.bmp",
]


def inspect_bmp(path: Path) -> tuple[int, int, list[tuple[int, int, int]]]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a BMP file")
    declared_size = struct.unpack_from("<I", data, 2)[0]
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bits_per_pixel = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    height = abs(signed_height)
    row_stride = ((width * bits_per_pixel + 31) // 32) * 4
    if (declared_size != len(data) or dib_size < 40 or width <= 0 or height <= 0
            or planes != 1 or bits_per_pixel != 24 or compression != 0
            or pixel_offset < 54 or pixel_offset + row_stride * height > len(data)):
        raise ValueError("invalid or unsupported BMP layout")

    pixels: list[tuple[int, int, int]] = []
    step_x = max(1, width // 96)
    step_y = max(1, height // 72)
    for y in range(0, height, step_y):
        stored_y = y if signed_height < 0 else height - 1 - y
        row_start = pixel_offset + stored_y * row_stride
        for x in range(0, width, step_x):
            blue, green, red = data[row_start + x * 3:row_start + x * 3 + 3]
            pixels.append((red, green, blue))
    return width, height, pixels


def main() -> int:
    args = sys.argv[1:]
    no_nrd = "--no-nrd" in args
    native_resolution = "--native-resolution" in args
    args = [arg for arg in args if arg not in {"--no-nrd", "--native-resolution"}]
    if len(args) > 1:
        print("usage: capture_runner.py [--no-nrd] [--native-resolution] [ENGINE.exe]", file=sys.stderr)
        return 2
    engine_exe = Path(args[0]) if args else Path("build/Release/CustomEngine.exe")
    if not engine_exe.is_absolute():
        engine_exe = PROJECT_ROOT / engine_exe
    engine_exe = engine_exe.resolve()

    if not engine_exe.is_file():
        print(f"[Runner Error] Engine executable not found: {engine_exe}", file=sys.stderr)
        return 2

    start_time_ns = time.time_ns()
    engine_args = [str(engine_exe), "--test"]
    if no_nrd:
        engine_args.append("--no-nrd")
    if native_resolution:
        engine_args.append("--native-resolution")
    print(f"[Runner] Launching {' '.join(engine_args)}")
    try:
        result = subprocess.run(
            engine_args,
            cwd=PROJECT_ROOT,
            timeout=180,
            check=False,
        )
    except subprocess.TimeoutExpired:
        print("[Runner Error] Capture test exceeded the 180-second timeout.", file=sys.stderr)
        return 1

    log_text = LOG_PATH.read_text(encoding="utf-8", errors="replace") if LOG_PATH.exists() else ""
    if not LOG_PATH.exists() or LOG_PATH.stat().st_mtime_ns < start_time_ns:
        print("[Runner Error] The engine did not write a fresh run log.", file=sys.stderr)
        return 1
    if result.returncode != 0:
        print(f"[Runner Error] Engine exited with code {result.returncode}.", file=sys.stderr)
        print(log_text[-6000:], file=sys.stderr)
        return 1

    if "Test suite finished successfully" not in log_text:
        print("[Runner Error] Engine exited before the capture sequence completed.", file=sys.stderr)
        print(log_text[-6000:], file=sys.stderr)
        return 1
    if "[DXR] Selected hardware adapter supports Tier" not in log_text or "[DXR] DispatchRays recorded" not in log_text:
        print("[Runner Error] Hardware DXR capability or active ray dispatch was not reported.", file=sys.stderr)
        return 1

    if native_resolution and "[Engine] Render resolution: 1920x1080" not in log_text:
        print("[Runner Error] Native-resolution run did not initialize a 1920x1080 render target.", file=sys.stderr)
        return 1

    milestone_results = re.findall(r"\[Test Milestone (\d+)\].*\((SUCCESS|FAILED)\)", log_text)
    failed = [number for number, status in milestone_results if status != "SUCCESS"]
    milestone_ids = sorted(int(number) for number, _status in milestone_results)
    if failed or milestone_ids != list(range(1, len(CAPTURES) + 1)):
        print(f"[Runner Error] Capture milestones failed or missing: {', '.join(failed or map(str, milestone_ids))}", file=sys.stderr)
        return 1
    if "[Scene] Uploaded material edits:" not in log_text or "[Test Sequence] Queued material overrides" not in log_text:
        print("[Runner Error] Material Inspector regression did not reach the GPU upload path.", file=sys.stderr)
        return 1

    captures_dir = PROJECT_ROOT / "captures"
    missing = []
    stale = []
    for name in CAPTURES:
        path = captures_dir / name
        try:
            data = path.read_bytes()
        except OSError:
            missing.append(name)
            continue
        if path.stat().st_mtime_ns < start_time_ns:
            stale.append(name)
            continue
        try:
            width, height, pixels = inspect_bmp(path)
        except (OSError, ValueError):
            missing.append(name)
            continue
        if width < 64 or height < 64 or len(set(pixels)) < 32 or sum(map(sum, pixels)) / (len(pixels) * 3) < 2.0:
            missing.append(name)
        if name == "08_material_upload.bmp":
            magenta_pixels = sum(1 for red, green, blue in pixels if red > 100 and blue > 80 and green < min(red, blue) * 0.55)
            magenta_ratio = magenta_pixels / max(1, len(pixels))
            if magenta_ratio < 0.10:
                missing.append(f"{name} (only {magenta_ratio:.1%} magenta pixels; GPU edit not visible)")
            else:
                print(f"[Runner] Material edit regression capture: {magenta_ratio:.1%} magenta pixels")

    if missing:
        print(f"[Runner Error] Missing or invalid BMP captures: {', '.join(missing)}", file=sys.stderr)
        return 1
    if stale:
        print(f"[Runner Error] Captures were not refreshed by this run: {', '.join(stale)}", file=sys.stderr)
        return 1

    direct_fps = re.search(r"\[FPS Check\] Direct \+ DXR shadows average: .*?\((\d+(?:\.\d+)?) FPS\)", log_text)
    gi_fps = re.search(r"\[FPS Check\] GI steady-state average \(frames 150-169\): .*?\((\d+(?:\.\d+)?) FPS\)", log_text)
    if direct_fps:
        print(f"[Runner] Direct + DXR shadows average: {direct_fps.group(1)} FPS")
    if gi_fps:
        print(f"[Runner] Path-traced GI steady-state average: {gi_fps.group(1)} FPS")
    resolution = re.search(r"\[Engine\] Render resolution: (\d+x\d+)", log_text)
    if resolution:
        print(f"[Runner] Render resolution: {resolution.group(1)}")
    print(f"[Runner] Verified {len(CAPTURES)} engine captures and {len(milestone_results)} successful milestones.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
