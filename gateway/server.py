"""
Async TCP server for the low-power TCP gateway.

Architecture
------------
* One asyncio task per connected device handles reading.
* A shared :class:`~gateway.session.SessionManager` tracks all sessions.
* A shared :class:`~gateway.router.Router` forwards DATA/REGISTER frames to
  backends.
* A periodic housekeeping coroutine reaps expired / idle sessions.
"""

import asyncio
import logging
import signal
import time
from typing import Optional

from gateway.config import GatewayConfig
from gateway.protocol import Frame, FrameParser, MessageType, ProtocolError
from gateway.router import Router
from gateway.session import Session, SessionManager, SessionState

logger = logging.getLogger(__name__)

# How often the housekeeping task runs (seconds).
_HOUSEKEEPING_INTERVAL = 30


class GatewayServer:
    """
    Low-power TCP gateway server.

    Usage::

        config = GatewayConfig()
        router = Router()
        server = GatewayServer(config, router)
        asyncio.run(server.run())
    """

    def __init__(self, config: GatewayConfig, router: Router) -> None:
        self._config = config
        self._router = router
        self._sessions = SessionManager(
            session_timeout=config.session_timeout
        )
        self._server: Optional[asyncio.AbstractServer] = None
        self._housekeeping_task: Optional[asyncio.Task] = None
        self._started_at: Optional[float] = None

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    async def run(self) -> None:
        """Start the server and block until stopped."""
        await self._router.start()
        self._server = await asyncio.start_server(
            self._handle_client,
            host=self._config.host,
            port=self._config.port,
            limit=self._config.max_frame_size + 128,  # asyncio stream buffer limit
        )
        self._started_at = time.monotonic()

        addrs = [sock.getsockname() for sock in self._server.sockets]
        logger.info("Gateway listening on %s", addrs)

        self._housekeeping_task = asyncio.ensure_future(
            self._housekeeping_loop()
        )
        self._install_signal_handlers()

        async with self._server:
            await self._server.serve_forever()

        await self._shutdown()

    async def stop(self) -> None:
        """Gracefully stop the server."""
        if self._server:
            self._server.close()

    def stats(self) -> dict:
        """Return runtime statistics."""
        uptime = (
            time.monotonic() - self._started_at
            if self._started_at is not None
            else 0
        )
        return {
            "uptime_seconds": round(uptime, 1),
            **self._sessions.stats(),
        }

    # ------------------------------------------------------------------
    # Connection handling
    # ------------------------------------------------------------------

    async def _handle_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        addr = writer.get_extra_info("peername", ("unknown", 0))
        remote_addr, remote_port = addr[0], addr[1]

        if self._sessions.total >= self._config.max_connections:
            logger.warning(
                "Connection limit reached (%d); rejecting %s:%s",
                self._config.max_connections,
                remote_addr,
                remote_port,
            )
            writer.close()
            return

        session = Session(
            remote_addr=remote_addr,
            remote_port=remote_port,
            reader=reader,
            writer=writer,
        )
        self._sessions.add(session)
        logger.info("New connection from %s", session.peer)

        try:
            await self._read_loop(session)
        finally:
            self._sessions.remove(session)
            await session.close()
            logger.info("Connection closed: %s", session.peer)

    async def _read_loop(self, session: Session) -> None:
        """Read and process frames from *session* until the connection is closed."""
        parser = FrameParser()

        while True:
            try:
                data = await asyncio.wait_for(
                    session.reader.read(4096),
                    timeout=self._config.heartbeat_timeout,
                )
            except asyncio.TimeoutError:
                logger.info(
                    "Heartbeat timeout for %s; closing", session.peer
                )
                break
            except (ConnectionResetError, OSError) as exc:
                logger.debug("Read error on %s: %s", session.peer, exc)
                break

            if not data:
                logger.debug("EOF on %s", session.peer)
                break

            session.touch()
            session.byte_count += len(data)

            try:
                parser.feed(data)
            except ProtocolError as exc:
                logger.warning(
                    "Protocol error from %s: %s; closing connection",
                    session.peer,
                    exc,
                )
                break

            while True:
                frame = parser.pop()
                if frame is None:
                    break
                session.packet_count += 1
                await self._dispatch(session, frame)

    async def _dispatch(self, session: Session, frame: Frame) -> None:
        """Handle a single decoded *frame* from *session*."""
        logger.debug("Received %s from %s", frame, session.peer)

        if frame.msg_type == MessageType.REGISTER:
            await self._handle_register(session, frame)

        elif frame.msg_type == MessageType.HEARTBEAT:
            await self._handle_heartbeat(session, frame)

        elif frame.msg_type == MessageType.DATA:
            await self._handle_data(session, frame)

        elif frame.msg_type == MessageType.CMD_RESP:
            logger.info(
                "CMD_RESP from device %#010x seq=%d: %r",
                frame.device_id,
                frame.seq,
                frame.payload,
            )
        else:
            logger.debug("Unhandled frame type %s from %s", frame.msg_type, session.peer)

    async def _handle_register(self, session: Session, frame: Frame) -> None:
        self._sessions.register_device(session, frame.device_id)
        ack = Frame.ack(frame.device_id, frame.seq)
        await session.send(ack.encode())
        await self._router.route(
            frame,
            metadata={"peer": session.peer},
        )

    async def _handle_heartbeat(self, session: Session, frame: Frame) -> None:
        # If not yet registered, use the device_id from the heartbeat frame.
        if session.device_id is None:
            session.device_id = frame.device_id
        ack = Frame.ack(frame.device_id, frame.seq)
        await session.send(ack.encode())

    async def _handle_data(self, session: Session, frame: Frame) -> None:
        if session.state != SessionState.ACTIVE:
            logger.warning(
                "DATA from unregistered device at %s; ignoring", session.peer
            )
            return
        ack = Frame.ack(frame.device_id, frame.seq)
        await session.send(ack.encode())
        await self._router.route(
            frame,
            metadata={"peer": session.peer},
        )

    # ------------------------------------------------------------------
    # Housekeeping
    # ------------------------------------------------------------------

    async def _housekeeping_loop(self) -> None:
        while True:
            await asyncio.sleep(_HOUSEKEEPING_INTERVAL)
            try:
                reaped = await self._sessions.reap_expired()
                if reaped:
                    logger.info("Housekeeping: reaped %d expired session(s)", reaped)
                logger.debug("Stats: %s", self.stats())
            except Exception as exc:
                logger.error("Housekeeping error: %s", exc, exc_info=True)

    # ------------------------------------------------------------------
    # Shutdown
    # ------------------------------------------------------------------

    def _install_signal_handlers(self) -> None:
        loop = asyncio.get_event_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, self._on_signal)
            except (NotImplementedError, RuntimeError):
                # Not available on Windows or in non-main thread.
                pass

    def _on_signal(self) -> None:
        logger.info("Shutdown signal received")
        asyncio.ensure_future(self.stop())

    async def _shutdown(self) -> None:
        if self._housekeeping_task:
            self._housekeeping_task.cancel()
            try:
                await self._housekeeping_task
            except asyncio.CancelledError:
                pass

        # Close all active sessions.
        for session in list(self._sessions._sessions.values()):
            await session.close()

        await self._router.stop()
        logger.info("Gateway shutdown complete")
