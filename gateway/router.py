"""
Message router – dispatches frames to registered backends.

Backends implement a simple async interface::

    async def start() -> None
    async def stop() -> None
    async def send(device_id: int, payload: bytes, metadata: dict) -> None
"""

import asyncio
import logging
from typing import List

from gateway.protocol import Frame, MessageType

logger = logging.getLogger(__name__)


class Router:
    """Routes incoming device frames to all registered backends."""

    def __init__(self) -> None:
        self._backends: List = []

    def register_backend(self, backend) -> None:
        self._backends.append(backend)
        logger.info("Backend registered: %s", type(backend).__name__)

    async def start(self) -> None:
        for backend in self._backends:
            try:
                await backend.start()
            except Exception as exc:
                logger.error(
                    "Failed to start backend %s: %s", type(backend).__name__, exc
                )

    async def stop(self) -> None:
        for backend in self._backends:
            try:
                await backend.stop()
            except Exception as exc:
                logger.error(
                    "Error stopping backend %s: %s", type(backend).__name__, exc
                )

    async def route(self, frame: Frame, metadata: dict = None) -> None:
        """Forward *frame* to all active backends."""
        if frame.msg_type not in (MessageType.DATA, MessageType.REGISTER):
            return

        meta = metadata or {}
        tasks = []
        for backend in self._backends:
            tasks.append(
                asyncio.ensure_future(
                    self._send_to_backend(backend, frame, meta)
                )
            )
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)

    @staticmethod
    async def _send_to_backend(backend, frame: Frame, metadata: dict) -> None:
        try:
            await backend.send(
                device_id=frame.device_id,
                payload=frame.payload,
                metadata={
                    "msg_type": frame.msg_type.name,
                    "seq": frame.seq,
                    **metadata,
                },
            )
        except Exception as exc:
            logger.error(
                "Backend %s error: %s", type(backend).__name__, exc, exc_info=True
            )
