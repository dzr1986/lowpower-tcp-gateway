"""Unit tests for gateway.session."""

import asyncio
import time
import pytest
from unittest.mock import AsyncMock, MagicMock

from gateway.session import Session, SessionManager, SessionState


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_session(addr="127.0.0.1", port=12345) -> Session:
    reader = MagicMock(spec=asyncio.StreamReader)
    writer = MagicMock(spec=asyncio.StreamWriter)
    writer.close = MagicMock()
    writer.wait_closed = AsyncMock()
    writer.write = MagicMock()
    writer.drain = AsyncMock()
    return Session(remote_addr=addr, remote_port=port, reader=reader, writer=writer)


# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------

class TestSession:
    def test_initial_state_is_connecting(self):
        s = _make_session()
        assert s.state == SessionState.CONNECTING

    def test_peer_property(self):
        s = _make_session("10.0.0.1", 5555)
        assert s.peer == "10.0.0.1:5555"

    def test_touch_updates_last_seen(self):
        s = _make_session()
        old = s.last_seen
        time.sleep(0.01)
        s.touch()
        assert s.last_seen > old

    def test_is_not_expired_fresh(self):
        s = _make_session()
        assert not s.is_expired(timeout=60)

    def test_is_expired_stale(self):
        s = _make_session()
        s.last_seen = time.monotonic() - 100
        assert s.is_expired(timeout=60)

    @pytest.mark.asyncio
    async def test_send_writes_data(self):
        s = _make_session()
        s.state = SessionState.ACTIVE
        await s.send(b"hello")
        s.writer.write.assert_called_once_with(b"hello")
        s.writer.drain.assert_awaited_once()

    @pytest.mark.asyncio
    async def test_send_drops_when_closing(self):
        s = _make_session()
        s.state = SessionState.CLOSING
        await s.send(b"hello")
        s.writer.write.assert_not_called()

    @pytest.mark.asyncio
    async def test_close_sets_closing_state(self):
        s = _make_session()
        await s.close()
        assert s.state == SessionState.CLOSING

    def test_repr_unregistered(self):
        s = _make_session()
        assert "unregistered" in repr(s)

    def test_repr_registered(self):
        s = _make_session()
        s.device_id = 0xDEADBEEF
        assert "0xdeadbeef" in repr(s)


# ---------------------------------------------------------------------------
# SessionManager
# ---------------------------------------------------------------------------

class TestSessionManager:
    def test_add_and_total(self):
        mgr = SessionManager()
        s = _make_session()
        mgr.add(s)
        assert mgr.total == 1

    def test_remove_decrements_total(self):
        mgr = SessionManager()
        s = _make_session()
        mgr.add(s)
        mgr.remove(s)
        assert mgr.total == 0

    def test_register_device(self):
        mgr = SessionManager()
        s = _make_session()
        mgr.add(s)
        mgr.register_device(s, device_id=0x1234)
        assert s.device_id == 0x1234
        assert s.state == SessionState.ACTIVE
        assert mgr.get_by_device(0x1234) is s

    def test_get_by_peer(self):
        mgr = SessionManager()
        s = _make_session("192.168.1.1", 8080)
        mgr.add(s)
        assert mgr.get_by_peer("192.168.1.1:8080") is s

    def test_get_by_peer_missing_returns_none(self):
        mgr = SessionManager()
        assert mgr.get_by_peer("1.2.3.4:9999") is None

    def test_get_by_device_missing_returns_none(self):
        mgr = SessionManager()
        assert mgr.get_by_device(0xDEAD) is None

    def test_active_count(self):
        mgr = SessionManager()
        s1 = _make_session("1.1.1.1", 1)
        s2 = _make_session("2.2.2.2", 2)
        mgr.add(s1)
        mgr.add(s2)
        mgr.register_device(s1, 0xAAA)
        assert mgr.active == 1

    def test_stats_keys(self):
        mgr = SessionManager()
        keys = mgr.stats().keys()
        assert "total_sessions" in keys
        assert "active_sessions" in keys
        assert "registered_devices" in keys

    @pytest.mark.asyncio
    async def test_reap_expired(self):
        mgr = SessionManager(session_timeout=1)
        s = _make_session()
        mgr.add(s)
        s.last_seen = time.monotonic() - 10  # Force expiry
        reaped = await mgr.reap_expired()
        assert reaped == 1
        assert mgr.total == 0

    @pytest.mark.asyncio
    async def test_reap_does_not_touch_fresh(self):
        mgr = SessionManager(session_timeout=300)
        s = _make_session()
        mgr.add(s)
        reaped = await mgr.reap_expired()
        assert reaped == 0
        assert mgr.total == 1

    def test_remove_unregistered_session(self):
        mgr = SessionManager()
        s = _make_session()
        mgr.add(s)
        mgr.register_device(s, 0xBEEF)
        mgr.remove(s)
        assert mgr.get_by_device(0xBEEF) is None
        assert mgr.total == 0
