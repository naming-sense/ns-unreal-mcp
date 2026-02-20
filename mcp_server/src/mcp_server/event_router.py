from __future__ import annotations

import asyncio
from collections import defaultdict, deque
from dataclasses import dataclass
import logging
from typing import TYPE_CHECKING
from typing import Any, Callable

if TYPE_CHECKING:
    from mcp_server.metrics import RuntimeMetrics

LOGGER = logging.getLogger("mcp_server.event_router")

EventListener = Callable[[dict[str, Any]], None]


@dataclass
class _SubscriptionState:
    token: int
    request_id: str | None
    queue: asyncio.Queue[dict[str, Any]]
    dropped_count: int = 0


class EventSubscription:
    def __init__(
        self,
        *,
        router: "EventRouter",
        token: int,
        request_id: str | None,
        queue: asyncio.Queue[dict[str, Any]],
    ) -> None:
        self._router = router
        self._token = token
        self._request_id = request_id
        self._queue = queue
        self._closed = False

    @property
    def request_id(self) -> str | None:
        return self._request_id

    @property
    def dropped_count(self) -> int:
        return self._router.get_dropped_count(self._token)

    async def get(self, timeout_s: float | None = None) -> dict[str, Any] | None:
        if self._closed:
            return None

        try:
            if timeout_s is None:
                return await self._queue.get()
            return await asyncio.wait_for(self._queue.get(), timeout_s)
        except asyncio.TimeoutError:
            return None

    def get_nowait(self) -> dict[str, Any] | None:
        if self._closed:
            return None

        try:
            return self._queue.get_nowait()
        except asyncio.QueueEmpty:
            return None

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._router.unsubscribe(self._token)


class EventRouter:
    def __init__(
        self,
        *,
        max_events_per_request: int = 200,
        max_global_events: int = 2_000,
        metrics: "RuntimeMetrics | None" = None,
    ) -> None:
        self._max_events_per_request = max_events_per_request
        self._events_by_request_id: dict[str, deque[dict[str, Any]]] = defaultdict(
            lambda: deque(maxlen=max_events_per_request)
        )
        self._global_events: deque[dict[str, Any]] = deque(maxlen=max_global_events)
        self._listeners: set[EventListener] = set()
        self._subscriptions: dict[int, _SubscriptionState] = {}
        self._next_subscription_token = 1
        self._metrics = metrics

    def publish(self, event: dict[str, Any]) -> None:
        normalized_event = self.normalize_event(event)
        request_id = str(normalized_event.get("request_id", "") or "")
        event_type = str(normalized_event.get("event_type", "") or "unknown")

        self._global_events.append(normalized_event)
        if request_id:
            self._events_by_request_id[request_id].append(normalized_event)
        if self._metrics is not None:
            self._metrics.inc("event_router.event_published")
            self._metrics.inc(f"event_router.event_type.{event_type}")
            self._metrics.set_gauge("event_router.global_buffer_size", len(self._global_events))

        for listener in list(self._listeners):
            try:
                listener(normalized_event)
            except Exception:
                LOGGER.exception("Event listener raised exception.")

        for subscription in list(self._subscriptions.values()):
            if (
                subscription.request_id is not None
                and subscription.request_id != request_id
            ):
                continue

            try:
                subscription.queue.put_nowait(normalized_event)
            except asyncio.QueueFull:
                subscription.dropped_count += 1
                if self._metrics is not None:
                    self._metrics.inc("event_router.subscription_dropped")

    def add_listener(self, listener: EventListener) -> None:
        self._listeners.add(listener)

    def remove_listener(self, listener: EventListener) -> None:
        self._listeners.discard(listener)

    def subscribe(
        self,
        *,
        request_id: str | None = None,
        queue_size: int = 256,
    ) -> EventSubscription:
        if queue_size <= 0:
            raise ValueError("queue_size must be > 0")

        token = self._next_subscription_token
        self._next_subscription_token += 1

        subscription_state = _SubscriptionState(
            token=token,
            request_id=request_id,
            queue=asyncio.Queue(maxsize=queue_size),
        )
        self._subscriptions[token] = subscription_state
        if self._metrics is not None:
            self._metrics.inc("event_router.subscription_created")
            self._metrics.set_gauge("event_router.subscriptions", len(self._subscriptions))
        return EventSubscription(
            router=self,
            token=token,
            request_id=request_id,
            queue=subscription_state.queue,
        )

    def unsubscribe(self, token: int) -> None:
        removed = self._subscriptions.pop(token, None)
        if removed is not None and self._metrics is not None:
            self._metrics.inc("event_router.subscription_closed")
            self._metrics.set_gauge("event_router.subscriptions", len(self._subscriptions))

    def get_dropped_count(self, token: int) -> int:
        subscription = self._subscriptions.get(token)
        if subscription is None:
            return 0
        return subscription.dropped_count

    def get_events(self, request_id: str) -> list[dict[str, Any]]:
        return list(self._events_by_request_id.get(request_id, []))

    def get_recent_events(self, limit: int = 100) -> list[dict[str, Any]]:
        if limit <= 0:
            return []
        return list(self._global_events)[-limit:]

    @staticmethod
    def normalize_event(event: dict[str, Any]) -> dict[str, Any]:
        event_type = str(event.get("event_type", "") or "")
        request_id = str(event.get("request_id", "") or "")
        timestamp_ms = int(event.get("timestamp_ms", 0) or 0)
        payload = event.get("payload")
        if not isinstance(payload, dict):
            payload = {}

        normalized: dict[str, Any] = {
            "type": "ue.event",
            "event_id": str(event.get("event_id", "") or ""),
            "event_type": event_type,
            "request_id": request_id,
            "timestamp_ms": timestamp_ms,
            "payload": payload,
            "notification_kind": "other",
        }

        if event_type == "event.progress":
            normalized["notification_kind"] = "progress"
            normalized["percent"] = payload.get("percent")
            normalized["phase"] = payload.get("phase")
        elif event_type == "event.log":
            normalized["notification_kind"] = "log"
            normalized["level"] = payload.get("level")
            normalized["message"] = payload.get("message")
        elif event_type == "event.artifact":
            normalized["notification_kind"] = "artifact"
        elif event_type == "event.job.status":
            normalized["notification_kind"] = "job_status"
        elif event_type == "event.changeset.created":
            normalized["notification_kind"] = "changeset"

        return normalized
