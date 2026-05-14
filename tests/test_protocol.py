"""Unit tests for gateway.protocol."""

import struct
import pytest

from gateway.protocol import (
    MAGIC,
    HEADER_SIZE,
    Frame,
    FrameParser,
    MessageType,
    ProtocolError,
    decode_frame,
)


# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------

def _make_raw(
    msg_type: MessageType,
    device_id: int,
    seq: int,
    payload: bytes = b"",
) -> bytes:
    header = struct.pack(
        ">BBHIH",
        MAGIC,
        int(msg_type),
        len(payload),
        device_id,
        seq,
    )
    return header + payload


# ---------------------------------------------------------------------------
# Frame.encode
# ---------------------------------------------------------------------------

class TestFrameEncode:
    def test_heartbeat_round_trip(self):
        frame = Frame.heartbeat(device_id=0x0001, seq=1)
        raw = frame.encode()
        decoded, consumed = decode_frame(raw)
        assert consumed == HEADER_SIZE
        assert decoded.msg_type == MessageType.HEARTBEAT
        assert decoded.device_id == 0x0001
        assert decoded.seq == 1
        assert decoded.payload == b""

    def test_data_round_trip(self):
        payload = b'{"temp": 22.5}'
        frame = Frame.data(device_id=0xDEADBEEF, seq=42, payload=payload)
        raw = frame.encode()
        decoded, consumed = decode_frame(raw)
        assert consumed == HEADER_SIZE + len(payload)
        assert decoded.msg_type == MessageType.DATA
        assert decoded.device_id == 0xDEADBEEF
        assert decoded.seq == 42
        assert decoded.payload == payload

    def test_ack_no_payload(self):
        frame = Frame.ack(device_id=1, seq=0)
        raw = frame.encode()
        assert len(raw) == HEADER_SIZE

    def test_payload_too_large_raises(self):
        with pytest.raises(ValueError):
            Frame(MessageType.DATA, 1, 0, b"x" * 65536).encode()

    def test_magic_byte(self):
        raw = Frame.heartbeat(1, 0).encode()
        assert raw[0] == MAGIC


# ---------------------------------------------------------------------------
# decode_frame
# ---------------------------------------------------------------------------

class TestDecodeFrame:
    def test_decodes_valid_frame(self):
        raw = _make_raw(MessageType.DATA, 0xABCD, 7, b"hello")
        frame, consumed = decode_frame(raw)
        assert frame.msg_type == MessageType.DATA
        assert frame.device_id == 0xABCD
        assert frame.seq == 7
        assert frame.payload == b"hello"
        assert consumed == HEADER_SIZE + 5

    def test_rejects_wrong_magic(self):
        raw = bytearray(_make_raw(MessageType.HEARTBEAT, 1, 0))
        raw[0] = 0xFF
        with pytest.raises(ProtocolError, match="magic"):
            decode_frame(bytes(raw))

    def test_rejects_unknown_type(self):
        raw = bytearray(_make_raw(MessageType.HEARTBEAT, 1, 0))
        raw[1] = 0xFF  # Invalid type
        with pytest.raises(ProtocolError, match="Unknown message type"):
            decode_frame(bytes(raw))

    def test_rejects_buffer_too_short_for_header(self):
        with pytest.raises(ProtocolError, match="too short for header"):
            decode_frame(b"\xAB")

    def test_rejects_buffer_too_short_for_payload(self):
        raw = _make_raw(MessageType.DATA, 1, 0, b"hello")
        with pytest.raises(ProtocolError, match="too short for payload"):
            decode_frame(raw[:HEADER_SIZE + 2])  # Only partial payload


# ---------------------------------------------------------------------------
# FrameParser (streaming)
# ---------------------------------------------------------------------------

class TestFrameParser:
    def test_single_frame(self):
        parser = FrameParser()
        raw = _make_raw(MessageType.HEARTBEAT, 1, 0)
        parser.feed(raw)
        frame = parser.pop()
        assert frame is not None
        assert frame.msg_type == MessageType.HEARTBEAT
        assert parser.pop() is None

    def test_multiple_frames_in_one_chunk(self):
        parser = FrameParser()
        raw = (
            _make_raw(MessageType.HEARTBEAT, 1, 0)
            + _make_raw(MessageType.DATA, 1, 1, b"abc")
            + _make_raw(MessageType.ACK, 1, 2)
        )
        parser.feed(raw)
        assert len(parser) == 3
        types = [parser.pop().msg_type for _ in range(3)]
        assert types == [MessageType.HEARTBEAT, MessageType.DATA, MessageType.ACK]

    def test_fragmented_frame(self):
        """Feed a frame in two chunks; should still parse correctly."""
        parser = FrameParser()
        raw = _make_raw(MessageType.DATA, 0x42, 5, b"sensor_data")
        mid = len(raw) // 2
        parser.feed(raw[:mid])
        assert len(parser) == 0  # Not yet complete
        parser.feed(raw[mid:])
        frame = parser.pop()
        assert frame is not None
        assert frame.payload == b"sensor_data"

    def test_empty_feed(self):
        parser = FrameParser()
        parser.feed(b"")
        assert parser.pop() is None

    def test_invalid_magic_raises(self):
        parser = FrameParser()
        bad = bytearray(_make_raw(MessageType.HEARTBEAT, 1, 0))
        bad[0] = 0x00
        with pytest.raises(ProtocolError):
            parser.feed(bytes(bad))

    def test_large_payload(self):
        payload = bytes(range(256)) * 100  # 25600 bytes
        parser = FrameParser()
        raw = _make_raw(MessageType.DATA, 99, 0, payload)
        parser.feed(raw)
        frame = parser.pop()
        assert frame is not None
        assert frame.payload == payload

    def test_len_counter(self):
        parser = FrameParser()
        raw = (
            _make_raw(MessageType.HEARTBEAT, 1, 0)
            + _make_raw(MessageType.HEARTBEAT, 1, 1)
        )
        parser.feed(raw)
        assert len(parser) == 2
        parser.pop()
        assert len(parser) == 1


# ---------------------------------------------------------------------------
# MessageType
# ---------------------------------------------------------------------------

class TestMessageType:
    def test_all_types_have_unique_values(self):
        values = [t.value for t in MessageType]
        assert len(values) == len(set(values))

    def test_register_is_first(self):
        assert MessageType.REGISTER == 0x01

    def test_all_types_encodable(self):
        for msg_type in MessageType:
            frame = Frame(msg_type, device_id=1, seq=0)
            raw = frame.encode()
            assert raw[1] == int(msg_type)
