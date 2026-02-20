from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
from typing import TypeVar

T = TypeVar("T")


def next_backoff_delay(current_delay: float, max_delay: float) -> float:
    if current_delay <= 0:
        return max_delay
    return min(current_delay * 2.0, max_delay)


async def call_with_retry(
    operation: Callable[[], Awaitable[T]],
    *,
    is_retryable_error: Callable[[Exception], bool],
    max_attempts: int,
    initial_backoff_s: float,
    max_backoff_s: float,
    on_retry: Callable[[Exception, int, float], Awaitable[None] | None] | None = None,
) -> T:
    if max_attempts <= 0:
        raise ValueError("max_attempts must be > 0")
    if initial_backoff_s <= 0:
        raise ValueError("initial_backoff_s must be > 0")
    if max_backoff_s <= 0:
        raise ValueError("max_backoff_s must be > 0")

    attempt = 1
    delay_s = initial_backoff_s
    last_error: Exception | None = None

    while attempt <= max_attempts:
        try:
            return await operation()
        except Exception as exc:
            last_error = exc
            if attempt >= max_attempts or not is_retryable_error(exc):
                raise

            if on_retry is not None:
                maybe_awaitable = on_retry(exc, attempt, delay_s)
                if asyncio.iscoroutine(maybe_awaitable):
                    await maybe_awaitable

            await asyncio.sleep(delay_s)
            attempt += 1
            delay_s = next_backoff_delay(delay_s, max_backoff_s)

    if last_error is not None:
        raise last_error
    raise RuntimeError("retry execution failed without an error")
