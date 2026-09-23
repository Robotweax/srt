// SPDX-License-Identifier: MIT
// Read-only check that the tested OBS process owns an on-screen main window.
#include <CoreGraphics/CoreGraphics.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    char* end = NULL;
    errno = 0;
    long requested = strtol(argv[1], &end, 10);
    if (errno || !end || *end || requested <= 0 || requested > INT_MAX)
        return 2;

    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!windows) {
        fputs("cannot inspect on-screen windows\n", stderr);
        return 2;
    }
    int matches = 0;
    for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
        CFDictionaryRef window = CFArrayGetValueAtIndex(windows, index);
        CFNumberRef owner = CFDictionaryGetValue(window, kCGWindowOwnerPID);
        CFNumberRef layer = CFDictionaryGetValue(window, kCGWindowLayer);
        CFDictionaryRef bounds = CFDictionaryGetValue(window, kCGWindowBounds);
        int pid = 0, level = -1;
        CGRect rectangle = CGRectZero;
        if (owner && layer && bounds
            && CFNumberGetValue(owner, kCFNumberIntType, &pid)
            && CFNumberGetValue(layer, kCFNumberIntType, &level)
            && CGRectMakeWithDictionaryRepresentation(bounds, &rectangle)
            && pid == requested && level == 0 && rectangle.size.width > 0
            && rectangle.size.height > 0)
            ++matches;
    }
    CFRelease(windows);
    printf("WINDOW pid=%ld visible=%d\n", requested, matches);
    return matches ? 0 : 1;
}
