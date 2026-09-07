#!/usr/bin/env python3
"""Generate or score Robotweax Live timing observations."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

from live_timing import (
    PCR_TICK_MODULUS,
    MpegTsProfile,
    generate_binary_messages,
    generate_mpeg_ts_messages,
    load_timing_samples,
    score_timing,
    synthetic_timing_samples,
)


PROFILE_PACKETS = {
    "ts-188": 1,
    "ts-376": 2,
    "ts-1316": 7,
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Score JSONL timing events or generate a deterministic "
            "MPEG-TS/non-TS baseline"
        )
    )
    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "--events",
        type=Path,
        help="JSONL events captured by an SRT/UDP timing runner",
    )
    source.add_argument(
        "--profile",
        choices=(*PROFILE_PACKETS, "binary-1200"),
        default="ts-1316",
        help="deterministic synthetic profile",
    )
    parser.add_argument("--messages", type=int, default=100)
    parser.add_argument(
        "--bitrate-bps", type=int, default=15_040_000
    )
    parser.add_argument("--latency-us", type=int, default=120_000)
    parser.add_argument("--release-offset-us", type=int, default=0)
    parser.add_argument("--egress-delay-us", type=int, default=50)
    parser.add_argument("--burst-window-us", type=int, default=1_000)
    parser.add_argument("--pcr-start-ticks", type=int, default=0)
    parser.add_argument(
        "--pcr-discontinuity-message",
        type=int,
        help="synthetic TS message whose first packet signals discontinuity",
    )
    parser.add_argument(
        "--write-events",
        type=Path,
        help="write generated synthetic observations as JSONL",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="write the scorecard as JSON instead of stdout",
    )
    return parser.parse_args()


def generated_messages(arguments: argparse.Namespace):
    if arguments.messages <= 0 or arguments.bitrate_bps <= 0:
        raise ValueError("message count and bitrate must be positive")
    if not 0 <= arguments.pcr_start_ticks < PCR_TICK_MODULUS:
        raise ValueError("initial PCR is outside the PCR clock range")
    if arguments.profile == "binary-1200":
        if arguments.pcr_discontinuity_message is not None:
            raise ValueError("binary profile has no PCR discontinuity")
        return generate_binary_messages(
            1_200,
            arguments.messages,
            arguments.bitrate_bps,
        )
    packets_per_message = PROFILE_PACKETS[arguments.profile]
    discontinuities: frozenset[int] = frozenset()
    if arguments.pcr_discontinuity_message is not None:
        if not 0 <= arguments.pcr_discontinuity_message < arguments.messages:
            raise ValueError("PCR discontinuity message is outside the stream")
        discontinuities = frozenset(
            {
                arguments.pcr_discontinuity_message
                * packets_per_message
            }
        )
    return generate_mpeg_ts_messages(
        MpegTsProfile(
            packets_per_message=packets_per_message,
            message_count=arguments.messages,
            bitrate_bits_per_second=arguments.bitrate_bps,
            pcr_interval_packets=packets_per_message,
            start_pcr_ticks=arguments.pcr_start_ticks,
            discontinuity_packets=discontinuities,
        )
    )


def main() -> int:
    arguments = parse_arguments()
    if arguments.events is not None:
        if arguments.write_events is not None:
            raise ValueError("captured events cannot be rewritten as synthetic")
        samples = load_timing_samples(arguments.events)
    else:
        samples = synthetic_timing_samples(
            generated_messages(arguments),
            arguments.latency_us,
            release_offset_microseconds=(
                arguments.release_offset_us
            ),
            egress_delay_microseconds=arguments.egress_delay_us,
        )
        if arguments.write_events is not None:
            with arguments.write_events.open(
                "w", encoding="utf-8"
            ) as destination:
                for sample in samples:
                    destination.write(
                        json.dumps(asdict(sample), sort_keys=True)
                        + "\n"
                    )
    scorecard = score_timing(
        samples,
        burst_window_microseconds=arguments.burst_window_us,
    )
    rendered = json.dumps(scorecard, indent=2, sort_keys=True) + "\n"
    if arguments.output is None:
        print(rendered, end="")
    else:
        arguments.output.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
