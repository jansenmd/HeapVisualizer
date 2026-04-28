# HeapVisualizer

HeapVisualizer is a Qt Widgets GUI application scaffold built with CMake.

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
