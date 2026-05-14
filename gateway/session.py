"""
Device session management.

Each connected TCP client is represented by a :class:`Session`.
The :class:`SessionManager` keeps a registry of all active sessions and
performs periodic housekeeping (expiring dead sessions, enforcing limits).
"""

import asyncio
import logging
import time
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Dict, List, Optional

logger = logging.getLogger(__name__)


class SessionState(Enum):
    CONNECTING = auto()   # TCP connection established; not yet registered
    ACTIVE = auto()       # Registered and communicating normally
    CLOSING = auto()      # Graceful close in progress


@dataclass
class Session:
    """Represents one connected device."""

    remote_addr: str
    remote_port: int
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter

    device_id: Optional[int] = None
    state: SessionState = SessionState.CONNECTING

    connected_at: float = field(default_factory=time.monotonic)
    last_seen: float = field(default_factory=time.monotonic)
    last_seq: int = 0
    packet_count: int = 0
    byte_count: int = 0

    def touch(self) -> None:
        """Update the last-seen timestamp."""
        self.last_seen = time.monotonic()

    def is_expired(self, timeout: float) -> bool:
        """Return True if the session has been idle longer than *timeout* seconds."""
        return (time.monotonic() - self.last_seen) > timeout

    async def send(self, data: bytes) -> None:
        """Write *data* to the underlying transport."""
        if self.state == SessionState.CLOSING:
            return
        try:
            self.writer.write(data)
            await self.writer.drain()
        except (ConnectionResetError, BrokenPipeError, OSError) as exc:
            logger.warning(
                "Send error to %s:%s – %s",
                self.remote_addr,
                self.remote_port,
                exc,
            )
            self.state = SessionState.CLOSING

    async def close(self) -> None:
        """Close the underlying TCP connection."""
        self.state = SessionState.CLOSING
        try:
            self.writer.close()
            await self.writer.wait_closed()
        except OSError:
            pass

    @property
    def peer(self) -> str:
        return f"{self.remote_addr}:{self.remote_port}"

    def __repr__(self) -> str:
        dev = f"{self.device_id:#010x}" if self.device_id is not None else "unregistered"
        return f"Session(peer={self.peer}, device={dev}, state={self.state.name})"


class SessionManager:
    """
    Registry for all active device sessions.

    Thread-safety note: this class is designed for use inside a single
    asyncio event loop; it is not thread-safe.
    """

    def __init__(self, session_timeout: float = 300.0) -> None:
        self._sessions: Dict[str, Session] = {}          # peer → Session
        self._device_map: Dict[int, Session] = {}        # device_id → Session
        self._session_timeout = session_timeout

    # ------------------------------------------------------------------
    # Registration
    # ------------------------------------------------------------------

    def add(self, session: Session) -> None:
        """Register a newly connected session."""
        self._sessions[session.peer] = session
        logger.debug("Session added: %s", session)

    def register_device(self, session: Session, device_id: int) -> None:
        """Associate *session* with *device_id* after a REGISTER frame."""
        # Remove stale mapping for the same device (e.g. reconnect).
        old = self._device_map.get(device_id)
        if old is not None and old is not session:
            logger.info(
                "Device %#010x reconnected; closing old session %s",
                device_id,
                old,
            )
            asyncio.ensure_future(old.close())
            self._sessions.pop(old.peer, None)

        session.device_id = device_id
        session.state = SessionState.ACTIVE
        self._device_map[device_id] = session
        logger.info("Device %#010x registered via session %s", device_id, session.peer)

    def remove(self, session: Session) -> None:
        """Remove a session from the registry (called on disconnect)."""
        self._sessions.pop(session.peer, None)
        if session.device_id is not None:
            # Only remove if it is still this session (not a replacement).
            if self._device_map.get(session.device_id) is session:
                del self._device_map[session.device_id]
        logger.debug("Session removed: %s", session)

    # ------------------------------------------------------------------
    # Lookup
    # ------------------------------------------------------------------

    def get_by_peer(self, peer: str) -> Optional[Session]:
        return self._sessions.get(peer)

    def get_by_device(self, device_id: int) -> Optional[Session]:
        return self._device_map.get(device_id)

    # ------------------------------------------------------------------
    # Housekeeping
    # ------------------------------------------------------------------

    def expired_sessions(self) -> List[Session]:
        """Return sessions that have exceeded the idle timeout."""
        return [
            s
            for s in self._sessions.values()
            if s.is_expired(self._session_timeout)
        ]

    async def reap_expired(self) -> int:
        """Close and remove all expired sessions.  Returns the count reaped."""
        expired = self.expired_sessions()
        for session in expired:
            logger.info("Reaping expired session: %s", session)
            await session.close()
            self.remove(session)
        return len(expired)

    # ------------------------------------------------------------------
    # Statistics
    # ------------------------------------------------------------------

    @property
    def total(self) -> int:
        return len(self._sessions)

    @property
    def active(self) -> int:
        return sum(
            1 for s in self._sessions.values() if s.state == SessionState.ACTIVE
        )

    def stats(self) -> dict:
        return {
            "total_sessions": self.total,
            "active_sessions": self.active,
            "registered_devices": len(self._device_map),
        }
