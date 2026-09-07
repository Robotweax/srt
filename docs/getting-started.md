# Getting started

This guide takes a new developer from a clean checkout to a tested Robotweax
SRT 0.2.3 build and outlines the first application integration steps.

## 1. Install prerequisites

You need:

- Git;
- CMake 3.20 or newer;
- a C++20-capable compiler;
- OpenSSL 3.x headers and libraries;
- Python 3.10 or newer when tests are enabled.

The supported build matrix covers Linux, macOS, and Windows. Make sure CMake
can find the same OpenSSL installation for all configurations you intend to
ship.

## 2. Clone and configure

```sh
git clone https://github.com/Robotweax/srt.git robotweax-srt
cd robotweax-srt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Keep builds out of the source tree. Use a separate build directory whenever
you change important dimensions such as static versus shared linkage or the
AES-GCM extension.

For Visual Studio and other multi-config generators, configure without
`CMAKE_BUILD_TYPE`:

```sh
cmake -S . -B build
```

## 3. Build

Single-config generators:

```sh
cmake --build build --parallel
```

Multi-config generators:

```sh
cmake --build build --config Release --parallel
```

The source-tree CMake target is `robotweax_srt`. Installed consumers use the
namespaced target `RobotweaxSRT::srt`.

## 4. Run the tests

Single-config generators:

```sh
ctest --test-dir build --output-on-failure
```

Multi-config generators:

```sh
ctest --test-dir build -C Release --output-on-failure
```

The default static development build enables the unit and integration test
suite. See [Testing](testing.md) for focused runs and package-consumer checks.

## 5. Understand the public lifecycle

Applications normally include the installed compatibility header:

```c
#include <srt/srt.h>
```

The process-level lifecycle is:

1. call `srt_startup()` before using sockets;
2. create a socket with `srt_create_socket()` or `srt_socket()`;
3. set connection-stage options;
4. establish a Caller/Listener or Rendezvous connection;
5. exchange data through Message or Stream APIs;
6. close every socket with `srt_close()`;
7. call `srt_cleanup()` when the process no longer uses SRT.

A minimal lifecycle check looks like this:

```c
#include <srt/srt.h>

int main(void)
{
    if (srt_startup() == SRT_ERROR) {
        return 1;
    }

    SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        (void)srt_cleanup();
        return 2;
    }

    int result = srt_close(socket);
    int cleanup = srt_cleanup();
    return result == SRT_ERROR || cleanup == SRT_ERROR ? 3 : 0;
}
```

## 6. Choose an endpoint role

Caller/Listener is the usual topology:

- the Listener binds, calls `srt_listen()`, and accepts a connected socket;
- the Caller optionally binds a source address and calls `srt_connect()`;
- data flows over the connected Caller and accepted Listener sockets.

Rendezvous has both endpoints bind local addresses and connect simultaneously.
It requires explicit `SRTO_RENDEZVOUS` configuration or the
`srt_rendezvous()` convenience function. See [Rendezvous](rendezvous.md).

Robotweax SRT 0.2 supports HSv5 connection establishment. Peers must present a
coherent HSv5 identity corresponding to SRT 1.3.0 or newer. There is no
automatic fallback to positive HSv4 establishment.

## 7. Choose a data mode

Live/Message mode preserves message boundaries and is the normal low-latency
transport profile. File/Stream mode provides ordered byte-stream semantics,
partial reads and writes, zero-byte end-of-stream behavior, and file helpers.

Set `SRTO_TRANSTYPE`, `SRTO_MESSAGEAPI`, congestion control, latency, and
encryption options before connection establishment. Do not change modes on an
already connected socket.

## 8. Add the installed package to an application

Install Robotweax SRT to a prefix:

```sh
cmake --install build --prefix "$PWD/stage"
```

Then use the exported target:

```cmake
find_package(RobotweaxSRT 0.2 CONFIG REQUIRED)
target_link_libraries(my_srt_app PRIVATE RobotweaxSRT::srt)
```

Pass the installation prefix through `CMAKE_PREFIX_PATH` if it is not in a
standard location.

## Next steps

- Follow the complete [Integration guide](integration.md).
- Review [Building and installing](building.md) for deployment profiles.
- Select options with the [Socket options](socket-options.md) reference.
- Review [Known limitations](limitations.md) before production qualification.
