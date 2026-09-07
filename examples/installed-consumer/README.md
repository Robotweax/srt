# Installed CMake package consumer

This directory is a standalone downstream project. It does not add the
Robotweax SRT source tree as a subdirectory and does not include private
headers. Instead, it discovers an installed Robotweax SRT package and links
the public `RobotweaxSRT::srt` target.

The program demonstrates the minimum integration lifecycle:

1. initialize and clean up the SRT runtime;
2. create and close a public SRT socket;
3. set and read a socket option;
4. verify that compile-time and runtime compatibility versions match.

The networked Message, File, and Connection Group examples remain the source
for complete connection, data-transfer, and `srt_bistats` flows. Statistics
require a connection and are deliberately not queried by this network-free
package probe.

## Build from an install prefix

First configure, build, and install Robotweax SRT. The prefix may be a system
installation or an isolated staging directory:

```sh
cmake -S /path/to/robotweax-srt -B /path/to/srt-build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/srt-build --parallel
cmake --install /path/to/srt-build --prefix /path/to/srt-stage
```

Then configure this directory as an independent project:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/srt-stage
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run it directly when desired:

```sh
./build/robotweax_srt_installed_consumer
```

On Windows with a shared build, ensure that the installed runtime directory is
on `PATH`, or copy the Robotweax SRT DLL beside the executable. Multi-config
generators also require the same configuration at build and test time, for
example `--config Release` and `ctest -C Release`.

If OpenSSL is installed in a nonstandard prefix, pass its normal discovery
hint, such as `-DOPENSSL_ROOT_DIR=/path/to/openssl`, while configuring the
consumer. `find_package(RobotweaxSRT CONFIG REQUIRED)` deliberately resolves
the same OpenSSL and thread dependencies exported by the library package.
