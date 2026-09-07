// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH

#include <climits>

int main(int argc, char**)
{
    // No argument is a healthy control. An argument deliberately triggers
    // runtime signed overflow; recovery would reach the erroneous exit 0.
    volatile int maximum = INT_MAX;
    volatile int result = maximum + (argc > 1 ? 1 : 0);
    (void)result;
    return 0;
}
