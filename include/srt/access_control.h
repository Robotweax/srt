/* SPDX-License-Identifier: MIT */

#ifndef ROBOTWEAX_SRT_COMPAT_ACCESS_CONTROL_H
#define ROBOTWEAX_SRT_COMPAT_ACCESS_CONTROL_H

/* Public application rejection codes compatible with the SRT access-control
 * convention. Applications choose these in a listener callback with
 * srt_setrejectreason(); the constants do not implement access policy. */
#define SRT_REJX_FALLBACK 1000 /* Unclassified application rejection. */
#define SRT_REJX_KEY_NOTSUP 1001 /* Unsupported Stream ID key. */
#define SRT_REJX_FILEPATH 1002 /* Invalid or unavailable file path. */
#define SRT_REJX_HOSTNOTFOUND 1003 /* Unknown requested host. */
#define SRT_REJX_BAD_REQUEST 1400 /* Invalid request syntax. */
#define SRT_REJX_UNAUTHORIZED 1401 /* Authentication unsuccessful. */
#define SRT_REJX_OVERLOAD 1402 /* Service capacity or usage quota exceeded. */
#define SRT_REJX_FORBIDDEN 1403 /* Requested access is denied. */
#define SRT_REJX_NOTFOUND 1404 /* Requested resource is unavailable. */
#define SRT_REJX_BAD_MODE 1405 /* Unsupported requested mode. */
#define SRT_REJX_UNACCEPTABLE 1406 /* Requested parameters cannot be served. */
#define SRT_REJX_CONFLICT 1409 /* Request conflicts with resource state. */
#define SRT_REJX_NOTSUP_MEDIA 1415 /* Unsupported media type. */
#define SRT_REJX_LOCKED 1423 /* Resource access is locked. */
#define SRT_REJX_FAILED_DEPEND 1424 /* Required dependent session is gone. */
#define SRT_REJX_ISE 1500 /* Internal service failure. */
#define SRT_REJX_UNIMPLEMENTED                                                 \
    1501 /* Recognized operation is not implemented. */
#define SRT_REJX_GW 1502 /* Gateway destination rejected the request. */
#define SRT_REJX_DOWN 1503 /* Service is temporarily unavailable. */
#define SRT_REJX_VERSION 1505 /* Unsupported SRT version. */
#define SRT_REJX_NOROOM 1507 /* Insufficient storage capacity. */

#endif
