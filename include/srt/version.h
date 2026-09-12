/* SPDX-License-Identifier: MIT */

#ifndef ROBOTWEAX_SRT_COMPAT_VERSION_H
#define ROBOTWEAX_SRT_COMPAT_VERSION_H

/*
 * Public API compatibility target.
 *
 * This is deliberately separate from Robotweax SRT's own release version.  The
 * value identifies the public SRT API revision that this compatibility layer
 * is tested against.
 */
#define SRT_MAKE_VERSION(major, minor, patch) \
    ((patch) + ((minor) * 0x100) + ((major) * 0x10000))
#define SRT_MAKE_VERSION_VALUE SRT_MAKE_VERSION

#define SRT_VERSION_MAJOR 1
#define SRT_VERSION_MINOR 5
#define SRT_VERSION_PATCH 7
#define SRT_VERSION_STRING "1.5.7"
#define SRT_VERSION_VALUE \
    SRT_MAKE_VERSION_VALUE( \
        SRT_VERSION_MAJOR, SRT_VERSION_MINOR, SRT_VERSION_PATCH)

/* Local implementation release, not the wire/API compatibility version. */
#define ROBOTWEAX_SRT_VERSION_MAJOR 0
#define ROBOTWEAX_SRT_VERSION_MINOR 2
#define ROBOTWEAX_SRT_VERSION_PATCH 4
#define ROBOTWEAX_SRT_VERSION_STRING "0.2.4"
#define ROBOTWEAX_SRT_VERSION_VALUE                                            \
    SRT_MAKE_VERSION_VALUE(ROBOTWEAX_SRT_VERSION_MAJOR,                        \
        ROBOTWEAX_SRT_VERSION_MINOR, ROBOTWEAX_SRT_VERSION_PATCH)

#endif
