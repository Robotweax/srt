/* SPDX-License-Identifier: MIT */

#ifndef ROBOTWEAX_SRT_H
#define ROBOTWEAX_SRT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ROBOTWEAX_SRT_SHARED)
#    if defined(ROBOTWEAX_SRT_BUILDING)
#      define ROBOTWEAX_SRT_API __declspec(dllexport)
#    else
#      define ROBOTWEAX_SRT_API __declspec(dllimport)
#    endif
#  else
#    define ROBOTWEAX_SRT_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define ROBOTWEAX_SRT_API __attribute__((visibility("default")))
#else
#  define ROBOTWEAX_SRT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ROBOTWEAX_SRT_ABI_VERSION 1u

typedef enum robotweax_srt_error {
    ROBOTWEAX_SRT_OK = 0,
    ROBOTWEAX_SRT_BUFFER_TOO_SMALL = 1,
    ROBOTWEAX_SRT_PACKET_TOO_SHORT = 2,
    ROBOTWEAX_SRT_INVALID_PACKET_TYPE = 3,
    ROBOTWEAX_SRT_INVALID_CONTROL_TYPE = 4,
    ROBOTWEAX_SRT_INVALID_ARGUMENT = 5,
    ROBOTWEAX_SRT_ABI_MISMATCH = 6,
    ROBOTWEAX_SRT_INVALID_OPTION = 7,
    ROBOTWEAX_SRT_INVALID_OPTION_VALUE = 8,
    ROBOTWEAX_SRT_ALLOCATION_FAILURE = 9
} robotweax_srt_error;

typedef enum robotweax_srt_packet_kind {
    ROBOTWEAX_SRT_DATA_PACKET = 0,
    ROBOTWEAX_SRT_CONTROL_PACKET = 1
} robotweax_srt_packet_kind;

/**
 * @brief ABI-versioned result produced by robotweax_srt_inspect_packet().
 *
 * Before every call set `struct_size` to `sizeof(robotweax_srt_packet_info)`
 * and `abi_version` to `ROBOTWEAX_SRT_ABI_VERSION`. Data-only fields are
 * meaningful for data packets and control-only fields for control packets.
 * `payload_offset` and `payload_size` are byte ranges in the supplied packet;
 * this structure never owns or retains the packet bytes.
 */
typedef struct robotweax_srt_packet_info {
    uint32_t struct_size;
    uint32_t abi_version;
    robotweax_srt_packet_kind kind;
    uint32_t sequence_number;
    uint32_t message_number;
    uint16_t control_type;
    uint16_t control_subtype;
    uint32_t timestamp;
    uint32_t destination_socket_id;
    size_t payload_offset;
    size_t payload_size;
} robotweax_srt_packet_info;

typedef enum robotweax_srt_socket_option {
    /** Bytes per second; nonnegative. */
    ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH = 0,
    /** Bytes per second; see the documented 0/-1 bandwidth modes. */
    ROBOTWEAX_SRT_OPT_MAXIMUM_BANDWIDTH = 1,
    /** Integer percentage in the supported 0..100 range. */
    ROBOTWEAX_SRT_OPT_OVERHEAD_PERCENT = 2,
    /** Receiver latency in milliseconds. */
    ROBOTWEAX_SRT_OPT_RECEIVER_LATENCY = 3,
    /** Peer latency in milliseconds. */
    ROBOTWEAX_SRT_OPT_PEER_LATENCY = 4,
    /** Sender drop delay in milliseconds. */
    ROBOTWEAX_SRT_OPT_SENDER_DROP_DELAY = 5,
    /** Boolean encoded as 0 or 1. */
    ROBOTWEAX_SRT_OPT_TSBPD_MODE = 6,
    /** Boolean encoded as 0 or 1. */
    ROBOTWEAX_SRT_OPT_TOO_LATE_PACKET_DROP = 7,
    /** Boolean encoded as 0 or 1. */
    ROBOTWEAX_SRT_OPT_PERIODIC_NAK = 8,
    /** Boolean encoded as 0 or 1. */
    ROBOTWEAX_SRT_OPT_RETRANSMIT_FLAG = 9,
    /** Send-buffer capacity in packets. */
    ROBOTWEAX_SRT_OPT_SEND_BUFFER_PACKETS = 10,
    /** Receive-buffer capacity in packets. */
    ROBOTWEAX_SRT_OPT_RECEIVE_BUFFER_PACKETS = 11,
    /** Flow-control window in packets. */
    ROBOTWEAX_SRT_OPT_FLOW_WINDOW_PACKETS = 12,
    /** Maximum application payload in bytes. */
    ROBOTWEAX_SRT_OPT_MAXIMUM_PAYLOAD_SIZE = 13,
    /** Boolean encoded as 0 or 1. */
    ROBOTWEAX_SRT_OPT_RENDEZVOUS = 14,
    /** 1 selects Message API; 0 selects Stream API. */
    ROBOTWEAX_SRT_OPT_MESSAGE_API = 15,
    /** 0 selects Live; 1 selects File transmission. */
    ROBOTWEAX_SRT_OPT_TRANSMISSION_TYPE = 16,
    /** Maximum adaptive reorder tolerance in packets. */
    ROBOTWEAX_SRT_OPT_MAXIMUM_REORDER_TOLERANCE = 17
} robotweax_srt_socket_option;

/**
 * @brief Opaque, caller-owned socket-option value object.
 * @note One object must not be read and written concurrently without external
 * synchronization. Its layout is not part of the ABI.
 */
typedef struct robotweax_srt_options robotweax_srt_options;

/**
 * @brief Decodes the fixed SRT packet header without retaining input bytes.
 * @param[in] bytes Packet bytes, valid for the duration of the call.
 * @param size Packet size in bytes.
 * @param[in,out] info Caller-owned output initialized with the current
 * `struct_size` and `abi_version` values.
 * @return `ROBOTWEAX_SRT_OK` on success or a specific validation error.
 * @note Thread-safe for independent input/output objects; performs no dynamic
 * allocation. The current ABI requires the full fixed-layout structure and
 * writes only that layout.
 */
ROBOTWEAX_SRT_API robotweax_srt_error robotweax_srt_inspect_packet(
    const uint8_t* bytes,
    size_t size,
    robotweax_srt_packet_info* info);

/**
 * @brief Returns stable library-owned text for an error value.
 * @return A NUL-terminated static string that must not be modified or freed.
 */
ROBOTWEAX_SRT_API const char* robotweax_srt_error_string(robotweax_srt_error error);

/**
 * @brief Allocates an option object initialized to protocol defaults.
 * @param[out] options Receives the caller-owned object; set to null when
 * allocation fails.
 */
ROBOTWEAX_SRT_API robotweax_srt_error robotweax_srt_options_create(
    robotweax_srt_options** options);

/** @brief Releases an option object; passing null is safe. */
ROBOTWEAX_SRT_API void robotweax_srt_options_destroy(robotweax_srt_options* options);

/**
 * @brief Validates and stores one integral option value.
 * @return `ROBOTWEAX_SRT_INVALID_OPTION` for an unknown identifier or
 * `ROBOTWEAX_SRT_INVALID_OPTION_VALUE` for an invalid value/combination.
 */
ROBOTWEAX_SRT_API robotweax_srt_error robotweax_srt_options_set(
    robotweax_srt_options* options,
    int option,
    int64_t value);

/**
 * @brief Reads one option value.
 * @param[out] value Receives the current integral value on success.
 */
ROBOTWEAX_SRT_API robotweax_srt_error robotweax_srt_options_get(
    const robotweax_srt_options* options,
    int option,
    int64_t* value);

#ifdef __cplusplus
}
#endif

#endif
