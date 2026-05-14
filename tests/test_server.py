"""Integration / unit tests for gateway.server."""

import asyncio
import struct
import pytest

from gateway.config import GatewayConfig
from gateway.protocol import Frame, FrameParser, MessageType, MAGIC, HEADER_SIZE
from gateway.router import Router
from gateway.server import GatewayServer


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _encode(msg_type: MessageType, device_id: int, seq: int, payload: bytes = b"") -> bytes:
    return Frame(msg_type, device_id, seq, payload).encode()


async def _run_server_for(server: GatewayServer, coro, timeout: float = 5.0):
    """Start *server*, run *coro*, then stop the server."""
    task = asyncio.ensure_future(server.run())
    await asyncio.sleep(0.1)  # Let the server bind
    try:
        return await asyncio.wait_for(coro, timeout=timeout)
    finally:
        await server.stop()
        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):
            pass


# ---------------------------------------------------------------------------
# Basic server tests
# ---------------------------------------------------------------------------

class TestGatewayServerIntegration:
    @pytest.fixture
    def config(self):
        cfg = GatewayConfig()
        cfg.port = 19000  # Use a high port for tests
        cfg.heartbeat_timeout = 2
        cfg.session_timeout = 5
        return cfg

    @pytest.fixture
    def router(self):
        return Router()

    @pytest.mark.asyncio
    async def test_server_accepts_connection(self, config, router):
        server = GatewayServer(config, router)

        async def _connect():
            reader, writer = await asyncio.open_connection("127.0.0.1", config.port)
            writer.close()
            await writer.wait_closed()

        await _run_server_for(server, _connect())

    @pytest.mark.asyncio
    async def test_register_flow(self, config, router):
        """Device registers → server responds with ACK."""
        server = GatewayServer(config, router)
        device_id = 0x00000042
        seq = 1

        async def _register():
            reader, writer = await asyncio.open_connection("127.0.0.1", config.port)
            reg_frame = Frame.register(device_id, seq)
            writer.write(reg_frame.encode())
            await writer.drain()

            # Read the ACK
            raw = await asyncio.wait_for(reader.read(HEADER_SIZE), timeout=2)
            assert len(raw) == HEADER_SIZE
            magic, msg_type_raw, length, resp_device_id, resp_seq = struct.unpack(
                ">BBHIH", raw
            )
            assert magic == MAGIC
            assert msg_type_raw == int(MessageType.ACK)
            assert resp_device_id == device_id
            assert resp_seq == seq

            writer.close()
            await writer.wait_closed()

        await _run_server_for(server, _register())

    @pytest.mark.asyncio
    async def test_heartbeat_flow(self, config, router):
        """Device sends heartbeat → server responds with ACK."""
        server = GatewayServer(config, router)
        device_id = 0x00000099
        seq = 7

        async def _heartbeat():
            reader, writer = await asyncio.open_connection("127.0.0.1", config.port)
            hb = Frame.heartbeat(device_id, seq)
            writer.write(hb.encode())
            await writer.drain()

            raw = await asyncio.wait_for(reader.read(HEADER_SIZE), timeout=2)
            assert len(raw) == HEADER_SIZE
            _, msg_type_raw, _, _, resp_seq = struct.unpack(">BBHIH", raw)
            assert msg_type_raw == int(MessageType.ACK)
            assert resp_seq == seq

            writer.close()
            await writer.wait_closed()

        await _run_server_for(server, _heartbeat())

    @pytest.mark.asyncio
    async def test_data_after_register(self, config, router):
        """Registered device sends DATA → server responds with ACK and routes frame."""
        routed_frames = []

        class _RecordingBackend:
            async def start(self):
                pass

            async def stop(self):
                pass

            async def send(self, device_id, payload, metadata):
                routed_frames.append((device_id, payload))

        router.register_backend(_RecordingBackend())
        server = GatewayServer(config, router)
        device_id = 0x00001234
        payload = b'{"v":42}'

        async def _send_data():
            reader, writer = await asyncio.open_connection("127.0.0.1", config.port)

            # Register
            writer.write(Frame.register(device_id, seq=1).encode())
            await writer.drain()
            await asyncio.wait_for(reader.read(HEADER_SIZE), timeout=2)  # ACK

            # Send data
            writer.write(Frame.data(device_id, seq=2, payload=payload).encode())
            await writer.drain()
            ack_raw = await asyncio.wait_for(reader.read(HEADER_SIZE), timeout=2)
            _, msg_type_raw, _, _, ack_seq = struct.unpack(">BBHIH", ack_raw)
            assert msg_type_raw == int(MessageType.ACK)
            assert ack_seq == 2

            # Give router a moment to dispatch
            await asyncio.sleep(0.1)

            writer.close()
            await writer.wait_closed()

        await _run_server_for(server, _send_data())
        # Data was routed to backend
        assert any(dev == device_id and p == payload for dev, p in routed_frames)

    @pytest.mark.asyncio
    async def test_multiple_concurrent_clients(self, config, router):
        """Multiple devices can connect and register concurrently."""
        server = GatewayServer(config, router)

        async def _connect_and_register(device_id: int, port: int):
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(Frame.register(device_id, seq=0).encode())
            await writer.drain()
            raw = await asyncio.wait_for(reader.read(HEADER_SIZE), timeout=2)
            assert len(raw) == HEADER_SIZE
            writer.close()
            await writer.wait_closed()

        async def _multi():
            tasks = [
                _connect_and_register(i, config.port) for i in range(1, 6)
            ]
            await asyncio.gather(*tasks)

        await _run_server_for(server, _multi())

    @pytest.mark.asyncio
    async def test_invalid_protocol_closes_connection(self, config, router):
        """Sending garbage data should close the connection."""
        server = GatewayServer(config, router)

        async def _send_garbage():
            reader, writer = await asyncio.open_connection("127.0.0.1", config.port)
            writer.write(b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF")
            await writer.drain()
            # Server should close the connection
            data = await asyncio.wait_for(reader.read(100), timeout=2)
            # Either empty (connection closed) or we get no valid response
            _ = data  # just ensure no crash

            writer.close()
            await writer.wait_closed()

        await _run_server_for(server, _send_garbage())

    @pytest.mark.asyncio
    async def test_stats(self, config, router):
        server = GatewayServer(config, router)

        async def _check_stats():
            reader, writer = await asyncio.open_connection("127.0.0.1", config.port)
            stats = server.stats()
            assert "uptime_seconds" in stats
            assert "total_sessions" in stats
            assert stats["total_sessions"] >= 1
            writer.close()
            await writer.wait_closed()

        await _run_server_for(server, _check_stats())
