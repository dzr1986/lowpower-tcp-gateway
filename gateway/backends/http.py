"""
HTTP backend for the low-power TCP gateway.

POSTs device data as JSON to a configurable HTTP endpoint::

    POST {url}
    Content-Type: application/json

    {
        "device_id": 12345,
        "timestamp": 1700000000.0,
        "seq": 42,
        "payload": "deadbeef...",
        "payload_size": 4,
        "msg_type": "DATA"
    }
"""

import asyncio
import json
import logging
import time
from typing import Optional

from gateway.config import HTTPConfig

logger = logging.getLogger(__name__)

try:
    import aiohttp  # type: ignore
    HAS_AIOHTTP = True
except ImportError:
    HAS_AIOHTTP = False


class HTTPBackend:
    """Async HTTP POST backend."""

    def __init__(self, config: HTTPConfig) -> None:
        self._config = config
        self._session: Optional[object] = None
        self._queue: asyncio.Queue = asyncio.Queue(maxsize=1000)
        self._worker_task: Optional[asyncio.Task] = None

    async def start(self) -> None:
        if not self._config.enabled:
            logger.info("HTTP backend disabled")
            return
        if not HAS_AIOHTTP:
            logger.warning(
                "aiohttp is not installed; HTTP backend will not be active. "
                "Install it with: pip install aiohttp"
            )
            return

        self._worker_task = asyncio.ensure_future(self._post_worker())
        logger.info("HTTP backend started (url=%s)", self._config.url)

    async def stop(self) -> None:
        if self._worker_task:
            self._worker_task.cancel()
            try:
                await self._worker_task
            except asyncio.CancelledError:
                pass
        logger.info("HTTP backend stopped")

    async def send(self, device_id: int, payload: bytes, metadata: dict) -> None:
        if not self._config.enabled or not HAS_AIOHTTP:
            return
        body = json.dumps(
            {
                "device_id": device_id,
                "timestamp": time.time(),
                "seq": metadata.get("seq", 0),
                "payload": payload.hex(),
                "payload_size": len(payload),
                "msg_type": metadata.get("msg_type", "DATA"),
            }
        ).encode()
        try:
            self._queue.put_nowait(body)
        except asyncio.QueueFull:
            logger.warning(
                "HTTP publish queue full; dropping message for device %#010x",
                device_id,
            )

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    async def _post_worker(self) -> None:
        headers = {
            "Content-Type": "application/json",
            **self._config.headers,
        }
        if self._config.auth_token:
            headers["Authorization"] = f"Bearer {self._config.auth_token}"

        async with aiohttp.ClientSession(  # type: ignore[name-defined]
            headers=headers,
            timeout=aiohttp.ClientTimeout(total=self._config.timeout),  # type: ignore[name-defined]
        ) as session:
            while True:
                body = await self._queue.get()
                await self._post_with_retry(session, body)

    async def _post_with_retry(self, session, body: bytes) -> None:
        backoff = 1
        for attempt in range(5):
            try:
                async with session.post(self._config.url, data=body) as resp:
                    if resp.status < 400:
                        return
                    logger.warning(
                        "HTTP backend received status %d (attempt %d)",
                        resp.status,
                        attempt + 1,
                    )
            except Exception as exc:
                logger.warning(
                    "HTTP post attempt %d failed: %s (retrying in %ds)",
                    attempt + 1,
                    exc,
                    backoff,
                )
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)
        logger.error("HTTP: failed to post after 5 attempts")
