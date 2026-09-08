// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH
extern "C" int late_cleanup_probe(const char* mode);

int main(int argc, char** argv)
{
    return late_cleanup_probe(argc > 1 ? argv[1] : "socket");
}
