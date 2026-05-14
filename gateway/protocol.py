"""
Binary protocol parser/encoder for low-power TCP gateway.

Frame format:
  +--------+--------+---------+---------+---------+---------+
  | Magic  |  Type  | Length  | Dev ID  |   Seq   | Payload |
  | 1 byte | 1 byte | 2 bytes | 4 bytes | 2 bytes | N bytes |
  +--------+--------+---------+---------+---------+---------+

  Total header size: 10 bytes
  Magic: 0xAB
  Max payload: 65535 bytes
"""

import struct
import enum
from dataclasses import dataclass, field
from typing import Optional, Tuple

MAGIC = 0xAB
HEADER_SIZE = 10  # bytes
MAX_PAYLOAD_SIZE = 65535


class MessageType(enum.IntEnum):
    REGISTER = 0x01   # Device registration
    HEARTBEAT = 0x02  # Keep-alive heartbeat
    DATA = 0x03       # Sensor / telemetry data
    ACK = 0x04        # Acknowledgement
    CMD = 0x05        # Command from gateway → device
    CMD_RESP = 0x06   # Command response from device


class ProtocolError(Exception):
    """Raised when a received frame is malformed."""


@dataclass
class Frame:
    """A decoded protocol frame."""

    msg_type: MessageType
    device_id: int
    seq: int
    payload: bytes = field(default=b"")

    # ------------------------------------------------------------------
    # Encoding
    # ------------------------------------------------------------------

    def encode(self) -> bytes:
        """Encode the frame into bytes ready to be sent over the network."""
        if len(self.payload) > MAX_PAYLOAD_SIZE:
            raise ValueError(
                f"Payload too large: {len(self.payload)} > {MAX_PAYLOAD_SIZE}"
            )
        header = struct.pack(
            ">BBHIH",
            MAGIC,
            int(self.msg_type),
            len(self.payload),
            self.device_id,
            self.seq,
        )
        return header + self.payload

    # ------------------------------------------------------------------
    # Convenience constructors
    # ------------------------------------------------------------------

    @classmethod
    def heartbeat(cls, device_id: int, seq: int) -> "Frame":
        return cls(MessageType.HEARTBEAT, device_id, seq)

    @classmethod
    def ack(cls, device_id: int, seq: int) -> "Frame":
        return cls(MessageType.ACK, device_id, seq)

    @classmethod
    def data(cls, device_id: int, seq: int, payload: bytes) -> "Frame":
        return cls(MessageType.DATA, device_id, seq, payload)

    @classmethod
    def register(cls, device_id: int, seq: int, payload: bytes = b"") -> "Frame":
        return cls(MessageType.REGISTER, device_id, seq, payload)

    @classmethod
    def cmd(cls, device_id: int, seq: int, payload: bytes) -> "Frame":
        return cls(MessageType.CMD, device_id, seq, payload)

    def __repr__(self) -> str:
        return (
            f"Frame(type={self.msg_type.name}, device_id={self.device_id:#010x}, "
            f"seq={self.seq}, payload_len={len(self.payload)})"
        )


class FrameParser:
    """
    Stateful, streaming frame parser.

    Feed raw bytes via :meth:`feed`; call :meth:`pop` to retrieve
    complete frames one at a time.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._frames: list = []

    def feed(self, data: bytes) -> None:
        """Append *data* to the internal buffer and parse any complete frames."""
        self._buf.extend(data)
        self._parse()

    def pop(self) -> Optional[Frame]:
        """Return the oldest complete frame, or *None* if none are ready."""
        return self._frames.pop(0) if self._frames else None

    def __len__(self) -> int:
        """Number of fully parsed frames waiting to be consumed."""
        return len(self._frames)

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _parse(self) -> None:
        while True:
            # Need at least a full header.
            if len(self._buf) < HEADER_SIZE:
                return

            # Validate magic byte.
            if self._buf[0] != MAGIC:
                raise ProtocolError(
                    f"Invalid magic byte: {self._buf[0]:#04x}"
                )

            magic, msg_type_raw, length, device_id, seq = struct.unpack_from(
                ">BBHIH", self._buf, 0
            )

            total = HEADER_SIZE + length
            if len(self._buf) < total:
                # Wait for more data.
                return

            payload = bytes(self._buf[HEADER_SIZE:total])
            del self._buf[:total]

            try:
                msg_type = MessageType(msg_type_raw)
            except ValueError:
                raise ProtocolError(
                    f"Unknown message type: {msg_type_raw:#04x}"
                )

            self._frames.append(
                Frame(msg_type=msg_type, device_id=device_id, seq=seq, payload=payload)
            )


def decode_frame(data: bytes) -> Tuple[Frame, int]:
    """
    Decode a single frame from *data*.

    Returns ``(frame, bytes_consumed)``.  Raises :exc:`ProtocolError` when
    the data does not contain a valid frame.
    """
    if len(data) < HEADER_SIZE:
        raise ProtocolError(
            f"Buffer too short for header: {len(data)} < {HEADER_SIZE}"
        )

    magic, msg_type_raw, length, device_id, seq = struct.unpack_from(
        ">BBHIH", data, 0
    )

    if magic != MAGIC:
        raise ProtocolError(f"Invalid magic byte: {magic:#04x}")

    total = HEADER_SIZE + length
    if len(data) < total:
        raise ProtocolError(
            f"Buffer too short for payload: {len(data)} < {total}"
        )

    try:
        msg_type = MessageType(msg_type_raw)
    except ValueError:
        raise ProtocolError(f"Unknown message type: {msg_type_raw:#04x}")

    payload = data[HEADER_SIZE:total]
    return Frame(msg_type=msg_type, device_id=device_id, seq=seq, payload=payload), total
