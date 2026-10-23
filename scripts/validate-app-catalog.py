#!/usr/bin/env python3
"""Validate that bundled app manifests and build targets agree."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MANIFESTS = ROOT / "rootfs" / "Nexora" / "Apps"
APP_SOURCES = ROOT / "user" / "apps"
SYSTEM_PATH = re.compile(r"^C:\\Nexora\\System\\([A-Za-z0-9_-]+)\.exe$", re.IGNORECASE)


def parse_manifest(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise ValueError(f"{path.relative_to(ROOT)}:{number}: expected Key=Value")
        normalized = key.strip().lower()
        if normalized in values:
            raise ValueError(f"{path.relative_to(ROOT)}:{number}: duplicate key {key.strip()}")
        values[normalized] = value.strip()
    for required in ("name", "path"):
        if not values.get(required):
            raise ValueError(f"{path.relative_to(ROOT)}: {required} is missing")
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", nargs="+", required=True, help="built-in executable basenames")
    args = parser.parse_args()

    available = {name.lower() for name in args.binary}
    manifests = sorted(MANIFESTS.glob("*.app"))
    errors: list[str] = []
    paths: dict[str, Path] = {}
    seen_names: dict[str, Path] = {}

    for manifest in manifests:
        try:
            values = parse_manifest(manifest)
        except (OSError, UnicodeError, ValueError) as error:
            errors.append(str(error))
            continue

        name = values["name"].casefold()
        if name in seen_names:
            errors.append(
                f"{manifest.relative_to(ROOT)}: duplicate app name also used by "
                f"{seen_names[name].relative_to(ROOT)}"
            )
        else:
            seen_names[name] = manifest

        match = SYSTEM_PATH.fullmatch(values["path"])
        if not match:
            errors.append(
                f"{manifest.relative_to(ROOT)}: Path must target "
                r"C:\Nexora\System\<name>.exe"
            )
            continue
        binary = match.group(1).lower()
        if binary not in available:
            errors.append(
                f"{manifest.relative_to(ROOT)}: {binary}.exe has no build target"
            )
        if binary in paths:
            errors.append(
                f"{manifest.relative_to(ROOT)}: executable also registered by "
                f"{paths[binary].relative_to(ROOT)}"
            )
        else:
            paths[binary] = manifest

    if not manifests:
        errors.append(f"{MANIFESTS.relative_to(ROOT)}: no app manifests found")

    for source_dir in sorted(path for path in APP_SOURCES.iterdir() if path.is_dir()):
        if not any(source_dir.glob("*.cpp")):
            continue
        if source_dir.name.lower() not in paths:
            errors.append(
                f"user/apps/{source_dir.name}: app sources have no matching "
                f"rootfs/Nexora/Apps/{source_dir.name}.app manifest"
            )

    if errors:
        for error in errors:
            print(f"app catalog: error: {error}", file=sys.stderr)
        return 1

    print(f"app catalog: {len(manifests)} manifests validated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
