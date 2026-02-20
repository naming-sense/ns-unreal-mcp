from __future__ import annotations

from mcp_server.utils.wsl import parse_default_gateway_ip


def test_parse_default_gateway_ip_from_proc_net_route() -> None:
    # Example adapted from /proc/net/route format.
    sample = "\n".join(
        [
            "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT",
            "eth0\t00000000\t010020AC\t0003\t0\t0\t0\t00000000\t0\t0\t0",
            "eth0\t0020A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0",
        ]
    )
    # Gateway 0xAC200001 in little-endian -> 172.32.0.1
    assert parse_default_gateway_ip(sample) == "172.32.0.1"


def test_parse_default_gateway_ip_returns_none_when_missing() -> None:
    sample = "\n".join(
        [
            "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT",
            "eth0\t0020A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0",
        ]
    )
    assert parse_default_gateway_ip(sample) is None

