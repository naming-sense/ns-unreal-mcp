from __future__ import annotations

import os
from pathlib import Path


def is_wsl() -> bool:
    if os.environ.get("WSL_DISTRO_NAME") or os.environ.get("WSL_INTEROP"):
        return True
    try:
        return "microsoft" in Path("/proc/version").read_text(encoding="utf-8").lower()
    except OSError:
        return False


def parse_default_gateway_ip(proc_net_route_text: str) -> str | None:
    lines = proc_net_route_text.splitlines()
    if not lines:
        return None

    for line in lines[1:]:
        fields = line.strip().split()
        if len(fields) < 8:
            continue

        destination = fields[1]
        gateway_hex = fields[2]
        mask = fields[7]

        if destination != "00000000" or mask != "00000000":
            continue

        try:
            gateway_value = int(gateway_hex, 16)
        except ValueError:
            continue

        # /proc/net/route uses little-endian hex for the gateway.
        octet1 = gateway_value & 0xFF
        octet2 = (gateway_value >> 8) & 0xFF
        octet3 = (gateway_value >> 16) & 0xFF
        octet4 = (gateway_value >> 24) & 0xFF
        return f"{octet1}.{octet2}.{octet3}.{octet4}"

    return None


def get_wsl_default_gateway_ip() -> str | None:
    try:
        route_text = Path("/proc/net/route").read_text(encoding="utf-8")
    except OSError:
        return None
    return parse_default_gateway_ip(route_text)

