"""Secret-safe structural tracing for SRT handshake datagrams."""

from __future__ import annotations

import errno
import hashlib
import ipaddress
import json
import select
import socket
import threading
import time
from dataclasses import dataclass


PACKET_HEADER_SIZE = 16
HANDSHAKE_SIZE = 48
HANDSHAKE_DATAGRAM_SIZE = PACKET_HEADER_SIZE + HANDSHAKE_SIZE
HANDSHAKE_CONTROL_TYPE = 0
ACKNOWLEDGEMENT_CONTROL_TYPE = 2
LOSS_REPORT_CONTROL_TYPE = 3
SHUTDOWN_CONTROL_TYPE = 5
DROP_REQUEST_CONTROL_TYPE = 7
PEER_ERROR_CONTROL_TYPE = 8
USER_DEFINED_CONTROL_TYPE = 0x7FFF
KEY_MATERIAL_HEADER_SIZE = 16
RUNTIME_KEY_MATERIAL_NAMES = {
    3: "KMREQ",
    4: "KMRSP",
}
LEGACY_HANDSHAKE_NAMES = {
    1: "HSREQ",
    2: "HSRSP",
}

EXTENSION_NAMES = {
    1: "HSREQ",
    2: "HSRSP",
    3: "KMREQ",
    4: "KMRSP",
    5: "SID",
    6: "CONGESTION",
    7: "FILTER",
    8: "GROUP",
}
REQUEST_NAMES = {
    -3: "DONE",
    -2: "AGREEMENT",
    -1: "CONCLUSION",
    0: "WAVEAHAND",
    1: "INDUCTION",
}
RENDEZVOUS_COOKIE_HALF_RANGE = 0x8000_0000
RENDEZVOUS_DIRECTIONS = {
    "sender_to_receiver",
    "receiver_to_sender",
    "either",
}
TRACE_ENDPOINT_DIRECTIONS = {
    "sender_to_receiver",
    "receiver_to_sender",
    "caller_to_listener",
    "listener_to_caller",
}
FAULT_ACTIONS = {
    "drop",
    "duplicate",
    "late_duplicate",
    "delay",
    "reorder",
}
FAULT_PACKET_KINDS = {"data", "control", "handshake"}
MAX_DELAYED_DATAGRAMS = 4_096
MAX_RUNTIME_KEY_MATERIAL_EVENTS = 128
MAX_ARQ_TRACE_EVENTS = 256
# Long rollover profiles cross the 31-bit boundary only after more than
# 16,384 authentic DATA packets. Keep their uniqueness evidence bounded while
# retaining enough identities to reject mutated ciphertext retransmissions.
MAX_DATA_KEY_IDENTITIES = 32_768
MAX_ORIGINAL_DATA_IDENTITIES = 32_768
MAX_SHUTDOWN_EVENTS = 8
SEQUENCE_MASK = 0x7FFF_FFFF
FORWARD_SEND_RETRY_SECONDS = 0.5
FORWARD_SEND_POLL_SECONDS = 0.05
PEER_ERROR_ACK_HOLD_MILLISECONDS = 25


@dataclass(frozen=True)
class RendezvousFault:
    action: str
    direction: str
    occurrence: int
    packet_kind: str = "data"
    delay_milliseconds: int = 0
    handshake_request: int | None = None
    control_type: int | None = None
    suppress_retransmissions: bool = False

    def __post_init__(self) -> None:
        if self.action not in FAULT_ACTIONS:
            raise ValueError(f"unsupported fault action: {self.action}")
        if self.direction not in RENDEZVOUS_DIRECTIONS:
            raise ValueError(
                f"unsupported fault direction: {self.direction}"
            )
        if self.packet_kind not in FAULT_PACKET_KINDS:
            raise ValueError(
                f"unsupported fault packet kind: {self.packet_kind}"
            )
        if self.occurrence <= 0:
            raise ValueError("fault occurrence must be positive")
        if self.delay_milliseconds < 0:
            raise ValueError("fault delay must not be negative")
        delayed_actions = {"delay", "late_duplicate"}
        if (
            self.action in delayed_actions
            and self.delay_milliseconds == 0
        ):
            raise ValueError(
                f"{self.action} faults require a positive delay"
            )
        if (
            self.action not in delayed_actions
            and self.delay_milliseconds != 0
        ):
            raise ValueError(
                "only delayed faults may specify a delay"
            )
        if (
            self.handshake_request is not None
            and self.packet_kind != "handshake"
        ):
            raise ValueError(
                "handshake request filtering requires handshake packets"
            )
        if (
            self.control_type is not None
            and (
                self.packet_kind != "control"
                or not 0 <= self.control_type <= 0x7FFF
            )
        ):
            raise ValueError(
                "control type filtering requires control packets "
                "and a 15-bit value"
            )
        if self.suppress_retransmissions and (
            self.action != "drop" or self.packet_kind != "data"
        ):
            raise ValueError(
                "retransmission suppression requires a DATA drop fault"
            )


def resolve_rendezvous_role(
    local_cookie: int, peer_cookie: int
) -> str:
    """Apply the wrapping cookie contest used by deployed SRT peers."""

    difference = (local_cookie - peer_cookie) & 0xFFFF_FFFF
    if difference == 0:
        return "unresolved"
    if difference < RENDEZVOUS_COOKIE_HALF_RANGE:
        return "initiator"
    if difference > RENDEZVOUS_COOKIE_HALF_RANGE:
        return "responder"
    local_signed = (
        local_cookie
        if local_cookie < RENDEZVOUS_COOKIE_HALF_RANGE
        else local_cookie - 0x1_0000_0000
    )
    peer_signed = (
        peer_cookie
        if peer_cookie < RENDEZVOUS_COOKIE_HALF_RANGE
        else peer_cookie - 0x1_0000_0000
    )
    return "initiator" if local_signed > peer_signed else "responder"


def rendezvous_role_observation(
    entries: list[dict[str, object]],
) -> dict[str, object] | None:
    cookies: dict[str, int] = {}
    for entry in entries:
        if entry.get("request") != 0:
            continue
        direction = entry.get("direction")
        cookie = entry.get("syn_cookie")
        if (
            isinstance(direction, str)
            and isinstance(cookie, str)
            and direction not in cookies
        ):
            cookies[direction] = int(cookie, 16)
    sender_cookie = cookies.get("sender_to_receiver")
    receiver_cookie = cookies.get("receiver_to_sender")
    if sender_cookie is None or receiver_cookie is None:
        return None
    sender_role = resolve_rendezvous_role(
        sender_cookie, receiver_cookie
    )
    receiver_role = resolve_rendezvous_role(
        receiver_cookie, sender_cookie
    )
    if sender_role == "unresolved" or receiver_role == "unresolved":
        return None
    return {
        "sender_cookie": sender_cookie,
        "receiver_cookie": receiver_cookie,
        "sender_role": sender_role,
        "receiver_role": receiver_role,
    }


def _u16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def _u32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def _i32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big", signed=True)


def _with_u32(data: bytes, offset: int, value: int) -> bytes:
    rewritten = bytearray(data)
    rewritten[offset : offset + 4] = value.to_bytes(4, "big")
    return bytes(rewritten)


def build_peer_error_datagram(
    destination_socket_id: int,
    timestamp: int,
    error_code: int = 4_000,
) -> bytes:
    """Encode the deployed type-8 packet, including its four-byte pad."""

    values = (destination_socket_id, timestamp, error_code)
    if not all(0 <= value <= 0xFFFF_FFFF for value in values):
        raise ValueError("PEERERROR fields must be unsigned 32-bit values")
    control_word = 0x8000_0000 | (PEER_ERROR_CONTROL_TYPE << 16)
    return b"".join(
        value.to_bytes(4, "big")
        for value in (
            control_word,
            error_code,
            timestamp,
            destination_socket_id,
            0,
        )
    )


def _translated_sequence_word(word: int, delta: int) -> int:
    marker = word & 0x8000_0000
    sequence = ((word & SEQUENCE_MASK) + delta) & SEQUENCE_MASK
    return marker | sequence


def translate_sequence_datagram(payload: bytes, delta: int) -> bytes:
    """Translate every 31-bit data-sequence field in one SRT datagram."""

    if len(payload) < PACKET_HEADER_SIZE:
        return payload
    first_word = _u32(payload, 0)
    if (first_word & 0x8000_0000) == 0:
        return _with_u32(
            payload,
            0,
            _translated_sequence_word(first_word, delta),
        )

    control_type = (first_word >> 16) & 0x7FFF
    rewritten = payload
    if (
        control_type == HANDSHAKE_CONTROL_TYPE
        and len(payload) >= HANDSHAKE_DATAGRAM_SIZE
    ):
        return _with_u32(
            payload,
            24,
            _translated_sequence_word(_u32(payload, 24), delta),
        )
    if (
        control_type == ACKNOWLEDGEMENT_CONTROL_TYPE
        and len(payload) >= PACKET_HEADER_SIZE + 4
    ):
        return _with_u32(
            payload,
            PACKET_HEADER_SIZE,
            _translated_sequence_word(
                _u32(payload, PACKET_HEADER_SIZE), delta
            ),
        )
    if control_type == LOSS_REPORT_CONTROL_TYPE:
        offset = PACKET_HEADER_SIZE
        while offset + 4 <= len(rewritten):
            rewritten = _with_u32(
                rewritten,
                offset,
                _translated_sequence_word(
                    _u32(rewritten, offset), delta
                ),
            )
            offset += 4
        return rewritten
    if (
        control_type == DROP_REQUEST_CONTROL_TYPE
        and len(payload) >= PACKET_HEADER_SIZE + 8
    ):
        rewritten = _with_u32(
            rewritten,
            PACKET_HEADER_SIZE,
            _translated_sequence_word(
                _u32(rewritten, PACKET_HEADER_SIZE), delta
            ),
        )
        return _with_u32(
            rewritten,
            PACKET_HEADER_SIZE + 4,
            _translated_sequence_word(
                _u32(rewritten, PACKET_HEADER_SIZE + 4), delta
            ),
        )
    return payload


def _describe_key_material(content: bytes) -> dict[str, object]:
    result: dict[str, object] = {
        "content_bytes": len(content),
        "content_sha256": hashlib.sha256(content).hexdigest(),
    }
    if len(content) < KEY_MATERIAL_HEADER_SIZE:
        result["malformed"] = "key material header is truncated"
        return result

    key_selection = content[3] & 0x03
    salt_words = content[14]
    key_words = content[15]
    key_count = 2 if key_selection == 3 else 1
    expected_bytes = (
        KEY_MATERIAL_HEADER_SIZE
        + salt_words * 4
        + key_count * key_words * 4
        + 8
    )
    result.update(
        {
            "packet_type": content[0] >> 4,
            "version": content[0] & 0x0F,
            "signature": f"0x{_u16(content, 1):04x}",
            "key_selection": key_selection,
            "key_encrypting_key_index": _u32(content, 4),
            "cipher": content[8],
            "authentication": content[9],
            "stream_encapsulation": content[10],
            "reserved_byte": content[11],
            "reserved_word": _u16(content, 12),
            "salt_words": salt_words,
            "key_words": key_words,
            "expected_content_bytes": expected_bytes,
            "content_length_matches": len(content) == expected_bytes,
        }
    )
    return result


def _describe_extension(
    extension_type: int,
    length_words: int,
    content: bytes,
) -> dict[str, object]:
    result: dict[str, object] = {
        "type": extension_type,
        "name": EXTENSION_NAMES.get(extension_type, "UNKNOWN"),
        "length_words": length_words,
    }
    if extension_type in (1, 2) and len(content) == 12:
        result["handshake"] = {
            "srt_version": f"0x{_u32(content, 0):06x}",
            "flags": f"0x{_u32(content, 4):08x}",
            "receiver_delay_ms": _u16(content, 8),
            "sender_delay_ms": _u16(content, 10),
        }
    elif extension_type in (3, 4):
        result["key_material"] = _describe_key_material(content)
    return result


def describe_runtime_key_material_datagram(
    payload: bytes,
    direction: str,
) -> dict[str, object] | None:
    """Return runtime KM structure without salt or wrapped key bytes."""

    if len(payload) < PACKET_HEADER_SIZE:
        return None
    control_word = _u32(payload, 0)
    if (control_word & 0x8000_0000) == 0:
        return None
    control_type = (control_word >> 16) & 0x7FFF
    subtype = control_word & 0xFFFF
    if (
        control_type != USER_DEFINED_CONTROL_TYPE
        or subtype not in RUNTIME_KEY_MATERIAL_NAMES
    ):
        return None
    return {
        "direction": direction,
        "datagram_bytes": len(payload),
        "destination_socket_id": _u32(payload, 12),
        "subtype": subtype,
        "name": RUNTIME_KEY_MATERIAL_NAMES[subtype],
        "key_material": _describe_key_material(
            payload[PACKET_HEADER_SIZE:]
        ),
    }


def describe_legacy_handshake_datagram(
    payload: bytes,
    direction: str,
) -> dict[str, object] | None:
    """Return secret-free HSv4 HSREQ/HSRSP structure."""

    if len(payload) < PACKET_HEADER_SIZE:
        return None
    control_word = _u32(payload, 0)
    if (control_word & 0x8000_0000) == 0:
        return None
    control_type = (control_word >> 16) & 0x7FFF
    subtype = control_word & 0xFFFF
    if (
        control_type != USER_DEFINED_CONTROL_TYPE
        or subtype not in LEGACY_HANDSHAKE_NAMES
    ):
        return None

    content = payload[PACKET_HEADER_SIZE:]
    result: dict[str, object] = {
        "direction": direction,
        "datagram_bytes": len(payload),
        "destination_socket_id": _u32(payload, 12),
        "subtype": subtype,
        "name": LEGACY_HANDSHAKE_NAMES[subtype],
        "content_bytes": len(content),
    }
    if len(content) < 8:
        result["malformed"] = "legacy handshake body is truncated"
        return result
    if len(content) % 4 != 0:
        result["malformed"] = "legacy handshake body is not word aligned"
        return result

    result.update(
        {
            "srt_version": _u32(content, 0),
            "flags": f"0x{_u32(content, 4):08x}",
        }
    )
    if len(content) >= 12:
        latency_word = _u32(content, 8)
        result["latency_word"] = f"0x{latency_word:08x}"
        result["latency_ms"] = latency_word & 0xFFFF
        if latency_word >> 16 != 0:
            result["malformed"] = "legacy latency reserved bits are set"
    return result


def describe_acknowledgement_datagram(
    payload: bytes,
    direction: str,
) -> dict[str, object] | None:
    """Return secret-free ACK metadata for causal wire validation."""

    if len(payload) < PACKET_HEADER_SIZE + 4:
        return None
    control_word = _u32(payload, 0)
    if (
        control_word & 0x8000_0000 == 0
        or (control_word >> 16) & 0x7FFF
        != ACKNOWLEDGEMENT_CONTROL_TYPE
    ):
        return None
    return {
        "direction": direction,
        "acknowledgement_number": _u32(payload, 4),
        "next_sequence": _u32(payload, PACKET_HEADER_SIZE)
        & SEQUENCE_MASK,
        "payload_bytes": len(payload) - PACKET_HEADER_SIZE,
    }


def describe_handshake_datagram(
    payload: bytes,
    direction: str,
) -> dict[str, object] | None:
    """Return public handshake structure, without salt or wrapped key bytes."""

    if len(payload) < PACKET_HEADER_SIZE:
        return None
    control_word = _u32(payload, 0)
    control_type = (control_word >> 16) & 0x7FFF
    if (control_word & 0x8000_0000) == 0:
        return None
    if control_type != HANDSHAKE_CONTROL_TYPE:
        return None

    result: dict[str, object] = {
        "direction": direction,
        "datagram_bytes": len(payload),
        "destination_socket_id": _u32(payload, 12),
    }
    if len(payload) < HANDSHAKE_DATAGRAM_SIZE:
        result["malformed"] = "handshake payload is truncated"
        return result

    fields = _u32(payload, 20)
    request = _i32(payload, 36)
    result.update(
        {
            "version": _u32(payload, 16),
            "encryption_field": fields >> 16,
            "extension_field": f"0x{fields & 0xFFFF:04x}",
            "initial_sequence": _u32(payload, 24),
            "maximum_transmission_unit": _u32(payload, 28),
            "flow_window": _u32(payload, 32),
            "request": request,
            "request_name": REQUEST_NAMES.get(request, "REJECTION"),
            "socket_id": _u32(payload, 40),
            "syn_cookie": f"0x{_u32(payload, 44):08x}",
        }
    )

    extensions: list[dict[str, object]] = []
    offset = HANDSHAKE_DATAGRAM_SIZE
    while offset < len(payload):
        if len(payload) - offset < 4:
            extensions.append(
                {
                    "malformed": "extension header is truncated",
                    "remaining_bytes": len(payload) - offset,
                }
            )
            break
        extension_type = _u16(payload, offset)
        length_words = _u16(payload, offset + 2)
        content_bytes = length_words * 4
        end = offset + 4 + content_bytes
        if end > len(payload):
            extensions.append(
                {
                    "type": extension_type,
                    "name": EXTENSION_NAMES.get(
                        extension_type, "UNKNOWN"
                    ),
                    "length_words": length_words,
                    "malformed": "extension content is truncated",
                    "available_bytes": len(payload) - offset - 4,
                }
            )
            break
        extensions.append(
            _describe_extension(
                extension_type,
                length_words,
                payload[offset + 4 : end],
            )
        )
        offset = end
    result["extensions"] = extensions
    return result


def describe_shutdown_datagram(
    payload: bytes,
    direction: str,
) -> dict[str, object] | None:
    """Describe one SHUTDOWN control packet without opaque payload exposure."""

    if len(payload) < PACKET_HEADER_SIZE:
        return None
    control_word = _u32(payload, 0)
    if (
        (control_word & 0x8000_0000) == 0
        or (control_word >> 16) & 0x7FFF != SHUTDOWN_CONTROL_TYPE
    ):
        return None
    result: dict[str, object] = {
        "direction": direction,
        "datagram_bytes": len(payload),
        "subtype": control_word & 0xFFFF,
        "type_specific_information": _u32(payload, 4),
        "timestamp": _u32(payload, 8),
        "destination_socket_id": _u32(payload, 12),
    }
    if len(payload) == PACKET_HEADER_SIZE + 4:
        # Historical UDT/SRT implementations keep one zero-valued iovec pad
        # because their writev path does not permit an empty data vector.
        # Expose the word structurally so version-scoped callers can validate
        # it without treating arbitrary SHUTDOWN payload as compatible.
        result["payload_bytes"] = 4
        result["legacy_padding_word"] = _u32(payload, PACKET_HEADER_SIZE)
    elif len(payload) != PACKET_HEADER_SIZE:
        result["payload_bytes"] = len(payload) - PACKET_HEADER_SIZE
        result["malformed"] = "SHUTDOWN datagram has unsupported payload"
    return result


class _HandshakeTraceRecorder:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._entries: dict[str, dict[str, object]] = {}
        self._error: str | None = None

    def render(self, scenario: str) -> str:
        with self._lock:
            entries = [dict(entry) for entry in self._entries.values()]
            error = self._error
        lines = [
            json.dumps(
                {
                    "event": "srt_handshake_trace",
                    "scenario": scenario,
                    **entry,
                },
                sort_keys=True,
            )
            for entry in entries
        ]
        if error is not None:
            lines.append(
                json.dumps(
                    {
                        "event": "srt_handshake_trace_error",
                        "scenario": scenario,
                        "error": error,
                    },
                    sort_keys=True,
                )
            )
        return "\n".join(lines) if lines else "<empty>"

    def error(self) -> str | None:
        with self._lock:
            return self._error

    def conclusion_socket_id(self, direction: str) -> int | None:
        """Return the single positive socket ID in an endpoint's conclusion."""
        if direction not in TRACE_ENDPOINT_DIRECTIONS:
            raise ValueError(f"unsupported relay direction {direction!r}")
        with self._lock:
            socket_ids = {
                entry.get("socket_id")
                for entry in self._entries.values()
                if entry.get("direction") == direction
                and entry.get("request") == -1
                and type(entry.get("socket_id")) is int
                and int(entry["socket_id"]) > 0
            }
        if len(socket_ids) != 1:
            return None
        return int(next(iter(socket_ids)))

    def conclusion_initial_sequence(self, direction: str) -> int | None:
        """Return the single 31-bit ISN in an endpoint's conclusion."""
        if direction not in TRACE_ENDPOINT_DIRECTIONS:
            raise ValueError(f"unsupported relay direction {direction!r}")
        with self._lock:
            initial_sequences = {
                entry.get("initial_sequence")
                for entry in self._entries.values()
                if entry.get("direction") == direction
                and entry.get("request") == -1
                and type(entry.get("initial_sequence")) is int
                and 0 <= int(entry["initial_sequence"]) <= SEQUENCE_MASK
            }
        if len(initial_sequences) != 1:
            return None
        return int(next(iter(initial_sequences)))

    def _record(
        self,
        payload: bytes,
        direction: str,
    ) -> None:
        description = describe_handshake_datagram(payload, direction)
        if description is None:
            return
        key = json.dumps(description, sort_keys=True)
        with self._lock:
            existing = self._entries.get(key)
            if existing is None:
                self._entries[key] = {**description, "count": 1}
            else:
                existing["count"] = int(existing["count"]) + 1

    def _set_error(self, error: OSError) -> None:
        with self._lock:
            if self._error is None:
                self._error = f"{type(error).__name__}: {error}"


class HandshakeTraceProxy(_HandshakeTraceRecorder):
    """Forward caller/listener UDP and retain secret-safe handshake metadata."""

    def __init__(
        self, target_port: int, *, host: str = "127.0.0.1"
    ) -> None:
        super().__init__()
        address = ipaddress.ip_address(host)
        family = socket.AF_INET6 if address.version == 6 else socket.AF_INET
        canonical_host = str(address)
        self._socket = socket.socket(family, socket.SOCK_DGRAM)
        try:
            self._socket.bind((canonical_host, 0))
            self._socket.settimeout(0.05)
        except OSError:
            self._socket.close()
            raise
        self._target = (
            (canonical_host, target_port, 0, 0)
            if family == socket.AF_INET6
            else (canonical_host, target_port)
        )
        self._source: (
            tuple[str, int] | tuple[str, int, int, int] | None
        ) = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._started = False
        self._legacy_handshake_events: list[dict[str, object]] = []
        self._runtime_key_material_events: list[dict[str, object]] = []
        self._first_data_events: dict[str, dict[str, object]] = {}
        self._first_ack_events: dict[str, dict[str, object]] = {}
        self._shutdown_events: list[dict[str, object]] = []
        self._control_events_omitted = 0
        self._relay_ordinal = 0

    @property
    def port(self) -> int:
        return int(self._socket.getsockname()[1])

    def start(self) -> None:
        if self._started:
            raise RuntimeError("handshake trace proxy is already started")
        self._thread.start()
        self._started = True

    def close(self) -> None:
        self._stop.set()
        if self._started:
            self._thread.join(timeout=1)
        self._socket.close()

    def render(self, scenario: str) -> str:
        trace = super().render(scenario)
        with self._lock:
            legacy = [dict(event) for event in self._legacy_handshake_events]
            key_material = [
                dict(event) for event in self._runtime_key_material_events
            ]
            first_data = [
                dict(event) for event in self._first_data_events.values()
            ]
            first_ack = [
                dict(event) for event in self._first_ack_events.values()
            ]
            shutdown = [dict(event) for event in self._shutdown_events]
            omitted = self._control_events_omitted
        events = [
            json.dumps(
                {
                    "event": "srt_legacy_handshake_trace",
                    "scenario": scenario,
                    "ordinal": ordinal,
                    **entry,
                },
                sort_keys=True,
            )
            for ordinal, entry in enumerate(legacy, start=1)
        ]
        events.extend(
            json.dumps(
                {
                    "event": "srt_runtime_key_material_trace",
                    "scenario": scenario,
                    "ordinal": ordinal,
                    **entry,
                },
                sort_keys=True,
            )
            for ordinal, entry in enumerate(key_material, start=1)
        )
        events.extend(
            json.dumps(
                {
                    "event": "srt_first_data_trace",
                    "scenario": scenario,
                    **entry,
                },
                sort_keys=True,
            )
            for entry in first_data
        )
        events.extend(
            json.dumps(
                {
                    "event": "srt_first_ack_trace",
                    "scenario": scenario,
                    **entry,
                },
                sort_keys=True,
            )
            for entry in first_ack
        )
        events.extend(
            json.dumps(
                {
                    "event": "srt_shutdown_trace",
                    "scenario": scenario,
                    "ordinal": ordinal,
                    **entry,
                },
                sort_keys=True,
            )
            for ordinal, entry in enumerate(shutdown, start=1)
        )
        if omitted != 0:
            events.append(
                json.dumps(
                    {
                        "event": "srt_control_trace_truncated",
                        "scenario": scenario,
                        "omitted": omitted,
                    },
                    sort_keys=True,
                )
            )
        if not events:
            return trace
        if trace == "<empty>":
            return "\n".join(events)
        return "\n".join((trace, *events))

    def _record_wire(self, payload: bytes, direction: str) -> None:
        legacy = describe_legacy_handshake_datagram(payload, direction)
        key_material = describe_runtime_key_material_datagram(
            payload, direction
        )
        acknowledgement = describe_acknowledgement_datagram(
            payload, direction
        )
        shutdown = describe_shutdown_datagram(payload, direction)
        with self._lock:
            self._relay_ordinal += 1
            relay_ordinal = self._relay_ordinal
            if (
                len(payload) >= PACKET_HEADER_SIZE
                and (_u32(payload, 0) & 0x8000_0000) == 0
                and direction not in self._first_data_events
            ):
                message_word = _u32(payload, 4)
                self._first_data_events[direction] = {
                    "direction": direction,
                    "relay_ordinal": relay_ordinal,
                    "sequence": _u32(payload, 0) & SEQUENCE_MASK,
                    "destination_socket_id": _u32(payload, 12),
                    "key_selection": (message_word >> 27) & 0x03,
                    "payload_bytes": len(payload) - PACKET_HEADER_SIZE,
                }
            if (
                acknowledgement is not None
                and direction not in self._first_ack_events
            ):
                self._first_ack_events[direction] = {
                    **acknowledgement,
                    "relay_ordinal": relay_ordinal,
                }
            if shutdown is not None:
                if len(self._shutdown_events) < MAX_SHUTDOWN_EVENTS:
                    self._shutdown_events.append(
                        {**shutdown, "relay_ordinal": relay_ordinal}
                    )
                else:
                    self._control_events_omitted += 1
            if legacy is None and key_material is None:
                return
            target = (
                self._legacy_handshake_events
                if legacy is not None
                else self._runtime_key_material_events
            )
            if len(target) < MAX_RUNTIME_KEY_MATERIAL_EVENTS:
                event = legacy if legacy is not None else key_material
                assert event is not None
                target.append(
                    {**event, "relay_ordinal": relay_ordinal}
                )
            else:
                self._control_events_omitted += 1

    def _should_forward_wire(self, payload: bytes, direction: str) -> bool:
        """Allow focused trace subclasses to suppress one wire class."""
        del payload, direction
        return True

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                payload, address = self._socket.recvfrom(65_535)
            except socket.timeout:
                continue
            except OSError as error:
                if not self._stop.is_set():
                    self._set_error(error)
                return
            try:
                if address == self._target:
                    self._record(payload, "listener_to_caller")
                    self._record_wire(payload, "listener_to_caller")
                    if (
                        self._source is not None
                        and self._should_forward_wire(
                            payload, "listener_to_caller"
                        )
                    ):
                        self._socket.sendto(payload, self._source)
                    continue

                self._source = address
                self._record(payload, "caller_to_listener")
                self._record_wire(payload, "caller_to_listener")
                if self._should_forward_wire(payload, "caller_to_listener"):
                    self._socket.sendto(payload, self._target)
            except OSError as error:
                if not self._stop.is_set():
                    self._set_error(error)
                return


class RendezvousTraceProxy(_HandshakeTraceRecorder):
    """Relay both rendezvous directions and observe the cookie contest."""

    def __init__(
        self,
        sender_target_port: int,
        receiver_target_port: int,
        fault: (
            RendezvousFault
            | tuple[RendezvousFault, ...]
            | None
        ) = None,
        *,
        host: str = "127.0.0.1",
    ) -> None:
        super().__init__()
        address = ipaddress.ip_address(host)
        family = (
            socket.AF_INET6
            if address.version == 6
            else socket.AF_INET
        )
        self._sender_socket = socket.socket(
            family, socket.SOCK_DGRAM
        )
        self._receiver_socket = socket.socket(
            family, socket.SOCK_DGRAM
        )
        try:
            for trace_socket in (
                self._sender_socket,
                self._receiver_socket,
            ):
                trace_socket.bind((host, 0))
                trace_socket.setblocking(False)
        except OSError:
            self._sender_socket.close()
            self._receiver_socket.close()
            raise
        self._sender_target = (host, sender_target_port)
        self._receiver_target = (host, receiver_target_port)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._started = False
        self._initialize_fault_state(fault)

    def _initialize_fault_state(
        self,
        fault: (
            RendezvousFault
            | tuple[RendezvousFault, ...]
            | None
        ),
    ) -> None:
        self._faults = (
            ()
            if fault is None
            else fault
            if isinstance(fault, tuple)
            else (fault,)
        )
        if not all(
            isinstance(item, RendezvousFault)
            for item in self._faults
        ):
            raise TypeError(
                "fault plan must contain RendezvousFault values"
            )
        self._fault_matches = [0] * len(self._faults)
        self._fault_events: list[
            dict[str, object] | None
        ] = [None] * len(self._faults)
        self._fault_data_sequences: set[
            tuple[str, int]
        ] = set()
        self._delay_direction: str | None = None
        self._delay_deadline: float | None = None
        self._delay_queue: list[
            tuple[
                socket.socket,
                tuple[str, int],
                bytes,
                str,
                dict[str, object],
            ]
        ] = []
        self._active_delay_fault_index: int | None = None
        self._late_duplicates: list[
            tuple[
                float,
                int,
                socket.socket,
                tuple[str, int],
                bytes,
                str,
                dict[str, object],
            ]
        ] = []
        self._reorder_held: (
            tuple[
                str,
                str,
                socket.socket,
                tuple[str, int],
                bytes,
                dict[str, object],
                int,
            ]
            | None
        ) = None
        self._forwarded_data_retransmissions = {
            "sender_to_receiver": 0,
            "receiver_to_sender": 0,
        }
        self._forwarded_data_observations: dict[
            str, dict[str, object]
        ] = {
            "sender_to_receiver": {
                "count": 0,
                "acknowledged": False,
            },
            "receiver_to_sender": {
                "count": 0,
                "acknowledged": False,
            },
        }
        self._runtime_key_material_events: list[
            dict[str, object]
        ] = []
        self._runtime_key_material_events_omitted = 0
        self._data_key_identities: dict[
            tuple[str, int, int], tuple[int, int]
        ] = {}
        self._data_key_observations: dict[
            str, dict[str, object]
        ] = {
            "sender_to_receiver": {
                "packets": 0,
                "unencrypted_packets": 0,
                "transitions": 0,
                "selectors": [],
                "destination_socket_ids": set(),
                "complete": True,
            },
            "receiver_to_sender": {
                "packets": 0,
                "unencrypted_packets": 0,
                "transitions": 0,
                "selectors": [],
                "destination_socket_ids": set(),
                "complete": True,
            },
        }
        self._loss_report_events: list[dict[str, object]] = []
        self._loss_report_events_omitted = 0
        self._latest_cumulative_acknowledgements: dict[
            str, dict[str, object]
        ] = {}
        self._retransmission_events: list[dict[str, object]] = []
        self._retransmission_events_omitted = 0
        self._original_data: dict[
            tuple[str, int, int], dict[str, object]
        ] = {}
        self._original_data_omitted = 0
        self._relay_ordinal = 0

    @property
    def sender_port(self) -> int:
        return int(self._sender_socket.getsockname()[1])

    @property
    def receiver_port(self) -> int:
        return int(self._receiver_socket.getsockname()[1])

    def start(self) -> None:
        self._thread.start()
        self._started = True

    def close(self) -> None:
        self._stop.set()
        if self._started:
            self._thread.join(timeout=1)
        self._sender_socket.close()
        self._receiver_socket.close()

    def role_observation(self) -> dict[str, object] | None:
        with self._lock:
            entries = [dict(entry) for entry in self._entries.values()]
        return rendezvous_role_observation(entries)

    def fault_observation(self) -> dict[str, object] | None:
        observations = self.fault_observations()
        return observations[0] if observations else None

    def fault_observations(self) -> list[dict[str, object]]:
        with self._lock:
            return [
                dict(event)
                for event in self._fault_events
                if event is not None
            ]

    def forwarded_data_retransmissions(self, direction: str) -> int:
        if direction not in self._forwarded_data_retransmissions:
            raise ValueError(f"unsupported relay direction {direction!r}")
        with self._lock:
            return self._forwarded_data_retransmissions[direction]

    def loss_report_observations(self) -> list[dict[str, object]]:
        with self._lock:
            return [dict(event) for event in self._loss_report_events]

    def retransmission_observations(
        self, direction: str
    ) -> list[dict[str, object]]:
        if direction not in self._forwarded_data_retransmissions:
            raise ValueError(f"unsupported relay direction {direction!r}")
        with self._lock:
            return [
                dict(event)
                for event in self._retransmission_events
                if event.get("direction") == direction
            ]

    def arq_trace_complete(self) -> bool:
        with self._lock:
            return (
                self._loss_report_events_omitted == 0
                and self._retransmission_events_omitted == 0
                and self._original_data_omitted == 0
            )

    def forwarded_data_observation(
        self, direction: str
    ) -> dict[str, object]:
        if direction not in self._forwarded_data_observations:
            raise ValueError(f"unsupported relay direction {direction!r}")
        with self._lock:
            return dict(self._forwarded_data_observations[direction])

    def data_key_observation(self, direction: str) -> dict[str, object]:
        if direction not in self._data_key_observations:
            raise ValueError(f"unsupported relay direction {direction!r}")
        with self._lock:
            observation = self._data_key_observations[direction]
            return {
                **observation,
                "selectors": list(observation["selectors"]),
                "destination_socket_ids": sorted(
                    observation["destination_socket_ids"]
                ),
            }

    def _record_data_key_selection(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        if (
            metadata.get("packet_kind") != "data"
            or metadata.get("filter_control") is True
        ):
            return
        sequence = metadata.get("sequence")
        message_number = metadata.get("message_number")
        selector = metadata.get("key_selection")
        destination_socket_id = metadata.get("destination_socket_id")
        if (
            type(sequence) is not int
            or type(message_number) is not int
            or type(selector) is not int
            or type(destination_socket_id) is not int
        ):
            with self._lock:
                self._data_key_observations[direction]["complete"] = False
            return
        identity = (direction, sequence, message_number)
        with self._lock:
            if identity in self._data_key_identities:
                if self._data_key_identities[identity] != (
                    selector,
                    destination_socket_id,
                ):
                    self._data_key_observations[direction][
                        "complete"
                    ] = False
                return
            observation = self._data_key_observations[direction]
            if len(self._data_key_identities) >= MAX_DATA_KEY_IDENTITIES:
                observation["complete"] = False
                return
            self._data_key_identities[identity] = (
                selector,
                destination_socket_id,
            )
            observation["packets"] = int(observation["packets"]) + 1
            if selector == 0:
                observation["unencrypted_packets"] = (
                    int(observation["unencrypted_packets"]) + 1
                )
            selectors = observation["selectors"]
            if not isinstance(selectors, list):
                raise RuntimeError("invalid data-key trace state")
            if not selectors or selectors[-1] != selector:
                if selectors:
                    observation["transitions"] = (
                        int(observation["transitions"]) + 1
                    )
                selectors.append(selector)
            socket_ids = observation["destination_socket_ids"]
            if not isinstance(socket_ids, set):
                raise RuntimeError("invalid data-key socket trace state")
            socket_ids.add(destination_socket_id)

    def render(self, scenario: str) -> str:
        trace = super().render(scenario)
        faults = self.fault_observations()
        events = [
            json.dumps(
                {
                    "event": "srt_fault_injection",
                    "scenario": scenario,
                    **fault,
                },
                sort_keys=True,
            )
            for fault in faults
        ]
        with self._lock:
            runtime_key_material = [
                dict(event)
                for event in self._runtime_key_material_events
            ]
            omitted = self._runtime_key_material_events_omitted
            loss_reports = [
                dict(event) for event in self._loss_report_events
            ]
            retransmissions = [
                dict(event) for event in self._retransmission_events
            ]
            arq_omitted = (
                self._loss_report_events_omitted
                + self._retransmission_events_omitted
                + self._original_data_omitted
            )
        events.extend(
            json.dumps(
                {
                    "event": "srt_runtime_key_material_trace",
                    "scenario": scenario,
                    "ordinal": ordinal,
                    **entry,
                },
                sort_keys=True,
            )
            for ordinal, entry in enumerate(
                runtime_key_material, start=1
            )
        )
        if omitted != 0:
            events.append(
                json.dumps(
                    {
                        "event":
                            "srt_runtime_key_material_trace_truncated",
                        "scenario": scenario,
                        "omitted": omitted,
                    },
                    sort_keys=True,
                )
            )
        events.extend(
            json.dumps(
                {
                    "event": "srt_loss_report_trace",
                    "scenario": scenario,
                    **entry,
                },
                sort_keys=True,
            )
            for entry in loss_reports
        )
        events.extend(
            json.dumps(
                {
                    "event": "srt_retransmission_trace",
                    "scenario": scenario,
                    **entry,
                },
                sort_keys=True,
            )
            for entry in retransmissions
        )
        if arq_omitted != 0:
            events.append(
                json.dumps(
                    {
                        "event": "srt_arq_trace_truncated",
                        "scenario": scenario,
                        "omitted": arq_omitted,
                    },
                    sort_keys=True,
                )
            )
        if not events:
            return trace
        if trace == "<empty>":
            return "\n".join(events)
        return "\n".join((trace, *events))

    def _record_runtime_key_material(
        self,
        payload: bytes,
        direction: str,
    ) -> None:
        description = describe_runtime_key_material_datagram(
            payload, direction
        )
        if description is None:
            return
        with self._lock:
            if (
                len(self._runtime_key_material_events)
                < MAX_RUNTIME_KEY_MATERIAL_EVENTS
            ):
                self._runtime_key_material_events.append(
                    description
                )
            else:
                self._runtime_key_material_events_omitted += 1

    @staticmethod
    def _packet_metadata(payload: bytes) -> dict[str, object]:
        if len(payload) < PACKET_HEADER_SIZE:
            return {"packet_kind": "malformed"}
        first_word = _u32(payload, 0)
        if (first_word & 0x8000_0000) == 0:
            message_word = _u32(payload, 4)
            message_number = message_word & 0x03FF_FFFF
            return {
                "packet_kind": "data",
                "sequence": first_word & 0x7FFF_FFFF,
                "message_number": message_number,
                "destination_socket_id": _u32(payload, 12),
                "filter_control": message_number == 0,
                "key_selection": (message_word >> 27) & 0x03,
                "retransmitted": bool((message_word >> 26) & 0x01),
                "payload_bytes": len(payload) - PACKET_HEADER_SIZE,
                "payload_sha256": hashlib.sha256(
                    payload[PACKET_HEADER_SIZE:]
                ).hexdigest(),
            }
        control_type = (first_word >> 16) & 0x7FFF
        if control_type != HANDSHAKE_CONTROL_TYPE:
            result: dict[str, object] = {
                "packet_kind": "control",
                "control_type": control_type,
            }
            if control_type == SHUTDOWN_CONTROL_TYPE:
                shutdown = describe_shutdown_datagram(
                    payload, "fault_match"
                )
                if shutdown is not None:
                    result.update(
                        {
                            key: value
                            for key, value in shutdown.items()
                            if key != "direction"
                        }
                    )
            if (
                control_type == ACKNOWLEDGEMENT_CONTROL_TYPE
                and len(payload) >= PACKET_HEADER_SIZE + 4
            ):
                result.update(
                    {
                        "acknowledgement_number": _u32(payload, 4),
                        "next_sequence": _u32(
                            payload, PACKET_HEADER_SIZE
                        )
                        & SEQUENCE_MASK,
                        "acknowledgement_payload_bytes":
                            len(payload) - PACKET_HEADER_SIZE,
                    }
                )
            return result
        handshake = describe_handshake_datagram(payload, "fault_match")
        if handshake is None:
            return {"packet_kind": "malformed"}
        return {
            "packet_kind": "handshake",
            "request": handshake.get("request"),
            "request_name": handshake.get("request_name"),
        }

    def _route(
        self, direction: str
    ) -> tuple[socket.socket, tuple[str, int]]:
        if direction == "sender_to_receiver":
            return self._receiver_socket, self._receiver_target
        return self._sender_socket, self._sender_target

    def _matching_fault(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> tuple[int, RendezvousFault] | None:
        if metadata.get("packet_kind") == "data":
            has_pending_data_fault = any(
                event is None
                and fault.packet_kind == "data"
                and (
                    fault.direction == "either"
                    or fault.direction == direction
                )
                for fault, event in zip(
                    self._faults, self._fault_events
                )
            )
            if not has_pending_data_fault:
                return None
            sequence = metadata.get("sequence")
            if not isinstance(sequence, int):
                return None
            sequence_key = (direction, sequence)
            if sequence_key in self._fault_data_sequences:
                return None
            self._fault_data_sequences.add(sequence_key)

        selected: tuple[int, RendezvousFault] | None = None
        for index, fault in enumerate(self._faults):
            if (
                self._fault_events[index] is not None
                or (
                    fault.direction != "either"
                    and direction != fault.direction
                )
                or metadata.get("packet_kind")
                    != fault.packet_kind
                or (
                    fault.packet_kind == "data"
                    and metadata.get("retransmitted") is True
                )
                or (
                    fault.handshake_request is not None
                    and metadata.get("request")
                        != fault.handshake_request
                )
                or (
                    fault.control_type is not None
                    and metadata.get("control_type")
                        != fault.control_type
                )
            ):
                continue
            self._fault_matches[index] += 1
            if self._fault_matches[index] == fault.occurrence:
                if selected is not None:
                    raise OSError(
                        errno.EINVAL,
                        "multiple rendezvous faults target "
                        "the same datagram",
                    )
                selected = (index, fault)
        return selected

    def _record_fault(
        self,
        index: int,
        fault: RendezvousFault,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        event = {
            "plan_index": index + 1,
            "action": fault.action,
            "direction": direction,
            "occurrence": fault.occurrence,
            **metadata,
        }
        if fault.suppress_retransmissions:
            event.update(
                {
                    "suppress_retransmissions": True,
                    "suppressed_retransmissions": 0,
                }
            )
        if fault.action == "drop" and fault.packet_kind == "data":
            event["later_data_observed_before_retransmission"] = False
        if fault.action in {"delay", "late_duplicate"}:
            event["delay_milliseconds"] = (
                fault.delay_milliseconds
            )
        with self._lock:
            self._fault_events[index] = event

    @staticmethod
    def _sequence_is_acknowledged(
        next_sequence: int, sequence: int
    ) -> bool:
        distance = (next_sequence - sequence) & SEQUENCE_MASK
        return 0 < distance < ((SEQUENCE_MASK + 1) // 2)

    def _record_cumulative_acknowledgement(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        next_sequence = metadata.get("next_sequence")
        if not isinstance(next_sequence, int):
            return
        opposite = {
            "sender_to_receiver": "receiver_to_sender",
            "receiver_to_sender": "sender_to_receiver",
        }
        with self._lock:
            self._latest_cumulative_acknowledgements[direction] = {
                "next_sequence": next_sequence,
                "relay_ordinal": metadata.get("relay_ordinal"),
            }
            for event in self._fault_events:
                dropped_sequence = (
                    event.get("sequence")
                    if event is not None
                    else None
                )
                if (
                    event is None
                    or event.get("action") != "drop"
                    or event.get("packet_kind") != "data"
                    or opposite.get(str(event.get("direction")))
                        != direction
                    or not isinstance(dropped_sequence, int)
                    or not self._sequence_is_acknowledged(
                        next_sequence, dropped_sequence
                    )
                    or event.get("cumulative_ack_observed") is True
                ):
                    continue
                event.update(
                    {
                        "cumulative_ack_observed": True,
                        "cumulative_ack_next_sequence": next_sequence,
                        "cumulative_ack_number": metadata.get(
                            "acknowledgement_number"
                        ),
                        "cumulative_ack_payload_bytes": metadata.get(
                            "acknowledgement_payload_bytes"
                        ),
                        "cumulative_ack_relay_ordinal": metadata.get(
                            "relay_ordinal"
                        ),
                        "cumulative_ack_delay_microseconds": (
                            (
                                int(metadata["relay_monotonic_ns"])
                                - int(event["relay_monotonic_ns"])
                            )
                            // 1_000
                            if "relay_monotonic_ns" in metadata
                            and "relay_monotonic_ns" in event
                            else None
                        ),
                    }
                )
            forwarded_direction = opposite.get(direction)
            if forwarded_direction is None:
                return
            observation = self._forwarded_data_observations[
                forwarded_direction
            ]
            first_sequence = observation.get("first_sequence")
            if (
                observation.get("acknowledged") is not True
                and isinstance(first_sequence, int)
                and self._sequence_is_acknowledged(
                    next_sequence, first_sequence
                )
            ):
                observation.update(
                    {
                        "acknowledged": True,
                        "acknowledgement_next_sequence": next_sequence,
                        "acknowledgement_relay_ordinal": metadata.get(
                            "relay_ordinal"
                        ),
                        "acknowledgement_monotonic_ns": metadata.get(
                            "relay_monotonic_ns"
                        ),
                        "acknowledgement_forwarded_data_count": (
                            observation.get("count")
                        ),
                    }
                )

    def _suppress_dropped_retransmission(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> bool:
        if (
            metadata.get("packet_kind") != "data"
            or metadata.get("retransmitted") is not True
        ):
            return False
        with self._lock:
            for event in self._fault_events:
                if (
                    event is None
                    or event.get("suppress_retransmissions") is not True
                    or event.get("direction") != direction
                    or event.get("sequence") != metadata.get("sequence")
                    or event.get("message_number")
                        != metadata.get("message_number")
                ):
                    continue
                attempts = int(
                    event.get("suppressed_retransmissions", 0)
                ) + 1
                event.update(
                    {
                        "suppressed_retransmissions": attempts,
                        "last_suppressed_retransmission_ordinal":
                            metadata.get("relay_ordinal"),
                        "last_suppressed_retransmission_delay_microseconds":
                            (
                                (
                                    int(metadata["relay_monotonic_ns"])
                                    - int(event["relay_monotonic_ns"])
                                )
                                // 1_000
                                if "relay_monotonic_ns" in metadata
                                and "relay_monotonic_ns" in event
                                else None
                            ),
                    }
                )
                return True
        return False

    def _record_retransmission(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        sequence = metadata.get("sequence")
        message_number = metadata.get("message_number")
        if (
            metadata.get("packet_kind") != "data"
            or type(sequence) is not int
            or type(message_number) is not int
            or message_number <= 0
        ):
            if metadata.get("retransmitted") is True:
                self._match_retransmission(direction, metadata)
            return
        identity = (direction, sequence, message_number)
        if metadata.get("retransmitted") is not True:
            with self._lock:
                if identity not in self._original_data:
                    if (
                        len(self._original_data)
                        < MAX_ORIGINAL_DATA_IDENTITIES
                    ):
                        self._original_data[identity] = {
                            "key_selection": metadata.get(
                                "key_selection"
                            ),
                            "payload_bytes": metadata.get(
                                "payload_bytes"
                            ),
                            "payload_sha256": metadata.get(
                                "payload_sha256"
                            ),
                            "relay_ordinal": metadata.get(
                                "relay_ordinal"
                            ),
                            "relay_monotonic_ns": metadata.get(
                                "relay_monotonic_ns"
                            ),
                        }
                    else:
                        self._original_data_omitted += 1
            return
        with self._lock:
            original = self._original_data.get(identity)
            acknowledgement = self._latest_cumulative_acknowledgements.get(
                {
                    "sender_to_receiver": "receiver_to_sender",
                    "receiver_to_sender": "sender_to_receiver",
                }.get(direction, "")
            )
            event = {
                "direction": direction,
                "sequence": sequence,
                "message_number": message_number,
                "key_selection": metadata.get("key_selection"),
                "payload_bytes": metadata.get("payload_bytes"),
                "payload_sha256": metadata.get("payload_sha256"),
                "relay_ordinal": metadata.get("relay_ordinal"),
                "original_observed": original is not None,
                "original_relay_ordinal": (
                    original.get("relay_ordinal")
                    if original is not None
                    else None
                ),
                "retransmission_delay_microseconds": (
                    (
                        int(metadata["relay_monotonic_ns"])
                        - int(original["relay_monotonic_ns"])
                    )
                    // 1_000
                    if original is not None
                    and type(metadata.get("relay_monotonic_ns")) is int
                    and type(original.get("relay_monotonic_ns")) is int
                    else None
                ),
                "prior_cumulative_ack_next_sequence": (
                    acknowledgement.get("next_sequence")
                    if acknowledgement is not None
                    else None
                ),
                "prior_cumulative_ack_relay_ordinal": (
                    acknowledgement.get("relay_ordinal")
                    if acknowledgement is not None
                    else None
                ),
                "original_key_selection": (
                    original.get("key_selection")
                    if original is not None
                    else None
                ),
                "original_payload_bytes": (
                    original.get("payload_bytes")
                    if original is not None
                    else None
                ),
                "original_payload_sha256": (
                    original.get("payload_sha256")
                    if original is not None
                    else None
                ),
                "ciphertext_matches_original": (
                    original is not None
                    and original.get("key_selection")
                        == metadata.get("key_selection")
                    and original.get("payload_bytes")
                        == metadata.get("payload_bytes")
                    and original.get("payload_sha256")
                        == metadata.get("payload_sha256")
                ),
            }
            if len(self._retransmission_events) < MAX_ARQ_TRACE_EVENTS:
                self._retransmission_events.append(event)
            else:
                self._retransmission_events_omitted += 1
        self._match_retransmission(direction, metadata)

    def _match_retransmission(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        with self._lock:
            if metadata.get("packet_kind") != "data":
                return
            for event in self._fault_events:
                if (
                    event is None
                    or event.get("action") != "drop"
                    or event.get("packet_kind") != "data"
                    or event.get("direction") != direction
                    or event.get("sequence")
                        != metadata.get("sequence")
                    # FEC controls can reuse their protected source
                    # sequence. A retransmission preserves both identifiers.
                    or event.get("message_number")
                        != metadata.get("message_number")
                    or event.get("retransmission_observed") is True
                ):
                    continue
                original_digest = event.get("payload_sha256")
                retransmission_digest = metadata.get(
                    "payload_sha256"
                )
                event.update(
                    {
                        "retransmission_observed": True,
                        "retransmission_flag": metadata.get(
                            "retransmitted"
                        ),
                        "retransmission_relay_ordinal": metadata.get(
                            "relay_ordinal"
                        ),
                        "retransmission_delay_microseconds": (
                            (
                                int(metadata["relay_monotonic_ns"])
                                - int(event["relay_monotonic_ns"])
                            )
                            // 1_000
                            if "relay_monotonic_ns" in metadata
                            and "relay_monotonic_ns" in event
                            else None
                        ),
                        "retransmission_key_selection": metadata.get(
                            "key_selection"
                        ),
                        "retransmission_payload_bytes": metadata.get(
                            "payload_bytes"
                        ),
                        "retransmission_payload_sha256": (
                            retransmission_digest
                        ),
                        "retransmission_ciphertext_matches": (
                            original_digest == retransmission_digest
                        ),
                    }
                )

    @staticmethod
    def _loss_report_ranges(
        payload: bytes,
    ) -> tuple[tuple[int, int], ...] | None:
        first_word = _u32(payload, 0) if len(payload) >= 4 else 0
        if (
            len(payload) < PACKET_HEADER_SIZE + 4
            or (first_word & 0x8000_0000) == 0
            or ((first_word >> 16) & 0x7FFF)
                != LOSS_REPORT_CONTROL_TYPE
        ):
            return None
        ranges: list[tuple[int, int]] = []
        offset = PACKET_HEADER_SIZE
        while offset + 4 <= len(payload):
            word = _u32(payload, offset)
            offset += 4
            if (word & 0x8000_0000) == 0:
                sequence = word & SEQUENCE_MASK
                ranges.append((sequence, sequence))
                continue
            if offset + 4 > len(payload):
                return None
            start = word & SEQUENCE_MASK
            end = _u32(payload, offset) & SEQUENCE_MASK
            offset += 4
            ranges.append((start, end))
        return tuple(ranges) if offset == len(payload) else None

    @classmethod
    def _loss_report_contains_sequence(
        cls, payload: bytes, sequence: int
    ) -> bool:
        ranges = cls._loss_report_ranges(payload)
        if ranges is None:
            return False
        return any(
            ((sequence - start) & SEQUENCE_MASK)
            <= ((end - start) & SEQUENCE_MASK)
            for start, end in ranges
        )

    def _record_loss_report(
        self,
        payload: bytes,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        opposite = {
            "sender_to_receiver": "receiver_to_sender",
            "receiver_to_sender": "sender_to_receiver",
        }
        ranges = self._loss_report_ranges(payload)
        if ranges is None:
            return
        with self._lock:
            event = {
                "direction": direction,
                "ranges": ranges,
                "relay_ordinal": metadata.get("relay_ordinal"),
            }
            if len(self._loss_report_events) < MAX_ARQ_TRACE_EVENTS:
                self._loss_report_events.append(event)
            else:
                self._loss_report_events_omitted += 1
            for event in self._fault_events:
                sequence = event.get("sequence") if event is not None else None
                if (
                    event is None
                    or event.get("action") != "drop"
                    or event.get("packet_kind") != "data"
                    or opposite.get(str(event.get("direction"))) != direction
                    or not isinstance(sequence, int)
                    or not self._loss_report_contains_sequence(
                        payload, sequence
                    )
                    or event.get("loss_report_observed") is True
                ):
                    continue
                event.update(
                    {
                        "loss_report_observed": True,
                        "loss_report_relay_ordinal": metadata.get(
                            "relay_ordinal"
                        ),
                        "loss_report_delay_microseconds": (
                            (
                                int(metadata["relay_monotonic_ns"])
                                - int(event["relay_monotonic_ns"])
                            )
                            // 1_000
                            if "relay_monotonic_ns" in metadata
                            and "relay_monotonic_ns" in event
                            else None
                        ),
                    }
                )

    def _flush_delay(self, force: bool = False) -> None:
        if self._delay_deadline is None:
            return
        if not force and time.monotonic() < self._delay_deadline:
            return
        released_at_ns = time.monotonic_ns()
        queued = self._delay_queue
        self._delay_queue = []
        self._delay_direction = None
        self._delay_deadline = None
        active_index = self._active_delay_fault_index
        self._active_delay_fault_index = None
        if active_index is not None:
            with self._lock:
                event = self._fault_events[active_index]
                if event is not None:
                    started_at_ns = event.get(
                        "relay_monotonic_ns"
                    )
                    event.update(
                        {
                            "delay_released": True,
                            "delayed_datagrams": len(queued),
                            "delay_elapsed_milliseconds": (
                                (
                                    released_at_ns
                                    - int(started_at_ns)
                                )
                                / 1_000_000
                                if isinstance(started_at_ns, int)
                                else None
                            ),
                        }
                    )
        for outbound, target, payload, direction, metadata in queued:
            self._send_forwarded(
                outbound,
                target,
                payload,
                direction,
                metadata,
            )

    def _queue_delayed(
        self,
        outbound: socket.socket,
        target: tuple[str, int],
        payload: bytes,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        if len(self._delay_queue) >= MAX_DELAYED_DATAGRAMS:
            raise OSError(
                errno.ENOBUFS,
                "rendezvous fault delay queue is full",
            )
        self._delay_queue.append(
            (
                outbound,
                target,
                payload,
                direction,
                dict(metadata),
            )
        )

    def _queue_late_duplicate(
        self,
        fault_index: int,
        fault: RendezvousFault,
        outbound: socket.socket,
        target: tuple[str, int],
        payload: bytes,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        if len(self._late_duplicates) >= MAX_DELAYED_DATAGRAMS:
            raise OSError(
                errno.ENOBUFS,
                "rendezvous late-duplicate queue is full",
            )
        self._late_duplicates.append(
            (
                time.monotonic()
                + fault.delay_milliseconds / 1_000,
                fault_index,
                outbound,
                target,
                payload,
                direction,
                dict(metadata),
            )
        )

    def _flush_late_duplicates(self, force: bool = False) -> None:
        if not self._late_duplicates:
            return
        now = time.monotonic()
        ready = []
        pending = []
        for item in self._late_duplicates:
            (ready if force or item[0] <= now else pending).append(item)
        if not ready:
            return
        self._late_duplicates = pending
        released_at_ns = time.monotonic_ns()
        for (
            _,
            fault_index,
            outbound,
            target,
            payload,
            direction,
            metadata,
        ) in ready:
            self._send_forwarded(
                outbound,
                target,
                payload,
                direction,
                metadata,
            )
            with self._lock:
                event = self._fault_events[fault_index]
                if event is not None:
                    started_at_ns = event.get("relay_monotonic_ns")
                    event.update(
                        {
                            "late_duplicate_released": True,
                            "delay_elapsed_milliseconds": (
                                (
                                    released_at_ns
                                    - int(started_at_ns)
                                )
                                / 1_000_000
                                if isinstance(started_at_ns, int)
                                else None
                            ),
                        }
                    )

    def _fault_view(self, payload: bytes, direction: str) -> bytes:
        return payload

    def _outbound_payload(self, payload: bytes, direction: str) -> bytes:
        return payload

    @staticmethod
    def _sequence_is_later(sequence: int, reference: int) -> bool:
        distance = (sequence - reference) & SEQUENCE_MASK
        return 0 < distance < ((SEQUENCE_MASK + 1) // 2)

    def _record_later_data(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        sequence = metadata.get("sequence")
        if (
            metadata.get("packet_kind") != "data"
            or type(sequence) is not int
        ):
            return
        with self._lock:
            for event in self._fault_events:
                dropped_sequence = (
                    event.get("sequence")
                    if event is not None
                    else None
                )
                if (
                    event is None
                    or event.get("action") != "drop"
                    or event.get("packet_kind") != "data"
                    or event.get("direction") != direction
                    or event.get("retransmission_observed") is True
                    or type(dropped_sequence) is not int
                    or not self._sequence_is_later(
                        sequence, dropped_sequence
                    )
                ):
                    continue
                if (
                    event.get(
                        "later_data_observed_before_retransmission"
                    )
                    is False
                ):
                    event.update(
                        {
                            "later_data_observed_before_retransmission":
                                True,
                            "first_later_data_sequence": sequence,
                            "first_later_data_relay_ordinal":
                                metadata.get("relay_ordinal"),
                        }
                    )

    def _record_forwarded(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        if metadata.get("packet_kind") != "data":
            return
        forwarded_at_ns = time.monotonic_ns()
        with self._lock:
            observation = self._forwarded_data_observations[direction]
            count = int(observation["count"]) + 1
            if count == 1:
                observation.update(
                    {
                        "first_sequence": metadata.get("sequence"),
                        "first_message_number": metadata.get(
                            "message_number"
                        ),
                        "first_relay_ordinal": metadata.get(
                            "relay_ordinal"
                        ),
                        "first_forwarded_monotonic_ns": forwarded_at_ns,
                        "destination_socket_id": metadata.get(
                            "destination_socket_id"
                        ),
                        "destination_socket_id_consistent": True,
                        "sequences_contiguous": True,
                        "message_numbers_contiguous": True,
                    }
                )
            else:
                previous_sequence = observation.get("last_sequence")
                previous_message_number = observation.get(
                    "last_message_number"
                )
                observation["sequences_contiguous"] = bool(
                    observation.get("sequences_contiguous")
                    and isinstance(previous_sequence, int)
                    and metadata.get("sequence")
                    == ((previous_sequence + 1) & 0x7FFF_FFFF)
                )
                observation["message_numbers_contiguous"] = bool(
                    observation.get("message_numbers_contiguous")
                    and isinstance(previous_message_number, int)
                    and metadata.get("message_number")
                    == previous_message_number + 1
                )
                observation["destination_socket_id_consistent"] = bool(
                    observation.get("destination_socket_id_consistent")
                    and metadata.get("destination_socket_id")
                    == observation.get("destination_socket_id")
                )
            observation.update(
                {
                    "count": count,
                    "last_sequence": metadata.get("sequence"),
                    "last_message_number": metadata.get("message_number"),
                    "last_relay_ordinal": metadata.get("relay_ordinal"),
                    "last_forwarded_monotonic_ns": forwarded_at_ns,
                }
            )
            if metadata.get("retransmitted") is True:
                self._forwarded_data_retransmissions[direction] += 1
        self._record_later_data(direction, metadata)

    def _send_forwarded(
        self,
        outbound: socket.socket,
        target: tuple[str, int],
        payload: bytes,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        self._send_datagram(outbound, target, payload)
        self._record_forwarded(direction, metadata)

    @staticmethod
    def _send_datagram(
        outbound: socket.socket,
        target: tuple[str, int],
        payload: bytes,
    ) -> None:
        """Forward one UDP datagram through bounded transient backpressure."""
        deadline = time.monotonic() + FORWARD_SEND_RETRY_SECONDS
        while True:
            try:
                sent = outbound.sendto(payload, target)
                if sent is not None and sent != len(payload):
                    raise OSError(
                        errno.EIO, "partial UDP datagram transmission"
                    )
                return
            except BlockingIOError as error:
                if error.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                    raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise
                _, writable, _ = select.select(
                    (),
                    (outbound,),
                    (),
                    min(FORWARD_SEND_POLL_SECONDS, remaining),
                )
                if not writable:
                    continue

    def _forward(self, payload: bytes, direction: str) -> None:
        outbound, target = self._route(direction)
        fault_view = self._fault_view(payload, direction)
        outbound_payload = self._outbound_payload(payload, direction)
        metadata = self._packet_metadata(fault_view)
        self._relay_ordinal = getattr(self, "_relay_ordinal", 0) + 1
        metadata.update(
            {
                "relay_ordinal": self._relay_ordinal,
                "relay_monotonic_ns": time.monotonic_ns(),
            }
        )
        self._record_loss_report(fault_view, direction, metadata)
        self._record_cumulative_acknowledgement(direction, metadata)
        self._record_data_key_selection(direction, metadata)
        if self._suppress_dropped_retransmission(
            direction, metadata
        ):
            return
        self._record_retransmission(direction, metadata)
        match = self._matching_fault(direction, metadata)

        if self._delay_deadline is not None:
            if match is not None:
                raise OSError(
                    errno.EINVAL,
                    "rendezvous fault overlaps an active delay",
                )
            if direction == self._delay_direction:
                self._queue_delayed(
                    outbound,
                    target,
                    outbound_payload,
                    direction,
                    metadata,
                )
                return

        if (
            self._reorder_held is not None
            and direction == self._reorder_held[0]
            and metadata.get("packet_kind")
            == self._reorder_held[1]
            and metadata.get("retransmitted") is not True
        ):
            if match is not None:
                raise OSError(
                    errno.EINVAL,
                    "rendezvous fault targets an active reorder partner",
                )
            (
                held_direction,
                _,
                held_outbound,
                held_target,
                held_payload,
                held_metadata,
                held_fault_index,
            ) = (
                self._reorder_held
            )
            self._reorder_held = None
            self._send_forwarded(
                outbound,
                target,
                outbound_payload,
                direction,
                metadata,
            )
            self._send_forwarded(
                held_outbound,
                held_target,
                held_payload,
                held_direction,
                held_metadata,
            )
            with self._lock:
                event = self._fault_events[held_fault_index]
                if event is not None:
                    event.update(
                        {
                            "reorder_released": True,
                            "reorder_partner_sequence":
                                metadata.get("sequence"),
                            "reorder_partner_retransmitted":
                                metadata.get("retransmitted"),
                            "reorder_partner_forwarded_first": True,
                        }
                    )
            return

        if self._reorder_held is not None and match is not None:
            raise OSError(
                errno.EINVAL,
                "rendezvous fault overlaps an active reorder",
            )
        if match is None:
            self._send_forwarded(
                outbound,
                target,
                outbound_payload,
                direction,
                metadata,
            )
            return

        fault_index, fault = match
        self._record_fault(
            fault_index, fault, direction, metadata
        )
        if fault.action == "drop":
            return
        if fault.action == "duplicate":
            outbound.sendto(outbound_payload, target)
            self._record_forwarded(direction, metadata)
            outbound.sendto(outbound_payload, target)
            return
        if fault.action == "late_duplicate":
            self._send_forwarded(
                outbound,
                target,
                outbound_payload,
                direction,
                metadata,
            )
            self._queue_late_duplicate(
                fault_index,
                fault,
                outbound,
                target,
                outbound_payload,
                direction,
                metadata,
            )
            return
        if fault.action == "delay":
            self._delay_direction = direction
            self._delay_deadline = (
                time.monotonic()
                + fault.delay_milliseconds / 1_000
            )
            self._active_delay_fault_index = fault_index
            self._queue_delayed(
                outbound,
                target,
                outbound_payload,
                direction,
                metadata,
            )
            return
        self._reorder_held = (
            direction,
            fault.packet_kind,
            outbound,
            target,
            outbound_payload,
            dict(metadata),
            fault_index,
        )

    def _run(self) -> None:
        sockets = (self._sender_socket, self._receiver_socket)
        while not self._stop.is_set():
            try:
                self._flush_delay()
                self._flush_late_duplicates()
                timeout = 0.05
                if self._delay_deadline is not None:
                    timeout = min(
                        timeout,
                        max(
                            0.0,
                            self._delay_deadline - time.monotonic(),
                        ),
                    )
                if self._late_duplicates:
                    timeout = min(
                        timeout,
                        max(
                            0.0,
                            min(
                                item[0]
                                for item in self._late_duplicates
                            )
                            - time.monotonic(),
                        ),
                    )
                readable, _, _ = select.select(
                    sockets, (), (), timeout
                )
                for trace_socket in readable:
                    payload, _ = trace_socket.recvfrom(65_535)
                    if trace_socket is self._sender_socket:
                        direction = "sender_to_receiver"
                    else:
                        direction = "receiver_to_sender"
                    self._record(payload, direction)
                    self._record_runtime_key_material(
                        payload, direction
                    )
                    self._forward(payload, direction)
            except OSError as error:
                if error.errno == errno.ECONNREFUSED:
                    continue
                if not self._stop.is_set():
                    self._set_error(error)
                return


class CallerListenerFaultProxy(RendezvousTraceProxy):
    """Relay caller/listener UDP through the shared deterministic fault engine."""

    def __init__(
        self,
        target_port: int,
        fault: (
            RendezvousFault
            | tuple[RendezvousFault, ...]
            | None
        ) = None,
        *,
        host: str = "127.0.0.1",
    ) -> None:
        _HandshakeTraceRecorder.__init__(self)
        address = ipaddress.ip_address(host)
        self._family = (
            socket.AF_INET6 if address.version == 6 else socket.AF_INET
        )
        canonical_host = str(address)
        self._socket: socket.socket | None = None
        self._target = (
            (canonical_host, target_port, 0, 0)
            if self._family == socket.AF_INET6
            else (canonical_host, target_port)
        )
        self._source: (
            tuple[str, int] | tuple[str, int, int, int] | None
        ) = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._started = False
        self._initialize_fault_state(fault)

    @property
    def port(self) -> int:
        if self._socket is None:
            raise RuntimeError("fault relay has not been started")
        return int(self._socket.getsockname()[1])

    def __enter__(self) -> CallerListenerFaultProxy:
        return self

    def __exit__(
        self,
        exception_type: object,
        exception: object,
        traceback: object,
    ) -> None:
        self.close()

    def close(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1)
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def start(self) -> None:
        if self._started:
            raise RuntimeError("fault relay is already started")
        relay_socket = socket.socket(self._family, socket.SOCK_DGRAM)
        try:
            relay_socket.bind((self._target[0], 0))
            relay_socket.settimeout(0.05)
            self._socket = relay_socket
            self._thread = threading.Thread(
                target=self._run, daemon=True
            )
            self._thread.start()
            self._started = True
        except BaseException:
            relay_socket.close()
            self._socket = None
            self._thread = None
            raise

    def _route(
        self, direction: str
    ) -> tuple[
        socket.socket,
        tuple[str, int] | tuple[str, int, int, int],
    ]:
        if self._socket is None:
            raise OSError(errno.EBADF, "fault relay is not started")
        if direction == "sender_to_receiver":
            return self._socket, self._target
        if self._source is None:
            raise OSError(
                errno.EHOSTUNREACH,
                "caller address is not known yet",
            )
        return self._socket, self._source

    def _record_retransmission(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        # SRTT_FILE deliberately clears the wire retransmission flag. Infer a
        # retry from a repeated immutable packet identity so caller/listener
        # profiles retain the same fail-closed ARQ evidence as Rendezvous.
        identity = (
            direction,
            metadata.get("sequence"),
            metadata.get("message_number"),
        )
        with self._lock:
            repeated = identity in self._original_data
        super()._record_retransmission(
            direction,
            {
                **metadata,
                "retransmitted": repeated,
            },
        )
        # Fault matching also supports legacy/clear FileCC probes whose
        # message-number field is zero and cannot enter the identity trace.
        self._match_retransmission(direction, metadata)

    def _run(self) -> None:
        relay_socket = self._socket
        if relay_socket is None:
            return
        while not self._stop.is_set():
            try:
                self._flush_delay()
                timeout = 0.05
                if self._delay_deadline is not None:
                    timeout = min(
                        timeout,
                        max(
                            0.0,
                            self._delay_deadline - time.monotonic(),
                        ),
                    )
                relay_socket.settimeout(timeout)
                payload, address = relay_socket.recvfrom(65_535)
                if address == self._target:
                    direction = "receiver_to_sender"
                else:
                    self._source = address
                    direction = "sender_to_receiver"
                self._record(payload, direction)
                self._record_runtime_key_material(payload, direction)
                self._forward(payload, direction)
            except socket.timeout:
                continue
            except OSError as error:
                if error.errno == errno.ECONNREFUSED:
                    continue
                if not self._stop.is_set():
                    self._set_error(error)
                return


class CallerListenerPeerErrorProxy(CallerListenerFaultProxy):
    """Inject PEERERROR and briefly hold its causal runtime ACK."""

    def __init__(
        self,
        target_port: int,
        error_code: int = 4_000,
        ack_hold_milliseconds: int = PEER_ERROR_ACK_HOLD_MILLISECONDS,
    ) -> None:
        super().__init__(target_port)
        if not 0 <= error_code <= 0xFFFF_FFFF:
            raise ValueError("PEERERROR code must be unsigned 32-bit")
        if not 0 <= ack_hold_milliseconds <= 60_000:
            raise ValueError("PEERERROR ACK hold must be 0..60000 ms")
        self._peer_error_code = error_code
        self._ack_hold_milliseconds = ack_hold_milliseconds
        self._sender_socket_id: int | None = None
        self._first_caller_data_sequence: int | None = None
        self._last_runtime_ack_next_sequence: int | None = None
        self._peer_error_injection: dict[str, object] | None = None

    def injection_observation(self) -> dict[str, object] | None:
        with self._lock:
            return (
                None
                if self._peer_error_injection is None
                else dict(self._peer_error_injection)
            )

    def causal_observation(self) -> dict[str, object]:
        with self._lock:
            return {
                "first_data_sequence": self._first_caller_data_sequence,
                "last_ack_next_sequence": (
                    self._last_runtime_ack_next_sequence
                ),
                "sender_socket_id": self._sender_socket_id,
            }

    def render(self, scenario: str) -> str:
        trace = super().render(scenario)
        causal_event = json.dumps(
            {
                "event": "srt_peer_error_causal_state",
                "scenario": scenario,
                **self.causal_observation(),
            },
            sort_keys=True,
        )
        trace = (
            causal_event
            if trace == "<empty>"
            else "\n".join((trace, causal_event))
        )
        injection = self.injection_observation()
        if injection is None:
            return trace
        event = json.dumps(
            {
                "event": "srt_peer_error_injection",
                "scenario": scenario,
                **injection,
            },
            sort_keys=True,
        )
        return event if trace == "<empty>" else "\n".join((trace, event))

    def _forward(self, payload: bytes, direction: str) -> None:
        if len(payload) >= PACKET_HEADER_SIZE:
            first_word = _u32(payload, 0)
            if (
                direction == "sender_to_receiver"
                and (first_word & 0x8000_0000) == 0
            ):
                if self._first_caller_data_sequence is None:
                    self._first_caller_data_sequence = (
                        first_word & SEQUENCE_MASK
                    )
            elif direction == "receiver_to_sender":
                destination = _u32(payload, 12)
                if destination != 0:
                    self._sender_socket_id = destination
                control_type = (
                    (first_word >> 16) & 0x7FFF
                    if (first_word & 0x8000_0000) != 0
                    else None
                )
                next_sequence = (
                    _u32(payload, PACKET_HEADER_SIZE) & SEQUENCE_MASK
                    if (
                        control_type
                            == ACKNOWLEDGEMENT_CONTROL_TYPE
                        and len(payload) >= PACKET_HEADER_SIZE + 4
                    )
                    else None
                )
                if next_sequence is not None:
                    self._last_runtime_ack_next_sequence = next_sequence
                if (
                    self._first_caller_data_sequence is not None
                    and self._peer_error_injection is None
                    and control_type
                        == ACKNOWLEDGEMENT_CONTROL_TYPE
                    and next_sequence is not None
                    and self._sequence_is_acknowledged(
                        next_sequence,
                        self._first_caller_data_sequence,
                    )
                ):
                    if self._sender_socket_id is None:
                        raise OSError(
                            errno.EPROTO,
                            "runtime ACK has no destination socket ID",
                        )
                    timestamp = _u32(payload, 8)
                    datagram = build_peer_error_datagram(
                        self._sender_socket_id,
                        timestamp,
                        self._peer_error_code,
                    )
                    outbound, target = self._route(
                        "receiver_to_sender"
                    )
                    self._send_datagram(outbound, target, datagram)
                    injected_at = time.monotonic_ns()

                    # Pinned Haivision v1.5.5 records PEERERROR without
                    # waking a blocked sender. Holding the ACK gives its
                    # receive worker a deterministic opportunity to record
                    # the error before that ACK releases the send buffer.
                    # The one-shot hold stays well below its minimum RTO.
                    time.sleep(self._ack_hold_milliseconds / 1_000.0)
                    ack_released_at = time.monotonic_ns()
                    with self._lock:
                        self._peer_error_injection = {
                            "control_type": PEER_ERROR_CONTROL_TYPE,
                            "error_code": self._peer_error_code,
                            "destination_socket_id":
                                self._sender_socket_id,
                            "timestamp": timestamp,
                            "datagram_bytes": len(datagram),
                            "trigger_acknowledgement_number":
                                _u32(payload, 4),
                            "first_data_sequence":
                                self._first_caller_data_sequence,
                            "trigger_ack_next_sequence": next_sequence,
                            "trigger_ack_payload_bytes":
                                len(payload) - PACKET_HEADER_SIZE,
                            "relay_monotonic_ns": injected_at,
                            "ack_hold_milliseconds":
                                self._ack_hold_milliseconds,
                            "ack_release_monotonic_ns": ack_released_at,
                        }
        super()._forward(payload, direction)


class FileRendezvousTraceProxy(RendezvousTraceProxy):
    """Observe FileCC retransmissions whose wire retransmit flag stays clear."""

    def _record_retransmission(
        self,
        direction: str,
        metadata: dict[str, object],
    ) -> None:
        # SRTT_FILE deliberately clears the wire retransmission flag. Infer a
        # retry only from an already-observed (direction, sequence, message)
        # identity, then let the common recorder compare its immutable wire
        # bytes and key selector with the original DATA datagram.
        identity = (
            direction,
            metadata.get("sequence"),
            metadata.get("message_number"),
        )
        with self._lock:
            repeated = identity in self._original_data
        super()._record_retransmission(
            direction,
            {
                **metadata,
                "retransmitted": repeated,
            },
        )
        # Fault matching also supports legacy/clear FileCC probes whose
        # message-number field is zero and therefore cannot participate in
        # the stronger duplicate-identity recorder above.
        self._match_retransmission(direction, metadata)

class CallerListenerSequenceProxy(CallerListenerFaultProxy):
    """Move a caller/listener connection into a chosen sequence namespace."""

    def __init__(
        self,
        target_port: int,
        target_initial_sequence: int,
        fault: (
            RendezvousFault
            | tuple[RendezvousFault, ...]
            | None
        ) = None,
    ) -> None:
        if not 0 <= target_initial_sequence <= SEQUENCE_MASK:
            raise ValueError(
                "target initial sequence must be a 31-bit value"
            )
        super().__init__(target_port, fault)
        self._target_initial_sequence = target_initial_sequence
        self._source_initial_sequence: int | None = None
        self._translation_delta: int | None = None
        self._translated_data_packets = 0
        self._translated_acknowledgements = 0
        self._first_translated_sequence: int | None = None
        self._last_translated_sequence: int | None = None
        self._data_wrap_observed = False
        self._ack_wrap_observed = False

    def sequence_observation(self) -> dict[str, object]:
        with self._lock:
            return {
                "source_initial_sequence":
                    self._source_initial_sequence,
                "target_initial_sequence":
                    self._target_initial_sequence,
                "translation_delta": self._translation_delta,
                "translated_data_packets":
                    self._translated_data_packets,
                "translated_acknowledgements":
                    self._translated_acknowledgements,
                "first_translated_sequence":
                    self._first_translated_sequence,
                "last_translated_sequence":
                    self._last_translated_sequence,
                "data_wrap_observed": self._data_wrap_observed,
                "ack_wrap_observed": self._ack_wrap_observed,
            }

    def render(self, scenario: str) -> str:
        trace = super().render(scenario)
        translation = json.dumps(
            {
                "event": "srt_sequence_translation",
                "scenario": scenario,
                **self.sequence_observation(),
            },
            sort_keys=True,
        )
        if trace == "<empty>":
            return translation
        return "\n".join((trace, translation))

    def _translated_payload(
        self, payload: bytes, direction: str
    ) -> bytes:
        if len(payload) >= HANDSHAKE_DATAGRAM_SIZE:
            first_word = _u32(payload, 0)
            control_type = (first_word >> 16) & 0x7FFF
            if (
                direction == "sender_to_receiver"
                and (first_word & 0x8000_0000) != 0
                and control_type == HANDSHAKE_CONTROL_TYPE
            ):
                source = _u32(payload, 24) & SEQUENCE_MASK
                if self._source_initial_sequence is None:
                    self._source_initial_sequence = source
                    self._translation_delta = (
                        self._target_initial_sequence - source
                    ) & SEQUENCE_MASK
                elif source != self._source_initial_sequence:
                    raise OSError(
                        errno.EPROTO,
                        "caller changed its initial sequence "
                        "during handshake",
                    )
        if self._translation_delta is None:
            return payload
        delta = (
            self._translation_delta
            if direction == "sender_to_receiver"
            else -self._translation_delta
        )
        return translate_sequence_datagram(payload, delta)

    def _observe_fault_view(
        self, payload: bytes, direction: str
    ) -> None:
        if len(payload) < PACKET_HEADER_SIZE:
            return
        first_word = _u32(payload, 0)
        if (
            direction == "sender_to_receiver"
            and (first_word & 0x8000_0000) == 0
        ):
            sequence = first_word & SEQUENCE_MASK
            with self._lock:
                self._translated_data_packets += 1
                if self._first_translated_sequence is None:
                    self._first_translated_sequence = sequence
                self._last_translated_sequence = sequence
                if sequence < self._target_initial_sequence:
                    self._data_wrap_observed = True
            return
        control_type = (first_word >> 16) & 0x7FFF
        if (
            direction == "receiver_to_sender"
            and (first_word & 0x8000_0000) != 0
            and control_type == ACKNOWLEDGEMENT_CONTROL_TYPE
            and len(payload) >= PACKET_HEADER_SIZE + 4
        ):
            next_sequence = _u32(
                payload, PACKET_HEADER_SIZE
            ) & SEQUENCE_MASK
            with self._lock:
                self._translated_acknowledgements += 1
                if next_sequence < self._target_initial_sequence:
                    self._ack_wrap_observed = True

    def _fault_view(self, payload: bytes, direction: str) -> bytes:
        translated = (
            self._translated_payload(payload, direction)
            if direction == "sender_to_receiver"
            else payload
        )
        self._observe_fault_view(translated, direction)
        return translated

    def _outbound_payload(self, payload: bytes, direction: str) -> bytes:
        return self._translated_payload(payload, direction)
