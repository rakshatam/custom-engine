#!/usr/bin/env python3
"""Compare scene-wide DXR reflections on Sponza and a Khronos glTF sample."""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
LOG = ROOT / "engine_run_log.txt"
CAPTURE_NAMES = (
    "09_reflections_on.bmp",
    "10_reflections_off.bmp",
    "11_reflections_restored.bmp",
)


def bmp_info(path: Path) -> tuple[int, int, int, int]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"{path.name} is not a BMP")
    file_size = struct.unpack_from("<I", data, 2)[0]
    offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bits = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    height = abs(signed_height)
    stride = ((width * bits + 31) // 32) * 4
    if (file_size != len(data) or dib_size < 40 or width < 1 or height < 1 or planes != 1
            or bits != 24 or compression != 0 or offset < 54 or offset + stride * height > len(data)):
        raise ValueError(f"{path.name} has an invalid or unsupported BMP layout")
    return width, height, offset, stride


def image_delta(a_path: Path, b_path: Path) -> tuple[float, float]:
    a = a_path.read_bytes()
    b = b_path.read_bytes()
    aw, ah, ao, astride = bmp_info(a_path)
    bw, bh, bo, bstride = bmp_info(b_path)
    if (aw, ah) != (bw, bh):
        raise ValueError("reflection captures have different dimensions")
    changed_pixels = 0
    delta_sum = 0
    channel_count = aw * ah * 3
    for y in range(ah):
        arow = ao + y * astride
        brow = bo + y * bstride
        for x in range(aw):
            p = x * 3
            delta = sum(abs(a[arow + p + c] - b[brow + p + c]) for c in range(3))
            delta_sum += delta
            changed_pixels += delta != 0
    return delta_sum / channel_count, changed_pixels / (aw * ah)


def run_scene(exe: Path, khronos: bool, native_resolution: bool) -> bool:
    started = time.time_ns()
    args = [str(exe), "--reflection-compare"]
    if native_resolution:
        args.append("--native-resolution")
    if khronos:
        args.append("--reflection-khronos")
    label = "Khronos Metal/Rough Spheres" if khronos else "Sponza"
    print(f"[Reflection Runner] Launching {label} comparison")
    try:
        result = subprocess.run(args, cwd=ROOT, timeout=180, check=False)
    except subprocess.TimeoutExpired:
        print(f"[Reflection Runner Error] {label} capture exceeded 180 seconds.", file=sys.stderr)
        return False

    log = LOG.read_text(encoding="utf-8", errors="replace") if LOG.exists() else ""
    if not LOG.exists() or LOG.stat().st_mtime_ns < started:
        print("[Reflection Runner Error] No fresh engine run log was written.", file=sys.stderr)
        return False
    if result.returncode != 0 or "[Reflection Test] Capture sequence completed" not in log:
        print(f"[Reflection Runner Error] {label} engine run did not finish cleanly.\n{log[-5000:]}", file=sys.stderr)
        return False
    if "[DXR] Selected hardware adapter supports Tier" not in log or "[DXR] DispatchRays recorded" not in log:
        print("[Reflection Runner Error] Hardware DXR capability or active dispatch was not reported.", file=sys.stderr)
        return False
    if khronos and "[Reflection Lab] Loaded Khronos glTF PBR sample" not in log:
        print("[Reflection Runner Error] Khronos test scene did not report an imported asset.", file=sys.stderr)
        return False

    milestone_ids = re.findall(r"\[Test Milestone (\d+)\].*\((SUCCESS|FAILED)\)", log)
    if [(number, status) for number, status in milestone_ids] != [("9", "SUCCESS"), ("10", "SUCCESS"), ("11", "SUCCESS")]:
        print(f"[Reflection Runner Error] Expected successful reflection captures, got {milestone_ids}.", file=sys.stderr)
        return False

    paths = [ROOT / "captures" / name for name in CAPTURE_NAMES]
    if any(not path.is_file() or path.stat().st_mtime_ns < started for path in paths):
        print("[Reflection Runner Error] One or more reflection captures are missing or stale.", file=sys.stderr)
        return False
    try:
        mean_delta, changed_ratio = image_delta(paths[0], paths[1])
    except (OSError, ValueError) as error:
        print(f"[Reflection Runner Error] {error}", file=sys.stderr)
        return False
    print(f"[Reflection Runner] {label}: {mean_delta:.3f} mean channel delta, {changed_ratio:.2%} changed pixels.")
    if mean_delta < 0.04 or changed_ratio < 0.001:
        print(f"[Reflection Runner Error] Toggle image delta is too small ({mean_delta:.4f}, {changed_ratio:.2%}).", file=sys.stderr)
        return False

    enabled = re.search(r"enabled direct avg=([\d.]+) ms \(([\d.]+) FPS\)", log)
    disabled = re.search(r"disabled avg=([\d.]+) ms \(([\d.]+) FPS\)", log)
    if not enabled or not disabled:
        print("[Reflection Runner Error] Enabled/disabled frame-time measurement is missing.", file=sys.stderr)
        return False
    enabled_fps, disabled_fps = float(enabled.group(2)), float(disabled.group(2))
    print(f"[Reflection Runner] DXR on/off: {enabled_fps:.2f}/{disabled_fps:.2f} FPS at {bmp_info(paths[0])[0]}x{bmp_info(paths[0])[1]}.")
    if native_resolution and bmp_info(paths[0])[:2] != (1920, 1080):
        print("[Reflection Runner Error] Native-resolution Sponza capture did not render at 1920x1080.", file=sys.stderr)
        return False
    if enabled_fps <= 0.0 or disabled_fps <= 0.0 or enabled_fps < disabled_fps * 0.55:
        print(f"[Reflection Runner Error] Reflection path exceeded the 45% FPS-cost budget ({enabled_fps:.2f}/{disabled_fps:.2f}).", file=sys.stderr)
        return False
    return True


def main() -> int:
    args = sys.argv[1:]
    native_resolution = "--native-resolution" in args
    args = [arg for arg in args if arg != "--native-resolution"]
    scene = "all"
    if "--scene" in args:
        index = args.index("--scene")
        if index + 1 >= len(args):
            print("usage: reflection_capture_runner.py [--scene all|sponza|khronos] [--native-resolution] [ENGINE.exe]", file=sys.stderr)
            return 2
        scene = args[index + 1].lower()
        del args[index:index + 2]
    if len(args) > 1 or scene not in {"all", "sponza", "khronos"}:
        print("usage: reflection_capture_runner.py [--scene all|sponza|khronos] [--native-resolution] [ENGINE.exe]", file=sys.stderr)
        return 2
    exe = Path(args[0]) if args else Path("build/Release/CustomEngine.exe")
    if not exe.is_absolute():
        exe = ROOT / exe
    exe = exe.resolve()
    if not exe.is_file():
        print(f"[Reflection Runner Error] Engine executable not found: {exe}", file=sys.stderr)
        return 2
    scenes = ([False, True] if scene == "all" else [scene == "khronos"])
    return 0 if all(run_scene(exe, khronos, native_resolution) for khronos in scenes) else 1


if __name__ == "__main__":
    raise SystemExit(main())
