#!/bin/sh
# CMake caches contain absolute source, output and dependency paths. Preserve a
# copied/moved build before configuring a new one at the destination.
set -eu

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <source-directory> <build-directory> [cmake-options...]" >&2
    exit 2
fi

source_dir=$(CDPATH= cd -- "$1" && pwd -P)
build_dir=$(realpath -m -- "$2")
shift 2

if [ ! -f "$source_dir/CMakeLists.txt" ]; then
    echo "No CMakeLists.txt in source directory: $source_dir" >&2
    exit 2
fi
if [ "$build_dir" = / ]; then
    echo "The filesystem root cannot be used as the build directory." >&2
    exit 2
fi
case "$source_dir/" in
    "$build_dir/"*)
        echo "Build directory must not be the source directory or one of its ancestors." >&2
        exit 2
        ;;
esac
case "$build_dir/" in
    */.git/*)
        echo "Build directory must not be inside Git metadata." >&2
        exit 2
        ;;
esac
if [ -f "$build_dir/CMakeLists.txt" ]; then
    echo "Refusing to use another source directory as the build directory: $build_dir" >&2
    exit 2
fi

cache="$build_dir/CMakeCache.txt"
if [ -f "$cache" ]; then
    cached_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")
    cached_build=$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$cache")
    if { [ -n "$cached_source" ] && [ "$cached_source" != "$source_dir" ]; } ||
       { [ -n "$cached_build" ] && [ "$cached_build" != "$build_dir" ]; }; then
        backup="$build_dir.stale"
        suffix=1
        while [ -e "$backup" ] || [ -L "$backup" ]; do
            backup="$build_dir.stale-$suffix"
            suffix=$((suffix + 1))
        done
        mv -- "$build_dir" "$backup"
        printf 'Copied or moved CMake cache detected. Previous build preserved at: %s\n' "$backup"
    fi
fi

exec cmake -S "$source_dir" -B "$build_dir" "$@"
