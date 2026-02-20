from __future__ import annotations

import pytest

from mcp_server.event_router import EventRouter


def test_normalize_event_progress_and_log() -> None:
    progress_event = EventRouter.normalize_event(
        {
            "event_id": "evt-1",
            "event_type": "event.progress",
            "request_id": "req-1",
            "timestamp_ms": 100,
            "payload": {"percent": 30, "phase": "running"},
        }
    )
    assert progress_event["type"] == "ue.event"
    assert progress_event["notification_kind"] == "progress"
    assert progress_event["phase"] == "running"

    log_event = EventRouter.normalize_event(
        {
            "event_id": "evt-2",
            "event_type": "event.log",
            "request_id": "req-1",
            "timestamp_ms": 110,
            "payload": {"level": "warning", "message": "warn"},
        }
    )
    assert log_event["notification_kind"] == "log"
    assert log_event["level"] == "warning"
    assert log_event["message"] == "warn"


@pytest.mark.asyncio
async def test_subscription_filters_by_request_id() -> None:
    router = EventRouter()
    subscription = router.subscribe(request_id="req-1", queue_size=4)
    try:
        router.publish(
            {
                "event_id": "evt-1",
                "event_type": "event.progress",
                "request_id": "req-1",
                "timestamp_ms": 100,
                "payload": {"percent": 10, "phase": "a"},
            }
        )
        router.publish(
            {
                "event_id": "evt-2",
                "event_type": "event.progress",
                "request_id": "req-2",
                "timestamp_ms": 101,
                "payload": {"percent": 20, "phase": "b"},
            }
        )

        received = await subscription.get(timeout_s=0.1)
        assert received is not None
        assert received["request_id"] == "req-1"
        assert received["notification_kind"] == "progress"
        assert await subscription.get(timeout_s=0.05) is None
    finally:
        subscription.close()


@pytest.mark.asyncio
async def test_subscription_drop_count_when_queue_full() -> None:
    router = EventRouter()
    subscription = router.subscribe(request_id="req-1", queue_size=1)
    try:
        router.publish(
            {
                "event_id": "evt-1",
                "event_type": "event.log",
                "request_id": "req-1",
                "timestamp_ms": 100,
                "payload": {"message": "m1"},
            }
        )
        router.publish(
            {
                "event_id": "evt-2",
                "event_type": "event.log",
                "request_id": "req-1",
                "timestamp_ms": 101,
                "payload": {"message": "m2"},
            }
        )
        assert subscription.dropped_count == 1
    finally:
        subscription.close()
