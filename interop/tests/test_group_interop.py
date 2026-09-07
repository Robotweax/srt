from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_group_interop  # noqa: E402


class GroupInteropUnitTests(unittest.TestCase):
    def test_invalid_control_size_diagnostic_is_always_fatal(self) -> None:
        self.assertTrue(
            run_group_interop.has_invalid_control_size_diagnostic(
                "SRT.cn: INVALID SIZE: expected at least one word"
            )
        )
        self.assertTrue(
            run_group_interop.has_invalid_control_size_diagnostic(
                "invalid size while decoding control packet"
            )
        )
        self.assertFalse(
            run_group_interop.has_invalid_control_size_diagnostic(
                "OPTION: #35 UNKNOWN"
            )
        )

    def test_pinned_listener_admission_requires_the_complete_suffix(
        self,
    ) -> None:
        self.assertEqual(
            run_group_interop.replacement_admission_message_count(False), 1
        )
        self.assertEqual(
            run_group_interop.replacement_admission_message_count(True),
            run_group_interop.REPLACEMENT_SUFFIX_MESSAGE_COUNT,
        )

    def test_replacement_admission_wait_requires_forwarded_data(self) -> None:
        relay = Mock()
        relay.forwarded_data_observation.side_effect = (
            {"count": 0, "acknowledged": False},
            {
                "count": 1,
                "first_sequence": 42,
                "acknowledged": False,
            },
        )
        caller = Mock()
        listener = Mock()
        caller.poll.return_value = None
        listener.poll.return_value = None

        observation = run_group_interop.wait_for_forwarded_data(
            relay, (caller, listener), 1.0
        )

        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["first_sequence"], 42)
        self.assertEqual(relay.forwarded_data_observation.call_count, 2)

    def test_replacement_admission_wait_stops_when_an_endpoint_exits(
        self,
    ) -> None:
        relay = Mock()
        relay.forwarded_data_observation.return_value = {
            "count": 0,
            "acknowledged": False,
        }
        caller = Mock()
        listener = Mock()
        caller.poll.return_value = None
        listener.poll.return_value = 6

        self.assertIsNone(
            run_group_interop.wait_for_forwarded_data(
                relay, (caller, listener), 1.0
            )
        )

    def test_replacement_admission_wait_can_require_the_complete_suffix(
        self,
    ) -> None:
        relay = Mock()
        relay.forwarded_data_observation.side_effect = (
            {"count": 1, "acknowledged": False},
            {"count": 13, "acknowledged": False},
            {"count": 14, "acknowledged": False},
        )
        caller = Mock()
        caller.poll.return_value = None

        observation = run_group_interop.wait_for_forwarded_data(
            relay, (caller,), 1.0, minimum_count=14
        )

        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["count"], 14)
        self.assertEqual(relay.forwarded_data_observation.call_count, 3)

    def test_replacement_payload_wait_requires_forward_and_ack(self) -> None:
        relay = Mock()
        relay.forwarded_data_observation.side_effect = (
            {"count": 0, "acknowledged": False},
            {
                "count": 1,
                "first_sequence": 42,
                "acknowledged": False,
            },
            {
                "count": 14,
                "first_sequence": 42,
                "acknowledged": True,
            },
        )
        caller = Mock()
        listener = Mock()
        caller.poll.return_value = None
        listener.poll.return_value = None

        observation = (
            run_group_interop.wait_for_acknowledged_forwarded_data(
                relay, (caller, listener), 1.0
            )
        )

        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["first_sequence"], 42)
        self.assertEqual(
            relay.forwarded_data_observation.call_count, 3
        )

    def test_replacement_payload_wait_stops_when_an_endpoint_exits(
        self,
    ) -> None:
        relay = Mock()
        relay.forwarded_data_observation.return_value = {
            "count": 0,
            "acknowledged": False,
        }
        caller = Mock()
        listener = Mock()
        caller.poll.return_value = 5
        listener.poll.return_value = None

        self.assertIsNone(
            run_group_interop.wait_for_acknowledged_forwarded_data(
                relay, (caller, listener), 1.0
            )
        )

    def test_replacement_final_evidence_still_requires_ack(self) -> None:
        self.assertTrue(
            run_group_interop.has_acknowledged_forwarded_data(
                {"count": 14, "acknowledged": True}
            )
        )
        self.assertFalse(
            run_group_interop.has_acknowledged_forwarded_data(
                {"count": 14, "acknowledged": False}
            )
        )
        self.assertFalse(
            run_group_interop.has_acknowledged_forwarded_data(
                {"count": 0, "acknowledged": True}
            )
        )

    def test_pinned_listener_replacement_failure_is_strictly_classified(
        self,
    ) -> None:
        observation = {
            "count": 14,
            "acknowledged": False,
            "first_sequence": 0x7FFF_FFF8,
            "last_sequence": 5,
            "first_message_number": 3,
            "last_message_number": 16,
            "first_forwarded_monotonic_ns": 1_000,
            "last_forwarded_monotonic_ns": 2_000,
            "first_relay_ordinal": 5,
            "destination_socket_id": 77,
            "destination_socket_id_consistent": True,
            "sequences_contiguous": True,
            "message_numbers_contiguous": True,
        }
        injection = {
            "control_type": 8,
            "error_code": 4_000,
            "datagram_bytes": 20,
            "destination_socket_id": 102,
            "ack_hold_milliseconds": 25,
            "first_data_sequence": 101,
            "trigger_ack_next_sequence": 102,
            "relay_monotonic_ns": 1_000,
            "ack_release_monotonic_ns": 25_001_000,
        }
        caller_output = "\n".join(
            (
                '{"event":"peer_error_sender_ready","role":"caller",'
                '"healthy_member":101,"failed_member":102}',
                '{"event":"peer_error_barrier","role":"caller"}',
                '{"event":"peer_error_replacement_ready",' '"role":"caller"}',
            )
        )
        listener_output = "\n".join(
            (
                '{"event":"ready"}',
                '{"event":"peer_error_receiver_ready",' '"role":"listener"}',
                '{"event":"peer_error_replacement_ready",' '"role":"listener"}',
            )
        )
        listener_error = "\n".join(
            (
                "12:00:00/SRT:RcvQ:w1*E:SRT.ac: OPTION: #35 UNKNOWN",
                run_group_interop.PINNED_REPLACEMENT_RECEIVE_ERROR,
            )
        )

        def classified(
            candidate_observation: dict[str, object] = observation,
            candidate_injection: dict[str, object] = injection,
            candidate_listener_error: str = listener_error,
            caller_alive: bool = True,
            listener_returncode: int = 6,
            retransmissions: int = 0,
        ) -> bool:
            return run_group_interop.has_expected_pinned_listener_replacement_failure(
                candidate_observation,
                77,
                candidate_injection,
                caller_output,
                "",
                listener_output,
                candidate_listener_error,
                caller_alive,
                listener_returncode,
                retransmissions,
            )

        self.assertTrue(classified())
        dropped_diagnostic = (
            "12:00:00/SRT:TsbPd!W:SRT.br: @77: RCV-DROPPED "
            "1 packet(s). Packet seqno %2147483640 delayed for 242.801 ms"
        )
        listener_error_with_drop = listener_error.replace(
            run_group_interop.PINNED_REPLACEMENT_RECEIVE_ERROR,
            dropped_diagnostic
            + "\n"
            + run_group_interop.PINNED_REPLACEMENT_RECEIVE_ERROR,
        )
        self.assertTrue(
            classified(candidate_listener_error=listener_error_with_drop)
        )
        self.assertFalse(
            classified(
                candidate_listener_error=listener_error_with_drop.replace(
                    "%2147483640", "%2147483641"
                )
            )
        )
        self.assertFalse(
            classified(
                candidate_listener_error=listener_error_with_drop.replace(
                    "@77", "@78"
                )
            )
        )
        self.assertFalse(
            classified(
                candidate_listener_error=listener_error_with_drop.replace(
                    "RCV-DROPPED 1 packet(s)",
                    "RCV-DROPPED 2 packet(s)",
                )
            )
        )
        self.assertFalse(
            classified(
                candidate_listener_error=listener_error_with_drop.replace(
                    dropped_diagnostic,
                    dropped_diagnostic + "\n" + dropped_diagnostic,
                )
            )
        )
        acknowledged_observation = {
            **observation,
            "acknowledged": True,
            "last_relay_ordinal": 18,
            "acknowledgement_next_sequence": 0x7FFF_FFF9,
            "acknowledgement_relay_ordinal": 9,
            "acknowledgement_monotonic_ns": 1_001,
            "acknowledgement_forwarded_data_count": 1,
        }
        self.assertTrue(classified(acknowledged_observation))
        self.assertFalse(
            classified(
                {
                    **acknowledged_observation,
                    "acknowledgement_relay_ordinal": 5,
                }
            )
        )
        self.assertFalse(
            classified(
                {
                    **acknowledged_observation,
                    "acknowledgement_monotonic_ns": 999,
                }
            )
        )
        self.assertFalse(
            classified(
                {
                    **acknowledged_observation,
                    "acknowledgement_next_sequence": 6,
                }
            )
        )
        self.assertFalse(
            classified(
                {
                    **acknowledged_observation,
                    "acknowledgement_forwarded_data_count": 999,
                }
            )
        )
        for invalid_observation in (
            {**observation, "count": 13},
            {**observation, "count": 15},
            {**observation, "acknowledged": True},
            {**observation, "last_sequence": 6},
            {**observation, "last_message_number": 15},
            {**observation, "sequences_contiguous": False},
            {**observation, "message_numbers_contiguous": False},
            {**observation, "destination_socket_id": 78},
            {**observation, "destination_socket_id_consistent": False},
            {**observation, "acknowledgement_next_sequence": 6},
            {
                **observation,
                "acknowledgement_forwarded_data_count": 1,
            },
        ):
            with self.subTest(observation=invalid_observation):
                self.assertFalse(classified(invalid_observation))
        self.assertFalse(classified(caller_alive=False))
        self.assertFalse(classified(listener_returncode=5))
        self.assertFalse(classified(retransmissions=1))
        self.assertFalse(
            classified({**observation}, {**injection, "error_code": 4_001})
        )
        self.assertFalse(
            run_group_interop.has_expected_pinned_listener_replacement_failure(
                observation,
                78,
                injection,
                caller_output,
                "",
                listener_output,
                listener_error,
                True,
                6,
                0,
            )
        )
        self.assertFalse(
            classified(
                {**observation},
                {**injection, "destination_socket_id": 103},
            )
        )
        self.assertFalse(
            classified(
                {**observation},
                {**injection, "trigger_ack_next_sequence": 103},
            )
        )
        self.assertFalse(
            classified(
                candidate_listener_error=(
                    "unexpected earlier error\n" + listener_error
                )
            )
        )
        self.assertFalse(
            classified(
                candidate_listener_error=(
                    listener_error + "\nunexpected trailing error"
                )
            )
        )

    def test_pinned_caller_replacement_failure_is_strictly_classified(
        self,
    ) -> None:
        observation = {"count": 0, "acknowledged": False}
        injection = {
            "control_type": 8,
            "error_code": 4_000,
            "datagram_bytes": 20,
            "destination_socket_id": 102,
            "ack_hold_milliseconds": 25,
            "first_data_sequence": 101,
            "trigger_ack_next_sequence": 102,
            "relay_monotonic_ns": 1_000,
            "ack_release_monotonic_ns": 25_001_000,
        }
        caller_output = "\n".join(
            (
                '{"event":"peer_error_sender_ready","role":"caller",'
                '"healthy_member":101,"failed_member":102}',
                '{"event":"peer_error_barrier","role":"caller"}',
                '{"event":"peer_error_replacement_unavailable",'
                '"role":"caller",'
                '"post_error_send_succeeded":true,'
                '"peer_error_group_update":false,'
                '"peer_error_member_isolated":false,'
                '"replacement_distinct":false,'
                '"replacement_connected":false,'
                '"group_state":5,"failed_state":5,'
                '"healthy_state":5,"replacement_state":9}',
            )
        )
        listener_output = "\n".join(
            (
                '{"event":"ready"}',
                '{"event":"peer_error_receiver_ready",' '"role":"listener"}',
            )
        )
        caller_error = run_group_interop.PINNED_REPLACEMENT_CALLER_ERRORS[0]

        def classified(
            candidate_observation: dict[str, object] = observation,
            candidate_injection: dict[str, object] = injection,
            candidate_output: str = caller_output,
            candidate_error: str = caller_error,
            caller_returncode: int = 5,
            listener_returncode: int | None = None,
            retransmissions: int = 0,
        ) -> bool:
            return run_group_interop.has_expected_pinned_caller_replacement_failure(
                candidate_observation,
                candidate_injection,
                candidate_output,
                candidate_error,
                listener_output,
                "",
                caller_returncode,
                listener_returncode,
                retransmissions,
        )

        self.assertTrue(classified())
        self.assertFalse(classified({"count": 1, "acknowledged": False}))
        self.assertFalse(classified({"count": 0, "acknowledged": True}))
        self.assertFalse(classified({"count": 0.0, "acknowledged": False}))
        self.assertFalse(
            classified(
                {
                    "count": 0,
                    "acknowledged": False,
                    "first_message_number": 3,
                }
            )
        )
        self.assertFalse(classified(caller_returncode=6))
        self.assertFalse(classified(listener_returncode=6))
        self.assertFalse(classified(retransmissions=1))
        self.assertFalse(
            classified(candidate_injection={**injection, "control_type": 7})
        )
        self.assertFalse(
            classified(
                candidate_injection={
                    **injection,
                    "destination_socket_id": 101,
                }
            )
        )
        self.assertFalse(
            classified(
                candidate_output=caller_output.replace(
                    '"peer_error_group_update":false',
                    '"peer_error_group_update":true',
                )
            )
        )
        self.assertFalse(classified(candidate_error=caller_error + "\nunexpected"))
        self.assertFalse(
            classified(
                candidate_error=(
                    run_group_interop.PINNED_REPLACEMENT_CALLER_ERROR_PREFIX
                    + "Success"
                    + run_group_interop.PINNED_REPLACEMENT_CALLER_STATE_SUFFIX
                )
            )
        )
        self.assertFalse(
            classified(
                candidate_error=(
                    run_group_interop.PINNED_REPLACEMENT_CALLER_ERROR_PREFIX
                    + "unexpected detail"
                    + run_group_interop.PINNED_REPLACEMENT_CALLER_STATE_SUFFIX
                )
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_pinned_caller_replacement_failure(
                observation,
                injection,
                caller_output,
                caller_error,
                listener_output,
                run_group_interop.EXPECTED_REVERSE_LISTENER_TIMEOUT,
                5,
                6,
                0,
            )
        )
        self.assertFalse(
            run_group_interop.has_expected_pinned_caller_replacement_failure(
                observation,
                injection,
                caller_output,
                caller_error,
                listener_output,
                "unexpected listener error",
                5,
                6,
                0,
            )
        )

    def test_completion_barrier_releases_a_running_listener(self) -> None:
        process = Mock()
        process.poll.return_value = None

        run_group_interop.release_completion_barrier(process)

        process.stdin.write.assert_called_once_with("continue\n")
        process.stdin.flush.assert_called_once_with()
        process.stdin.close.assert_called_once_with()

    def test_completion_barrier_ignores_a_finished_listener(self) -> None:
        process = Mock()
        process.poll.return_value = 0

        run_group_interop.release_completion_barrier(process)

        process.stdin.write.assert_not_called()

    def test_event_parser_requires_the_requested_role(self) -> None:
        output = "\n".join(
            (
                "diagnostic text",
                '{"event":"ready"}',
                '{"event":"complete","role":"listener"}',
            )
        )
        self.assertTrue(run_group_interop.has_event(output, "ready"))
        self.assertTrue(
            run_group_interop.has_event(output, "complete", "listener")
        )
        self.assertFalse(
            run_group_interop.has_event(output, "complete", "caller")
        )

    def test_baseline_payload_barrier_requires_both_endpoint_roles(
        self,
    ) -> None:
        caller = '{"event":"payload_sender_ready","role":"caller"}'
        listener = (
            '{"event":"payload_receiver_ready","role":"listener"}'
        )
        self.assertTrue(
            run_group_interop.has_baseline_payload_barrier(caller, listener)
        )
        self.assertFalse(
            run_group_interop.has_baseline_payload_barrier("", listener)
        )
        self.assertFalse(
            run_group_interop.has_baseline_payload_barrier(
                caller,
                listener.replace('"listener"', '"caller"'),
            )
        )

    def test_malformed_json_is_ignored(self) -> None:
        self.assertFalse(
            run_group_interop.has_event("{not-json}", "complete")
        )

    def test_group_callback_requires_expected_token_matched_events(self) -> None:
        baseline = (
            '{"event":"complete","role":"caller",'
            '"callback_calls":2,"callback_valid":true,'
            '"tokens":[4001,4002]}'
        )
        self.assertTrue(
            run_group_interop.has_valid_group_callback(
                baseline, [4001, 4002]
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_group_callback(
                baseline.replace(
                    '"callback_calls":2', '"callback_calls":1'
                ),
                [4001, 4002],
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_group_callback(
                baseline.replace(
                    '"callback_valid":true', '"callback_valid":false'
                ),
                [4001, 4002],
            )
        )
        late_join = (
            '{"event":"complete","role":"caller",'
            '"callback_calls":3,"callback_valid":true,'
            '"tokens":[4001,4002,4003]}'
        )
        self.assertTrue(
            run_group_interop.has_valid_group_callback(
                late_join, [4001, 4002, 4003]
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_group_callback(
                late_join, [4001, 4002]
            )
        )

    def test_listener_completion_requires_update_evidence(self) -> None:
        valid = (
            '{"event":"complete","role":"listener",'
            '"members":2,"listener_update":true}'
        )
        self.assertTrue(run_group_interop.has_listener_update(valid))
        self.assertFalse(
            run_group_interop.has_listener_update(
                valid.replace('"listener_update":true',
                              '"listener_update":false')
            )
        )
        self.assertFalse(
            run_group_interop.has_listener_update(
                '{"event":"complete","role":"caller",'
                '"listener_update":true}'
            )
        )

    def test_late_listener_update_is_independently_reported(self) -> None:
        valid = (
            '{"event":"complete","role":"listener",'
            '"listener_update":true,"initial_listener_update":true,'
            '"late_listener_update":true}'
        )
        self.assertTrue(run_group_interop.has_initial_listener_update(valid))
        self.assertTrue(run_group_interop.has_late_listener_update(valid))
        self.assertFalse(
            run_group_interop.has_late_listener_update(
                valid.replace('"late_listener_update":true',
                              '"late_listener_update":false')
            )
        )
        self.assertFalse(
            run_group_interop.has_late_listener_update(
                '{"event":"complete","role":"caller",'
                '"late_listener_update":true}'
            )
        )
        reference_limited = valid.replace(
            '"late_listener_update":true',
            '"late_listener_update":false',
        )
        self.assertTrue(
            run_group_interop.has_required_listener_updates(
                reference_limited,
                late_join=True,
                require_repeated=False,
            )
        )
        self.assertFalse(
            run_group_interop.has_required_listener_updates(
                reference_limited,
                late_join=True,
                require_repeated=True,
            )
        )
        self.assertTrue(
            run_group_interop.has_required_listener_updates(
                valid,
                late_join=True,
                require_repeated=True,
            )
        )
        self.assertFalse(
            run_group_interop.has_required_listener_updates(
                valid.replace('"initial_listener_update":true',
                              '"initial_listener_update":false'),
                late_join=True,
                require_repeated=False,
            )
        )

    def test_bonded_listener_requires_two_listener_marker(self) -> None:
        valid = (
            '{"event":"complete","role":"listener",'
            '"bonded_listeners":2}'
        )
        self.assertTrue(run_group_interop.has_bonded_listener(valid))
        self.assertFalse(
            run_group_interop.has_bonded_listener(
                valid.replace('"bonded_listeners":2',
                              '"bonded_listeners":1')
            )
        )

    def test_payload_marker_is_role_specific(self) -> None:
        valid = (
            '{"event":"complete","role":"listener",'
            '"payload_valid":true}'
        )
        self.assertTrue(
            run_group_interop.has_valid_payload(valid, "listener")
        )
        self.assertFalse(
            run_group_interop.has_valid_payload(valid, "caller")
        )
        self.assertFalse(
            run_group_interop.has_valid_payload(
                valid.replace('"payload_valid":true',
                              '"payload_valid":false'),
                "listener",
            )
        )

    def test_receive_contract_requires_exact_public_errors_and_timing(
        self,
    ) -> None:
        valid = json.dumps(
            {
                "event": "receive_contract",
                "role": "listener",
                "matched": True,
                "profile": "strict",
                "nonblocking_result": -1,
                "nonblocking_error": 6_002,
                "nonblocking_elapsed_us": 125,
                "timeout_result": -1,
                "timeout_error": 6_003,
                "timeout_elapsed_us": 150_250,
                "timeout_configured_ms": 150,
            }
        )
        self.assertTrue(
            run_group_interop.has_valid_receive_contract(
                valid, pinned_reference=False
            )
        )
        for field, replacement in (
            ("matched", False),
            ("nonblocking_error", 6_003),
            ("nonblocking_elapsed_us", 500_000),
            ("timeout_error", 6_002),
            ("timeout_elapsed_us", 49_999),
            ("timeout_configured_ms", 151),
        ):
            with self.subTest(field=field):
                invalid = json.loads(valid)
                invalid[field] = replacement
                self.assertFalse(
                    run_group_interop.has_valid_receive_contract(
                        json.dumps(invalid), pinned_reference=False
                    )
                )

        reference = json.loads(valid)
        reference["profile"] = "reference"
        reference["timeout_error"] = 6_002
        self.assertTrue(
            run_group_interop.has_valid_receive_contract(
                json.dumps(reference), pinned_reference=True
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_receive_contract(
                json.dumps(reference), pinned_reference=False
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_receive_contract(
                valid, pinned_reference=True
            )
        )

    def test_late_join_marker_is_role_specific(self) -> None:
        valid = (
            '{"event":"complete","role":"caller",'
            '"late_join":true}'
        )
        self.assertTrue(run_group_interop.has_late_join(valid, "caller"))
        self.assertFalse(
            run_group_interop.has_late_join(valid, "listener")
        )
        self.assertFalse(
            run_group_interop.has_late_join(
                valid.replace('"late_join":true', '"late_join":false'),
                "caller",
            )
        )

    def test_member_count_is_role_specific(self) -> None:
        valid = (
            '{"event":"complete","role":"listener",'
            '"members":3}'
        )
        self.assertTrue(
            run_group_interop.has_member_count(valid, "listener", 3)
        )
        self.assertFalse(
            run_group_interop.has_member_count(valid, "listener", 2)
        )
        self.assertFalse(
            run_group_interop.has_member_count(valid, "caller", 3)
        )

    def test_peer_metadata_requires_version_and_listener_callback(self) -> None:
        reference_version = run_group_interop.PINNED_SRT_VERSION
        caller = (
            '{"event":"complete","role":"caller",'
            f'"peer_version":{reference_version},'
            '"peer_version_valid":true}'
        )
        listener = (
            '{"event":"complete","role":"listener",'
            f'"peer_version":{reference_version},'
            '"peer_version_valid":true,'
            '"listener_callback_calls":2,'
            '"listener_metadata_valid":true}'
        )
        self.assertTrue(
            run_group_interop.has_valid_peer_metadata(caller, "caller", 2)
        )
        self.assertTrue(
            run_group_interop.has_valid_peer_metadata(
                listener, "listener", 2
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_metadata(
                listener.replace('"listener_callback_calls":2',
                                 '"listener_callback_calls":1'),
                "listener",
                2,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_metadata(
                caller.replace(str(reference_version), "0"), "caller", 2
            )
        )

    def test_late_join_profile_rejects_bonded_composition(self) -> None:
        with self.assertRaisesRegex(ValueError, "cannot be combined"):
            run_group_interop.run_case(
                Path("listener"),
                Path("caller"),
                "invalid",
                "broadcast",
                False,
                bonded=True,
                profile="late-join",
            )

    def test_group_peer_error_requires_causal_member_evidence(self) -> None:
        caller = {
            "payload_valid": True,
            "callback_valid": True,
            "peer_error": True,
            "peer_error_replacement": False,
            "peer_error_member_isolated": True,
            "peer_error_group_update": True,
            "group_connected": True,
            "members": 2,
            "peer_version_valid": True,
            "healthy_member": 101,
            "failed_member": 102,
        }
        listener = {
            "payload_valid": True,
            "peer_error": True,
            "peer_error_replacement": False,
            "replacement_attached": False,
            "transient_group_receive_errors": 0,
            "group_connected": True,
            "bonded_listeners": 2,
            "listener_callback_calls": 2,
            "listener_metadata_valid": True,
            "peer_version": run_group_interop.PINNED_SRT_VERSION,
            "peer_version_valid": True,
        }
        injection = {
            "control_type": 8,
            "error_code": 4_000,
            "datagram_bytes": 20,
            "destination_socket_id": 102,
            "ack_hold_milliseconds": 25,
            "relay_monotonic_ns": 1_000,
            "ack_release_monotonic_ns": 25_001_000,
        }
        self.assertTrue(
            run_group_interop.has_valid_peer_error_evidence(
                caller, listener, injection
            )
        )
        reference_post_failure = {
            **listener,
            "peer_version": 0,
            "peer_version_valid": False,
        }
        self.assertTrue(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                reference_post_failure,
                injection,
                reference_listener=True,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                {**listener, "peer_version_valid": False},
                injection,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                listener,
                {**injection, "destination_socket_id": 101},
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                {**caller, "group_connected": False},
                listener,
                injection,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                listener,
                {**injection, "ack_release_monotonic_ns": 24_000_000},
            )
        )

    def test_group_peer_error_replacement_requires_distinct_member(self) -> None:
        caller = {
            "payload_valid": True,
            "callback_valid": True,
            "peer_error": True,
            "peer_error_replacement": True,
            "peer_error_member_isolated": True,
            "peer_error_group_update": True,
            "replacement_member": 103,
            "replacement_distinct": True,
            "replacement_group_update_absent": True,
            "replacement_connected": True,
            "group_connected": True,
            "members": 2,
            "peer_version_valid": True,
            "healthy_member": 101,
            "failed_member": 102,
        }
        listener = {
            "payload_valid": True,
            "peer_error": True,
            "peer_error_replacement": True,
            "replacement_attached": True,
            "transient_group_receive_errors": 1,
            "group_connected": True,
            "bonded_listeners": 2,
            "listener_callback_calls": 3,
            "listener_metadata_valid": True,
            "peer_version": 0,
            "peer_version_valid": False,
        }
        injection = {
            "control_type": 8,
            "error_code": 4_000,
            "datagram_bytes": 20,
            "destination_socket_id": 102,
            "ack_hold_milliseconds": 25,
            "relay_monotonic_ns": 1_000,
            "ack_release_monotonic_ns": 25_001_000,
        }
        self.assertTrue(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                listener,
                injection,
                replacement=True,
                reference_listener=True,
            )
        )
        robotweax_listener = {
            **listener,
            "transient_group_receive_errors": 0,
            "peer_version": run_group_interop.PINNED_SRT_VERSION,
            "peer_version_valid": True,
        }
        self.assertTrue(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                robotweax_listener,
                injection,
                replacement=True,
                reference_listener=False,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                listener,
                injection,
                replacement=True,
                reference_listener=False,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                {
                    **robotweax_listener,
                    "transient_group_receive_errors": 1,
                },
                injection,
                replacement=True,
                reference_listener=False,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                {**caller, "replacement_member": 102},
                listener,
                injection,
                replacement=True,
                reference_listener=True,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                {**caller, "replacement_group_update_absent": False},
                listener,
                injection,
                replacement=True,
                reference_listener=True,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                {**listener, "listener_callback_calls": 2},
                injection,
                replacement=True,
                reference_listener=True,
            )
        )
        self.assertFalse(
            run_group_interop.has_valid_peer_error_evidence(
                caller,
                {**listener, "transient_group_receive_errors": -1},
                injection,
                replacement=True,
                reference_listener=True,
            )
        )

    @staticmethod
    def _path_outage_data(
        first_message: int, last_message: int, destination: int
    ) -> tuple[dict[str, object], ...]:
        return tuple(
            {
                "direction": "caller_to_listener",
                "relay_ordinal": message * 2,
                "sequence": 500 + message - 1,
                "message_number": message,
                "packet_position": 3,
                "in_order": False,
                "key_selection": 0,
                "retransmitted": False,
                "timestamp": message * 20_000,
                "destination_socket_id": destination,
                "payload_bytes": 1_316 if message % 2 == 0 else 188,
                "ciphertext_sha256": f"{message:064x}",
            }
            for message in range(first_message, last_message + 1)
        )

    @staticmethod
    def _path_outage_observation() -> dict[str, object]:
        return {
            "trigger_original_data_count": 4,
            "primary_original_data_count": 4,
            "dropped_primary_datagrams": 9,
            "dropped_primary_data_datagrams": 4,
            "outage_started_monotonic_ns": 1_000_000,
            "outage_sequence": 503,
            "first_backup_data_monotonic_ns": 1_060_000,
            "first_backup_sequence": 503,
            "first_backup_data_delay_microseconds": 60,
        }

    def test_path_outage_wire_evidence_is_fail_closed(self) -> None:
        primary = self._path_outage_data(1, 10, 701)
        backup = self._path_outage_data(4, 16, 702)
        outage = self._path_outage_observation()
        validate = run_group_interop.has_valid_path_outage_wire_evidence
        self.assertTrue(
            validate(
                outage,
                primary,
                backup,
                701,
                702,
                reference_sender=False,
            )
        )
        reference_outage = {
            key: value
            for key, value in outage.items()
            if not key.startswith("first_backup_")
        }
        reference_outage["dropped_primary_data_datagrams"] = 13
        reference_outage["dropped_primary_datagrams"] = 20
        self.assertFalse(
            validate(
                outage,
                primary,
                backup[:-1],
                701,
                702,
                reference_sender=False,
            )
        )
        self.assertTrue(
            validate(
                reference_outage,
                self._path_outage_data(1, 16, 701),
                (),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
            )
        )
        reference_outage_queued_tail = {
            **reference_outage,
            "dropped_primary_data_datagrams": 12,
        }
        self.assertTrue(
            validate(
                reference_outage_queued_tail,
                self._path_outage_data(1, 15, 701),
                (),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
            )
        )
        self.assertFalse(
            validate(
                reference_outage_queued_tail,
                self._path_outage_data(1, 14, 701),
                (),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
            )
        )
        self.assertFalse(
            validate(
                reference_outage,
                self._path_outage_data(1, 15, 701),
                (),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
            )
        )
        self.assertFalse(
            validate(
                reference_outage,
                self._path_outage_data(1, 16, 701),
                backup[:1],
                701,
                702,
                reference_sender=True,
                reference_sender_variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
            )
        )
        scheduler_backup = list(
            copy.deepcopy(self._path_outage_data(16, 16, 702))
        )
        scheduler_backup[0]["sequence"] = 537
        scheduler_outage = {
            **reference_outage,
            "first_backup_data_monotonic_ns": 1_240_000,
            "first_backup_sequence": 537,
            "first_backup_data_delay_microseconds": 240,
        }
        scheduler_variant = (
            run_group_interop.PATH_OUTAGE_REFERENCE_SCHEDULER_COLLAPSE
        )
        self.assertTrue(
            validate(
                scheduler_outage,
                self._path_outage_data(1, 16, 701),
                tuple(scheduler_backup),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=scheduler_variant,
            )
        )
        self.assertFalse(
            validate(
                scheduler_outage,
                self._path_outage_data(1, 16, 701),
                tuple(scheduler_backup),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
            )
        )
        contiguous_backup = copy.deepcopy(scheduler_backup)
        contiguous_backup[0]["sequence"] = 503
        self.assertFalse(
            validate(
                {**scheduler_outage, "first_backup_sequence": 503},
                self._path_outage_data(1, 16, 701),
                tuple(contiguous_backup),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=scheduler_variant,
            )
        )
        wrong_payload_backup = copy.deepcopy(scheduler_backup)
        wrong_payload_backup[0]["ciphertext_sha256"] = "f" * 64
        self.assertFalse(
            validate(
                scheduler_outage,
                self._path_outage_data(1, 16, 701),
                tuple(wrong_payload_backup),
                701,
                702,
                reference_sender=True,
                reference_sender_variant=scheduler_variant,
            )
        )
        for mutation in (
            {**outage, "outage_sequence": 504},
            {**outage, "unexpected": 1},
            {**outage, "dropped_primary_data_datagrams": 0},
        ):
            self.assertFalse(
                validate(
                    mutation,
                    primary,
                    backup,
                    701,
                    702,
                    reference_sender=False,
                )
            )
        bad_backup = list(copy.deepcopy(backup))
        bad_backup[2]["destination_socket_id"] = 999
        self.assertFalse(
            validate(
                outage,
                primary,
                tuple(bad_backup),
                701,
                702,
                reference_sender=False,
            )
        )
        bad_backup = list(copy.deepcopy(backup))
        bad_backup[2]["message_number"] = 99
        self.assertFalse(
            validate(
                outage,
                primary,
                tuple(bad_backup),
                701,
                702,
                reference_sender=False,
            )
        )

    @staticmethod
    def _sender_output(
        reference_sender: bool, *, scheduler_collapse: bool = False
    ) -> str:
        if scheduler_collapse and not reference_sender:
            raise ValueError("only the pinned sender can collapse")
        terminal_event = (
            "path_outage_sender_failure"
            if reference_sender
            else "path_outage_sender_complete"
        )
        terminal: dict[str, object] = {
            "event": terminal_event,
            "role": "caller",
            "sent_messages": 16,
            "first_active_member": 101,
            "final_active_member": 101 if reference_sender else 102,
            "active_member_transitions": 0 if reference_sender else 1,
            "group_state": 6 if reference_sender else 5,
            "primary_state": 6,
            "backup_state": 9 if reference_sender else 5,
            "error_code": 0,
            "srt_version": run_group_interop.PINNED_SRT_VERSION,
        }
        if scheduler_collapse:
            terminal.update(
                {
                    "final_active_member": -1,
                    "active_member_transitions": 1,
                    "group_state": 5,
                    "primary_state": 5,
                    "backup_state": 5,
                }
            )
        events: tuple[dict[str, object], ...] = (
            {
                "event": "path_outage_sender_ready",
                "role": "caller",
                "primary_member": 101,
                "backup_member": 102,
                "srt_version": run_group_interop.PINNED_SRT_VERSION,
            },
            {"event": "path_outage_sender_released", "role": "caller"},
            terminal,
        )
        if not reference_sender:
            events += (
                {
                    "event": "complete",
                    "role": "caller",
                    "group": 200,
                    "member": 101,
                    "members": 2,
                    "policy": "backup",
                    "callback_calls": 2,
                    "callback_valid": True,
                    "payload_valid": True,
                    "late_join": False,
                    "peer_error": False,
                    "peer_error_replacement": False,
                    "peer_error_member_isolated": False,
                    "peer_error_group_update": False,
                    "replacement_member": -1,
                    "replacement_distinct": False,
                    "replacement_group_update_absent": False,
                    "replacement_connected": False,
                    "healthy_member": 101,
                    "failed_member": 102,
                    "group_connected": True,
                    "peer_version": run_group_interop.PINNED_SRT_VERSION,
                    "peer_version_valid": True,
                    "tokens": [4_001, 4_002],
                },
            )
        return "\n".join(json.dumps(event) for event in events) + "\n"

    @staticmethod
    def _receiver_output(reference_receiver: bool) -> str:
        terminal: dict[str, object] = {
            "event": (
                "path_outage_receiver_complete"
                if reference_receiver
                else "path_outage_receiver_failure"
            ),
            "role": "listener",
            "received_messages": 16 if reference_receiver else 3,
            "group_state": 5,
            "srt_version": run_group_interop.PINNED_SRT_VERSION,
        }
        if not reference_receiver:
            terminal["error_code"] = 6_003
        events: tuple[dict[str, object], ...] = (
            {"event": "ready"},
            {
                "event": "path_outage_receiver_ready",
                "role": "listener",
                "srt_version": run_group_interop.PINNED_SRT_VERSION,
            },
            terminal,
        )
        if reference_receiver:
            events += (
                {
                    "event": "complete",
                    "role": "listener",
                    "group": 300,
                    "members": 2,
                    "listener_update": True,
                    "initial_listener_update": False,
                    "late_listener_update": True,
                    "bonded_listeners": 1,
                    "payload_valid": True,
                    "late_join": False,
                    "peer_error": False,
                    "peer_error_replacement": False,
                    "replacement_attached": False,
                    "transient_group_receive_errors": 0,
                    "group_connected": True,
                    "payload_hash": 123,
                    "peer_version": run_group_interop.PINNED_SRT_VERSION,
                    "peer_version_valid": True,
                    "listener_callback_calls": 2,
                    "listener_metadata_valid": True,
                },
            )
        return "\n".join(json.dumps(event) for event in events) + "\n"

    @staticmethod
    def _scheduler_collapse_stderr(
        *, terminal_override: bool = False
    ) -> str:
        prefix = "00:00:00.000001/reference-group!W:SRT.gs: "
        buffer_prefix = "00:00:00.000002/SRT:RcvQ:w2*E:SRT.bs: "
        queue_prefix = "00:00:00.000003/SRT:SndQ:w2*E:SRT.qs: "
        lines = [
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            "(2 available). Reason: no stable links",
            prefix + "@101 FRESH-ACTIVATED",
            buffer_prefix
            + "CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
            "max offset=0",
            queue_prefix + "CSndBuffer: skipping packet %0 #0 with TTL=0",
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            "(1 available). Reason: no stable links",
            prefix + "@102 FRESH-ACTIVATED",
            prefix
            + (
                "@102: IPE: Overriding with seq %112 DISCREPANCY "
                "against current %140 and next sched %110 - diff=-28"
                if terminal_override
                else "@102: IPE: Overriding with seq %120 DISCREPANCY "
                "against current %140 and next sched %118 - diff=-20"
            ),
            queue_prefix
            + "@102: IPE: packData: SCHEDULING sequence 110 is "
            "behind of EXTRACTION sequence 141, dropping this packet: "
            "DIFF=-31 STAMP=1234ABCD",
        ]
        if terminal_override:
            lines.extend(
                (
                    queue_prefix
                    + "@102: IPE: packData: SCHEDULING sequence 111 is "
                    "behind of EXTRACTION sequence 142, dropping this "
                    "packet: DIFF=-31 STAMP=2345BCDE",
                    prefix
                    + "grp/sendBackup: @102: IPE: another running link "
                    "seq discrepancy: %119 vs. previous %121 - fixing",
                    prefix
                    + "@102: IPE: Overriding with seq %122 DISCREPANCY "
                    "against current %142 and next sched %120 - diff=-20",
                    prefix
                    + "grp/sendBackup: @102: IPE: another running link "
                    "seq discrepancy: %120 vs. previous %122 - fixing",
                    prefix
                    + "@102: IPE: Overriding with seq %123 DISCREPANCY "
                    "against current %143 and next sched %121 - diff=-20",
                )
            )
        else:
            lines.extend(
                (
                    prefix
                    + "@102: IPE: Overriding with seq %121 DISCREPANCY "
                    "against current %141 and next sched %119 - diff=-20",
                    queue_prefix
                    + "@102: IPE: packData: SCHEDULING sequence 111 is "
                    "behind of EXTRACTION sequence 142, dropping this "
                    "packet: DIFF=-31 STAMP=2345BCDE",
                    prefix
                    + "grp/sendBackup: @102: IPE: another running link "
                    "seq discrepancy: %119 vs. previous %121 - fixing",
                )
            )
        lines.append(run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR)
        return "\n".join(lines)

    @staticmethod
    def _scheduler_collapse_queue_drain_stderr(
        *, terminal_before_final_schedule: bool = False
    ) -> str:
        prefix = "00:00:00.000001/reference-group!W:SRT.gs: "
        buffer_prefix = "00:00:00.000002/SRT:RcvQ:w2*E:SRT.bs: "
        queue_prefix = "00:00:00.000003/SRT:SndQ:w2*E:SRT.qs: "
        final_schedule = (
            queue_prefix
            + "@102: IPE: packData: SCHEDULING sequence 113 is "
            "behind of EXTRACTION sequence 144, dropping this packet: "
            "DIFF=-31 STAMP=4567DEF0"
        )
        lines = [
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            "(2 available). Reason: no stable links",
            prefix + "@101 FRESH-ACTIVATED",
            buffer_prefix
            + "CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
            "max offset=0",
            queue_prefix
            + "CSndBuffer: skipping packet %0 #0 with TTL=0",
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            "(1 available). Reason: no stable links",
            prefix
            + "@102: IPE: Overriding with seq %112 DISCREPANCY "
            "against current %140 and next sched %110 - diff=-28",
            queue_prefix
            + "@102: IPE: packData: SCHEDULING sequence 110 is "
            "behind of EXTRACTION sequence 141, dropping this packet: "
            "DIFF=-31 STAMP=1234ABCD",
            prefix + "@102 FRESH-ACTIVATED",
            queue_prefix
            + "@102: IPE: packData: SCHEDULING sequence 111 is "
            "behind of EXTRACTION sequence 142, dropping this packet: "
            "DIFF=-31 STAMP=2345BCDE",
            prefix
            + "grp/sendBackup: @102: IPE: another running link seq "
            "discrepancy: %114 vs. previous %116 - fixing",
            prefix
            + "@102: IPE: Overriding with seq %117 DISCREPANCY "
            "against current %142 and next sched %115 - diff=-25",
            queue_prefix
            + "@102: IPE: packData: SCHEDULING sequence 112 is "
            "behind of EXTRACTION sequence 143, dropping this packet: "
            "DIFF=-31 STAMP=3456CDEF",
            final_schedule,
            prefix
            + "grp/sendBackup: @102: IPE: another running link seq "
            "discrepancy: %115 vs. previous %117 - fixing",
            prefix
            + "@102: IPE: Overriding with seq %118 DISCREPANCY "
            "against current %144 and next sched %116 - diff=-26",
        ]
        if terminal_before_final_schedule:
            lines.remove(final_schedule)
            lines.extend(
                (
                    run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
                    final_schedule,
                )
            )
        else:
            lines.append(run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR)
        return "\n".join(lines)

    @staticmethod
    def _scheduler_collapse_interleaved_queue_drain_stderr(
        *,
        late_tail_depth: int = 0,
        terminal_before_final_schedule: bool = False,
    ) -> str:
        if late_tail_depth not in (0, 1, 2, 3):
            raise ValueError("unsupported late queue tail depth")
        prefix = "00:00:00.000001/reference-group!W:SRT.gs: "
        buffer_prefix = "00:00:00.000002/SRT:RcvQ:w2*E:SRT.bs: "
        queue_prefix = "00:00:00.000003/SRT:SndQ:w2*E:SRT.qs: "

        def scheduled(sequence: int, extraction: int, stamp: int) -> str:
            return (
                queue_prefix
                + "@102: IPE: packData: SCHEDULING sequence "
                + f"{sequence} is behind of EXTRACTION sequence "
                + f"{extraction}, dropping this packet: DIFF=-31 "
                + f"STAMP={stamp:08X}"
            )

        def fixing(sequence: int, previous: int) -> str:
            return (
                prefix
                + "grp/sendBackup: @102: IPE: another running link seq "
                + f"discrepancy: %{sequence} vs. previous %{previous} "
                + "- fixing"
            )

        def override(
            sequence: int, current: int, next_scheduled: int
        ) -> str:
            difference = sequence - current
            return (
                prefix
                + f"@102: IPE: Overriding with seq %{sequence} "
                + f"DISCREPANCY against current %{current} and next "
                + f"sched %{next_scheduled} - diff={difference}"
            )

        repair_offset = -late_tail_depth
        lines = [
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            + "(2 available). Reason: no stable links",
            prefix + "@101 FRESH-ACTIVATED",
            buffer_prefix
            + "CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
            + "max offset=0",
            queue_prefix + "CSndBuffer: skipping packet %0 #0 with TTL=0",
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            + "(1 available). Reason: no stable links",
            override(112, 140, 110),
            scheduled(110, 141, 0x1234ABCD),
            scheduled(111, 142, 0x2345BCDE),
            prefix + "@102 FRESH-ACTIVATED",
            scheduled(112, 143, 0x3456CDEF),
            scheduled(113, 144, 0x4567DEF0),
            fixing(118 + repair_offset, 120 + repair_offset),
            override(121 + repair_offset, 144, 119 + repair_offset),
            fixing(119 + repair_offset, 121 + repair_offset),
            override(122 + repair_offset, 144, 120 + repair_offset),
            scheduled(114, 145, 0x5678EF01),
            scheduled(115, 146, 0x6789F012),
            fixing(120 + repair_offset, 122 + repair_offset),
            override(123 + repair_offset, 146, 121 + repair_offset),
            scheduled(116, 147, 0x789A0123),
            fixing(121 + repair_offset, 123 + repair_offset),
            override(124 + repair_offset, 147, 122 + repair_offset),
            fixing(122 + repair_offset, 124 + repair_offset),
            override(125 + repair_offset, 147, 123 + repair_offset),
            scheduled(117, 148, 0x89AB1234),
        ]
        final_schedule = scheduled(118, 149, 0x9ABC2345)
        if not terminal_before_final_schedule:
            lines.append(final_schedule)
        lines.extend(
            [
                fixing(123 + repair_offset, 125 + repair_offset),
                override(126 + repair_offset, 149, 124 + repair_offset),
                run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
            ]
        )
        if terminal_before_final_schedule:
            lines.append(final_schedule)
        return "\n".join(lines)

    @staticmethod
    def _scheduler_collapse_complete_queue_drain_stderr() -> str:
        prefix = "00:00:00.000001/reference-group!W:SRT.gs: "
        buffer_prefix = "00:00:00.000002/SRT:RcvQ:w2*E:SRT.bs: "
        queue_prefix = "00:00:00.000003/SRT:SndQ:w2*E:SRT.qs: "

        def scheduled(sequence: int) -> str:
            return (
                queue_prefix
                + "@102: IPE: packData: SCHEDULING sequence "
                + f"{sequence} is behind of EXTRACTION sequence "
                + f"{sequence + 31}, dropping this packet: DIFF=-31 "
                + f"STAMP={sequence:08X}"
            )

        def fixing(sequence: int, previous: int) -> str:
            return (
                prefix
                + "grp/sendBackup: @102: IPE: another running link seq "
                + f"discrepancy: %{sequence} vs. previous %{previous} "
                + "- fixing"
            )

        def override(
            sequence: int, current: int, next_scheduled: int
        ) -> str:
            return (
                prefix
                + f"@102: IPE: Overriding with seq %{sequence} "
                + f"DISCREPANCY against current %{current} and next "
                + f"sched %{next_scheduled} - diff={sequence - current}"
            )

        lines = [
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            + "(2 available). Reason: no stable links",
            prefix + "@101 FRESH-ACTIVATED",
            buffer_prefix
            + "CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
            + "max offset=0",
            queue_prefix + "CSndBuffer: skipping packet %0 #0 with TTL=0",
            prefix
            + "grp/sendBackup: trying to activate a stand-by link "
            + "(1 available). Reason: no stable links",
            override(112, 140, 110),
            prefix + "@102 FRESH-ACTIVATED",
            scheduled(110),
            scheduled(111),
        ]
        for offset in range(6):
            lines.extend(
                (
                    fixing(118 + offset, 120 + offset),
                    override(121 + offset, 142 + offset, 119 + offset),
                    scheduled(112 + offset),
                )
            )
        lines.append(run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR)
        return "\n".join(lines)

    def test_path_outage_sender_events_bind_directional_outcomes(
        self,
    ) -> None:
        robotweax = self._sender_output(False)
        reference = self._sender_output(True)
        scheduler_collapse = self._sender_output(
            True, scheduler_collapse=True
        )
        self.assertIsNotNone(
            run_group_interop.has_exact_path_outage_sender_events(
                robotweax, reference_sender=False
            )
        )
        self.assertIsNotNone(
            run_group_interop.has_exact_path_outage_sender_events(
                reference, reference_sender=True
            )
        )
        collapse_events = (
            run_group_interop.has_exact_path_outage_sender_events(
                scheduler_collapse, reference_sender=True
            )
        )
        self.assertIsNotNone(collapse_events)
        assert collapse_events is not None
        self.assertEqual(
            run_group_interop.reference_path_outage_sender_variant(
                *collapse_events
            ),
            run_group_interop.PATH_OUTAGE_REFERENCE_SCHEDULER_COLLAPSE,
        )
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_sender_events(
                robotweax, reference_sender=True
            )
        )
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_sender_events(
                reference, reference_sender=False
            )
        )
        parsed = [json.loads(line) for line in reference.splitlines()]
        parsed[-1]["active_member_transitions"] = "0"
        malformed = "\n".join(json.dumps(event) for event in parsed)
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_sender_events(
                malformed, reference_sender=True
            )
        )
        parsed = [
            json.loads(line) for line in scheduler_collapse.splitlines()
        ]
        parsed[-1]["active_member_transitions"] = 2
        malformed = "\n".join(json.dumps(event) for event in parsed)
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_sender_events(
                malformed, reference_sender=True
            )
        )
        parsed = [json.loads(line) for line in reference.splitlines()]
        parsed[-1]["unexpected"] = True
        malformed = "\n".join(json.dumps(event) for event in parsed)
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_sender_events(
                malformed, reference_sender=True
            )
        )

    def test_path_outage_receiver_events_are_exact_and_version_scoped(
        self,
    ) -> None:
        reference = self._receiver_output(True)
        robotweax = self._receiver_output(False)
        self.assertIsNotNone(
            run_group_interop.has_exact_path_outage_receiver_events(
                reference, reference_receiver=True
            )
        )
        initial_update = reference.replace(
            '"initial_listener_update": false',
            '"initial_listener_update": true',
        )
        self.assertIsNotNone(
            run_group_interop.has_exact_path_outage_receiver_events(
                initial_update, reference_receiver=True
            )
        )
        self.assertIsNotNone(
            run_group_interop.has_exact_path_outage_receiver_events(
                robotweax, reference_receiver=False
            )
        )
        parsed = [json.loads(line) for line in reference.splitlines()]
        parsed[1]["srt_version"] = 0x01_05_06
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_receiver_events(
                "\n".join(json.dumps(event) for event in parsed),
                reference_receiver=True,
            )
        )
        parsed = [json.loads(line) for line in reference.splitlines()]
        parsed[-1]["initial_listener_update"] = 1
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_receiver_events(
                "\n".join(json.dumps(event) for event in parsed),
                reference_receiver=True,
            )
        )
        parsed = [json.loads(line) for line in reference.splitlines()]
        parsed[2]["received_messages"] = 15
        self.assertIsNone(
            run_group_interop.has_exact_path_outage_receiver_events(
                "\n".join(json.dumps(event) for event in parsed),
                reference_receiver=True,
            )
        )

    def test_reference_path_outage_stderr_classifiers_reject_noise(
        self,
    ) -> None:
        listener_error = "\n".join(
            (
                "stamp/*E:SRT.ac: @1: OPTION: #35 UNKNOWN",
                "stamp/*E:SRT.ac: @2: OPTION: #35 UNKNOWN",
                "stamp/*E:SRT.ac: @1: OPTION: #59 UNKNOWN",
                "stamp/*E:SRT.ac: @2: OPTION: #59 UNKNOWN",
            )
        )
        sender_error = "\n".join(
            (
                "stamp grp/sendBackup: trying to activate a stand-by link",
                "stamp FRESH-ACTIVATED",
                "stamp CSndBuffer::getMsgNoAt: IPE: offset=2",
                "stamp ATTACK/IPE: incoming ack seq 4 exceeds current 1",
                "stamp CSndBuffer: skipping packet %0 #0 with TTL=0",
                run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_path_outage_listener_stderr(
                listener_error
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_path_outage_sender_stderr(
                sender_error,
                variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
                primary_member=101,
                backup_member=102,
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_path_outage_sender_stderr(
                sender_error.replace(
                    "stamp CSndBuffer: skipping packet %0 #0 with TTL=0\n",
                    "",
                ),
                variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
                primary_member=101,
                backup_member=102,
            )
        )
        self.assertFalse(
            run_group_interop.has_expected_reference_path_outage_listener_stderr(
                listener_error + "\nunexpected"
            )
        )
        self.assertFalse(
            run_group_interop.has_expected_reference_path_outage_sender_stderr(
                sender_error + "\nunexpected",
                variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
                primary_member=101,
                backup_member=102,
            )
        )
        self.assertFalse(
            run_group_interop.has_expected_reference_path_outage_sender_stderr(
                sender_error.replace(
                    "stamp ATTACK/IPE: incoming ack seq 4 exceeds current 1\n",
                    "",
                ),
                variant=(
                    run_group_interop.PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
                ),
                primary_member=101,
                backup_member=102,
            )
        )

    def test_path_outage_process_classifiers_are_directional_and_fail_closed(
        self,
    ) -> None:
        listener_error = "\n".join(
            (
                "stamp/*E:SRT.ac: @1: OPTION: #35 UNKNOWN",
                "stamp/*E:SRT.ac: @2: OPTION: #35 UNKNOWN",
                "stamp/*E:SRT.ac: @1: OPTION: #59 UNKNOWN",
                "stamp/*E:SRT.ac: @2: OPTION: #59 UNKNOWN",
            )
        )
        sender_error = "\n".join(
            (
                "stamp grp/sendBackup: trying to activate a stand-by link",
                "stamp FRESH-ACTIVATED",
                "stamp CSndBuffer::getMsgNoAt: IPE: offset=2",
                "stamp ATTACK/IPE: incoming ack seq 4 exceeds current 1",
                "stamp CSndBuffer: skipping packet %0 #0 with TTL=0",
                run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_listener_path_outage_success(
                self._sender_output(False),
                "",
                self._receiver_output(True),
                listener_error,
                0,
                0,
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_sender_path_outage_failure(
                self._sender_output(True),
                sender_error,
                self._receiver_output(False),
                (
                    "path-outage receive failed after 3 messages: "
                    "Connection was broken"
                ),
                5,
                6,
            )
        )
        self.assertFalse(
            run_group_interop.has_expected_reference_listener_path_outage_success(
                self._sender_output(False),
                "unexpected",
                self._receiver_output(True),
                listener_error,
                0,
                0,
            )
        )
        self.assertFalse(
            run_group_interop.has_expected_reference_sender_path_outage_failure(
                self._sender_output(True),
                sender_error,
                self._receiver_output(False),
                (
                    "path-outage receive failed after 3 messages: "
                    "Connection was broken"
                ),
                0,
                6,
            )
        )

    def test_reference_scheduler_collapse_stderr_is_identity_bound(
        self,
    ) -> None:
        error = self._scheduler_collapse_stderr()
        validate = (
            run_group_interop.has_expected_reference_scheduler_collapse_stderr
        )
        self.assertTrue(
            validate(error, primary_member=101, backup_member=102)
        )
        terminal_override = self._scheduler_collapse_stderr(
            terminal_override=True
        )
        self.assertTrue(
            validate(
                terminal_override,
                primary_member=101,
                backup_member=102,
            )
        )
        queue_drain = self._scheduler_collapse_queue_drain_stderr()
        self.assertTrue(
            validate(
                queue_drain,
                primary_member=101,
                backup_member=102,
            )
        )
        terminal_queue_drain = (
            self._scheduler_collapse_queue_drain_stderr(
                terminal_before_final_schedule=True
            )
        )
        self.assertTrue(
            validate(
                terminal_queue_drain,
                primary_member=101,
                backup_member=102,
            )
        )
        interleaved_queue_drain = (
            self._scheduler_collapse_interleaved_queue_drain_stderr()
        )
        self.assertTrue(
            validate(
                interleaved_queue_drain,
                primary_member=101,
                backup_member=102,
            )
        )
        late_queue_tail = (
            self._scheduler_collapse_interleaved_queue_drain_stderr(
                late_tail_depth=1
            )
        )
        self.assertTrue(
            validate(
                late_queue_tail,
                primary_member=101,
                backup_member=102,
            )
        )
        post_terminal_late_queue_tail = (
            self._scheduler_collapse_interleaved_queue_drain_stderr(
                late_tail_depth=1,
                terminal_before_final_schedule=True,
            )
        )
        self.assertTrue(
            validate(
                post_terminal_late_queue_tail,
                primary_member=101,
                backup_member=102,
            )
        )
        complete_queue_drain = (
            self._scheduler_collapse_complete_queue_drain_stderr()
        )
        self.assertTrue(
            validate(
                complete_queue_drain,
                primary_member=101,
                backup_member=102,
            )
        )
        two_packet_late_queue_tail = (
            self._scheduler_collapse_interleaved_queue_drain_stderr(
                late_tail_depth=2
            )
        )
        self.assertTrue(
            validate(
                two_packet_late_queue_tail,
                primary_member=101,
                backup_member=102,
            )
        )
        late_queue_tail_lines = late_queue_tail.splitlines()
        scheduled_tail = [
            line
            for line in late_queue_tail_lines
            if any(
                f"SCHEDULING sequence {sequence} " in line
                for sequence in range(114, 119)
            )
        ]
        reordered_late_tail = [
            line
            for line in late_queue_tail_lines
            if line not in scheduled_tail
        ]
        first_repair = next(
            index
            for index, line in enumerate(reordered_late_tail)
            if "another running link seq discrepancy" in line
        )
        reordered_late_tail[first_repair:first_repair] = scheduled_tail
        two_packet_tail_lines = two_packet_late_queue_tail.splitlines()
        two_packet_scheduled_tail = [
            line
            for line in two_packet_tail_lines
            if any(
                f"SCHEDULING sequence {sequence} " in line
                for sequence in range(114, 119)
            )
        ]
        reordered_two_packet_tail = [
            line
            for line in two_packet_tail_lines
            if line not in two_packet_scheduled_tail
        ]
        first_two_packet_repair = next(
            index
            for index, line in enumerate(reordered_two_packet_tail)
            if "another running link seq discrepancy" in line
        )
        reordered_two_packet_tail[
            first_two_packet_repair:first_two_packet_repair
        ] = two_packet_scheduled_tail
        mutations = {
            "unexpected line": error + "\nunexpected",
            "wrong activated backup": error.replace(
                "@102 FRESH-ACTIVATED", "@999 FRESH-ACTIVATED"
            ),
            "wrong scheduler member": error.replace(
                "@102: IPE: Overriding", "@999: IPE: Overriding"
            ),
            "inconsistent override difference": error.replace(
                "next sched %118 - diff=-20",
                "next sched %118 - diff=-19",
            ),
            "noncontiguous scheduled data": error.replace(
                "SCHEDULING sequence 111", "SCHEDULING sequence 112"
            ),
            "missing sequence repair": "\n".join(
                line
                for line in error.splitlines()
                if "another running link seq discrepancy" not in line
            ),
            "terminal override without repair": "\n".join(
                line
                for line in terminal_override.splitlines()
                if "another running link seq discrepancy" not in line
            ),
            "wrong terminal override member": terminal_override.replace(
                "@102: IPE: Overriding with seq %123",
                "@999: IPE: Overriding with seq %123",
            ),
            "wrong terminal override pairing": terminal_override.replace(
                "next sched %121 - diff=-20",
                "next sched %122 - diff=-20",
            ),
            "terminal pair detached from preceding override": (
                terminal_override.replace(
                    "discrepancy: %120 vs. previous %122 - fixing",
                    "discrepancy: %200 vs. previous %202 - fixing",
                )
                .replace(
                    "Overriding with seq %123 DISCREPANCY against "
                    "current %143 and next sched %121 - diff=-20",
                    "Overriding with seq %203 DISCREPANCY against "
                    "current %223 and next sched %201 - diff=-20",
                )
            ),
            "bootstrap override detached from scheduler": (
                terminal_override.replace(
                    "Overriding with seq %112 DISCREPANCY against "
                    "current %140 and next sched %110 - diff=-28",
                    "Overriding with seq %1000 DISCREPANCY against "
                    "current %1028 and next sched %998 - diff=-28",
                )
            ),
            "earlier repair detached from following override": (
                terminal_override.replace(
                    "discrepancy: %119 vs. previous %121 - fixing",
                    "discrepancy: %500 vs. previous %502 - fixing",
                )
            ),
            "lookup between terminal repair and override": (
                terminal_override.replace(
                    "00:00:00.000001/reference-group!W:SRT.gs: "
                    "@102: IPE: Overriding with seq %123",
                    "00:00:00.000002/SRT:RcvQ:w2*E:SRT.bs: "
                    "CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
                    "max offset=-2\n"
                    "00:00:00.000001/reference-group!W:SRT.gs: "
                    "@102: IPE: Overriding with seq %123",
                )
            ),
            "terminal override before repair": terminal_override.replace(
                "00:00:00.000001/reference-group!W:SRT.gs: "
                "grp/sendBackup: @102: IPE: another running link seq "
                "discrepancy: %120 vs. previous %122 - fixing\n"
                "00:00:00.000001/reference-group!W:SRT.gs: "
                "@102: IPE: Overriding with seq %123 DISCREPANCY "
                "against current %143 and next sched %121 - diff=-20",
                "00:00:00.000001/reference-group!W:SRT.gs: "
                "@102: IPE: Overriding with seq %123 DISCREPANCY "
                "against current %143 and next sched %121 - diff=-20\n"
                "00:00:00.000001/reference-group!W:SRT.gs: "
                "grp/sendBackup: @102: IPE: another running link seq "
                "discrepancy: %120 vs. previous %122 - fixing",
            ),
            "warning after terminal override": terminal_override.replace(
                run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
                "00:00:00.000001/reference-group!W:SRT.gs: "
                + run_group_interop.PINNED_OPTION_WARNING_SUFFIXES[0]
                + "\n"
                + run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
            ),
            "two terminal overrides": terminal_override.replace(
                run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
                "00:00:00.000001/reference-group!W:SRT.gs: "
                + "@102: IPE: Overriding with seq %124 DISCREPANCY "
                "against current %144 and next sched %122 - diff=-20\n"
                + run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
            ),
            "line after terminal": terminal_override + "\nunexpected",
            "second schedule after terminal": terminal_queue_drain
            + "\n"
            + terminal_queue_drain.splitlines()[-1],
            "wrong post-terminal scheduler member": (
                terminal_queue_drain.replace(
                    "@102: IPE: packData: SCHEDULING sequence 113",
                    "@999: IPE: packData: SCHEDULING sequence 113",
                )
            ),
            "wrong post-terminal scheduler difference": (
                terminal_queue_drain.replace(
                    "DIFF=-31 STAMP=4567DEF0",
                    "DIFF=-30 STAMP=4567DEF0",
                )
            ),
            "queue drain detached from scheduled tail": queue_drain.replace(
                "discrepancy: %114 vs. previous %116 - fixing",
                "discrepancy: %214 vs. previous %216 - fixing",
            ),
            "queue drain missing final repair": "\n".join(
                line
                for line in queue_drain.splitlines()
                if "discrepancy: %115 vs. previous %117 - fixing" not in line
            ),
            "queue drain without any repair": "\n".join(
                line
                for line in queue_drain.splitlines()
                if "another running link seq discrepancy" not in line
                and "Overriding with seq %117" not in line
                and "Overriding with seq %118" not in line
            ),
            "queue drain warning before terminal": queue_drain.replace(
                run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
                "00:00:00.000001/reference-group!W:SRT.gs: "
                + run_group_interop.PINNED_OPTION_WARNING_SUFFIXES[0]
                + "\n"
                + run_group_interop.PATH_OUTAGE_REFERENCE_SENDER_ERROR,
            ),
            "interleaved queue drain detached from scheduled tail": (
                interleaved_queue_drain.replace(
                    "discrepancy: %118 vs. previous %120 - fixing",
                    "discrepancy: %128 vs. previous %130 - fixing",
                )
            ),
            "interleaved queue drain breaks repair adjacency": (
                interleaved_queue_drain.replace(
                    "@102: IPE: Overriding with seq %123",
                    "CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
                    "max offset=-2\n"
                    "00:00:00.000001/reference-group!W:SRT.gs: "
                    "@102: IPE: Overriding with seq %123",
                )
            ),
            "interleaved queue drain noncontiguous schedule": (
                interleaved_queue_drain.replace(
                    "SCHEDULING sequence 116",
                    "SCHEDULING sequence 119",
                )
            ),
            "late queue tail emitted before repair begins": "\n".join(
                reordered_late_tail
            ),
            "noncontiguous post-terminal interleaved queue tail": (
                post_terminal_late_queue_tail.replace(
                    "SCHEDULING sequence 118 is behind of "
                    "EXTRACTION sequence 149",
                    "SCHEDULING sequence 119 is behind of "
                    "EXTRACTION sequence 150",
                )
            ),
            "complete queue drain detaches repair from scheduled prefix": (
                complete_queue_drain.replace(
                    "discrepancy: %118 vs. previous %120 - fixing",
                    "discrepancy: %218 vs. previous %220 - fixing",
                )
            ),
            "complete queue drain has a scheduled gap": (
                complete_queue_drain.replace(
                    "SCHEDULING sequence 116",
                    "SCHEDULING sequence 119",
                )
            ),
            "two-packet queue tail is deeper than observed": (
                self._scheduler_collapse_interleaved_queue_drain_stderr(
                    late_tail_depth=3
                )
            ),
            "two-packet queue tail has a scheduled gap": (
                two_packet_late_queue_tail.replace(
                    "SCHEDULING sequence 116",
                    "SCHEDULING sequence 119",
                )
            ),
            "two-packet queue tail emitted before repair begins": (
                "\n".join(reordered_two_packet_tail)
            ),
            "two-packet queue tail breaks final repair adjacency": (
                two_packet_late_queue_tail.replace(
                    "@102: IPE: Overriding with seq %124",
                    "CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
                    "max offset=-2\n"
                    "00:00:00.000001/reference-group!W:SRT.gs: "
                    "@102: IPE: Overriding with seq %124",
                )
            ),
        }
        for label, mutation in mutations.items():
            with self.subTest(label=label):
                self.assertFalse(
                    validate(
                        mutation,
                        primary_member=101,
                        backup_member=102,
                    )
                )

    def test_reference_scheduler_sequence_math_is_rollover_safe(
        self,
    ) -> None:
        difference = run_group_interop.signed_srt_sequence_difference
        self.assertEqual(difference(0, 0x7FFF_FFFF), 1)
        self.assertEqual(difference(0x7FFF_FFFF, 0), -1)
        self.assertEqual(difference(118, 120), -2)
        self.assertIsNone(difference(-1, 0))
        self.assertIsNone(difference(0, 0x8000_0000))

    def test_scheduler_collapse_process_profile_is_separate(self) -> None:
        listener_error = (
            "path-outage receive failed after 3 messages: "
            "Non-blocking call failure: transmission timed out"
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_sender_path_outage_failure(
                self._sender_output(True, scheduler_collapse=True),
                self._scheduler_collapse_stderr(),
                self._receiver_output(False),
                listener_error,
                5,
                6,
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_sender_path_outage_failure(
                self._sender_output(True, scheduler_collapse=True),
                self._scheduler_collapse_stderr(terminal_override=True),
                self._receiver_output(False),
                listener_error,
                5,
                6,
            )
        )
        self.assertTrue(
            run_group_interop.has_expected_reference_sender_path_outage_failure(
                self._sender_output(True, scheduler_collapse=True),
                self._scheduler_collapse_interleaved_queue_drain_stderr(
                    late_tail_depth=2
                ),
                self._receiver_output(False),
                listener_error,
                5,
                6,
            )
        )
        self.assertFalse(
            run_group_interop.has_expected_reference_sender_path_outage_failure(
                self._sender_output(True),
                self._scheduler_collapse_stderr(),
                self._receiver_output(False),
                listener_error,
                5,
                6,
            )
        )


if __name__ == "__main__":
    unittest.main()
