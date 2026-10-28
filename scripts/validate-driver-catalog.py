#!/usr/bin/env python3
"""Validate Nexora's bundled native-driver descriptors before packaging the system tree."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT / "rootfs" / "Nexora" / "Drivers" / "Repository"
REQUIRED = {"format", "name", "provider", "architecture", "bus", "hardware-ids", "source",
            "status", "physical-test", "capabilities", "firmware"}
STATUSES = {"qemu-tested", "implemented-unverified", "experimental"}
ID = re.compile(r"^(?:[0-9A-Fa-f]{4}:[0-9A-Fa-f]{4}|class:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}|pci:any)$")


def main() -> int:
    files = sorted(REPOSITORY.rglob("*.nxd"))
    if not files:
        print("driver catalog: no .nxd descriptors found", file=sys.stderr)
        return 1
    all_ids: dict[str, Path] = {}
    errors: list[str] = []
    for path in files:
        values: dict[str, str] = {}
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                errors.append(f"{path.relative_to(ROOT)}:{number}: expected key = value")
                continue
            key, value = (part.strip() for part in line.split("=", 1))
            if key in values:
                errors.append(f"{path.relative_to(ROOT)}:{number}: duplicate key {key}")
            values[key] = value
        missing = REQUIRED - values.keys()
        if missing:
            errors.append(f"{path.relative_to(ROOT)}: missing {', '.join(sorted(missing))}")
            continue
        if values["format"] != "nexora-driver-package-v1":
            errors.append(f"{path.relative_to(ROOT)}: unsupported descriptor format")
        if values["architecture"] != "x86_64":
            errors.append(f"{path.relative_to(ROOT)}: unsupported architecture")
        if values["status"] not in STATUSES:
            errors.append(f"{path.relative_to(ROOT)}: invalid evidence status {values['status']}")
        if not values["name"] or not values["capabilities"]:
            errors.append(f"{path.relative_to(ROOT)}: name and capabilities must not be empty")
        source = values["source"]
        if not source.startswith("kernel:") or not (ROOT / source.removeprefix("kernel:")).is_file():
            errors.append(f"{path.relative_to(ROOT)}: source does not exist: {source}")
        for hardware_id in values["hardware-ids"].split(","):
            hardware_id = hardware_id.strip()
            if not ID.fullmatch(hardware_id):
                errors.append(f"{path.relative_to(ROOT)}: malformed hardware ID {hardware_id}")
                continue
            owner = all_ids.get(hardware_id.lower())
            if owner and owner != path:
                errors.append(f"{path.relative_to(ROOT)}: duplicate hardware ID {hardware_id} also in {owner.relative_to(ROOT)}")
            all_ids[hardware_id.lower()] = path
    if errors:
        print("driver catalog: invalid", file=sys.stderr)
        print("\n".join(f"  {error}" for error in errors), file=sys.stderr)
        return 1
    print(f"driver catalog: {len(files)} descriptors validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
