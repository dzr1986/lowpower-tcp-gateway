"""
MQTT backend for the low-power TCP gateway.

Publishes device data as JSON to::

    {topic_prefix}/devices/{device_id_hex}/data

Registration events go to::

    {topic_prefix}/devices/{device_id_hex}/register
"""

import asyncio
import json
import logging
import time
from typing import Optional

from gateway.config import MQTTConfig

logger = logging.getLogger(__name__)

try:
    import aiomqtt  # type: ignore
    HAS_AIOMQTT = True
except ImportError:
    HAS_AIOMQTT = False


class MQTTBackend:
    """Async MQTT publisher backend."""

    def __init__(self, config: MQTTConfig) -> None:
        self._config = config
        self._client: Optional[object] = None
        self._connected = False
        self._reconnect_task: Optional[asyncio.Task] = None
        self._queue: asyncio.Queue = asyncio.Queue(maxsize=1000)
        self._worker_task: Optional[asyncio.Task] = None

    async def start(self) -> None:
        if not self._config.enabled:
            logger.info("MQTT backend disabled")
            return
        if not HAS_AIOMQTT:
            logger.warning(
                "aiomqtt is not installed; MQTT backend will not be active. "
                "Install it with: pip install aiomqtt"
            )
            return

        self._worker_task = asyncio.ensure_future(self._publish_worker())
        logger.info(
            "MQTT backend started (broker=%s:%s)",
            self._config.host,
            self._config.port,
        )

    async def stop(self) -> None:
        if self._worker_task:
            self._worker_task.cancel()
            try:
                await self._worker_task
            except asyncio.CancelledError:
                pass
        logger.info("MQTT backend stopped")

    async def send(self, device_id: int, payload: bytes, metadata: dict) -> None:
        if not self._config.enabled or not HAS_AIOMQTT:
            return
        msg_type = metadata.get("msg_type", "DATA")
        topic_suffix = "register" if msg_type == "REGISTER" else "data"
        topic = (
            f"{self._config.topic_prefix}/devices/"
            f"{device_id:#010x}/{topic_suffix}"
        )
        message = json.dumps(
            {
                "device_id": device_id,
                "timestamp": time.time(),
                "seq": metadata.get("seq", 0),
                "payload": payload.hex(),
                "payload_size": len(payload),
            }
        ).encode()
        try:
            self._queue.put_nowait((topic, message))
        except asyncio.QueueFull:
            logger.warning("MQTT publish queue full; dropping message for device %#010x", device_id)

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    async def _publish_worker(self) -> None:
        """Drain the publish queue, (re-)connecting as needed."""
        while True:
            topic, message = await self._queue.get()
            await self._publish_with_retry(topic, message)

    async def _publish_with_retry(self, topic: str, message: bytes) -> None:
        backoff = 1
        for attempt in range(5):
            try:
                async with aiomqtt.Client(  # type: ignore[name-defined]
                    hostname=self._config.host,
                    port=self._config.port,
                    username=self._config.username or None,
                    password=self._config.password or None,
                    identifier=self._config.client_id,
                    keepalive=self._config.keepalive,
                ) as client:
                    await client.publish(topic, message, qos=self._config.qos)
                    return
            except Exception as exc:
                logger.warning(
                    "MQTT publish attempt %d failed: %s (retrying in %ds)",
                    attempt + 1,
                    exc,
                    backoff,
                )
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 30)
        logger.error("MQTT: failed to publish to %s after 5 attempts", topic)
