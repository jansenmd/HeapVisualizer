# HeapVisualizer

HeapVisualizer is a Qt Widgets GUI application scaffold built with CMake.

The project also builds `HeapAllocationTest`, a second Qt GUI application for manually allocating and freeing heap memory blocks. This will eventually provide test data for the main visualizer.

On Linux, the project also builds `libheap_allocation_hooks.so`, an `LD_PRELOAD` shared library that hooks common libc allocation/deallocation entry points while the allocation test app runs. C++ heap allocations are still visible when the C++ runtime services them through those libc allocation functions.

## Version

Current build version: `0.1.0`

The About dialog shows the project version and the CMake configure date for the build.

## Build

Linux:

```sh
./scripts/build-linux-debug.sh
./scripts/build-linux-release.sh
```

Windows cross-build from Linux/WSL with MinGW-w64:

```sh
export QT_ROOT='C:/Qt/6.x.x/mingw_64'
./scripts/build-windows-debug.sh
./scripts/build-windows-release.sh
```

All configurations:

```sh
./scripts/build-all.sh
```

If CMake cannot find Qt, it prints target-specific instructions for installing the required Qt development resources.

## Allocation Hook Test

Launch the allocation test app with the preload library injected:

```sh
./scripts/run-allocation-test-preload.sh debug
```

Use `release` instead of `debug` to run the release build. The preload library prints a load message and allocation counter summary to stderr.

The preload library also attempts to stream allocation snapshots once per second to `127.0.0.1:47329`. Override the destination with:

```sh
export HEAP_VISUALIZER_STREAM_HOST=127.0.0.1
export HEAP_VISUALIZER_STREAM_PORT=47329
```

In `HeapVisualizer`, use `File -> Start Streaming` and enter the same IP address and port.
