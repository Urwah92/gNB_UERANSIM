#!/usr/bin/env python3
"""Check build-cache relocation with tiny real CMake projects, without compiling UERANSIM."""

import pathlib
import shutil
import subprocess
import sys
import tempfile


PROJECT = """cmake_minimum_required(VERSION 3.17)
project(relocation_fixture LANGUAGES NONE)
set(RELOCATION_VALUE "default" CACHE STRING "Argument preservation marker")
file(WRITE "${CMAKE_BINARY_DIR}/fixture-value.txt" "${RELOCATION_VALUE}")
"""
GENERATOR = "Unix Makefiles"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def fixture(folder):
    folder.mkdir(parents=True)
    (folder / "CMakeLists.txt").write_text(PROJECT)
    return folder


def snapshot(folder):
    """Compare all file contents and directory entries, without relying on mtimes."""
    result = {}
    for path in sorted(folder.rglob("*")):
        relative = str(path.relative_to(folder))
        if path.is_symlink():
            result[relative] = ("symlink", str(path.readlink()))
        elif path.is_dir():
            result[relative] = ("directory",)
        else:
            result[relative] = ("file", path.read_bytes())
    return result


def cache_value(build, key):
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(key + ":"):
            return line.split("=", 1)[1]
    raise AssertionError(f"Missing {key} in {build}/CMakeCache.txt")


def backups(build):
    return sorted(build.parent.glob(build.name + ".stale*"))


def configure(wrapper, source, build, value="argument with spaces;literal", cwd=None, valid=True):
    result = subprocess.run(
        ["sh", str(wrapper), str(source), str(build), "-G", GENERATOR,
         "-DCMAKE_BUILD_TYPE=Debug", f"-DRELOCATION_VALUE:STRING={value}"],
        cwd=cwd, capture_output=True, text=True, timeout=15,
    )
    require((result.returncode == 0) == valid,
            f"Unexpected configure exit {result.returncode} for source={source}, build={build}:\n"
            f"{result.stdout}\n{result.stderr}")
    if valid:
        require(cache_value(build, "CMAKE_HOME_DIRECTORY") == str(source.resolve()),
                "Fresh cache points to the wrong source directory")
        require(cache_value(build, "CMAKE_CACHEFILE_DIR") == str(build.resolve()),
                "Fresh cache points to the wrong build directory")
        require(cache_value(build, "CMAKE_GENERATOR") == GENERATOR, "Generator arguments were lost")
        require(cache_value(build, "CMAKE_BUILD_TYPE") == "Debug", "CMake definition was lost")
        require((build / "fixture-value.txt").read_text() == value,
                "CMake argument quoting changed spaces/semicolons")
    return result


def preserve_marker(build):
    (build / "keep me.txt").write_text("original build artifact\n")
    (build / "preserved nested artifacts").mkdir()
    (build / "preserved nested artifacts" / "payload.bin").write_bytes(bytes(range(256)))
    return snapshot(build)


def preserved_backup(build, expected):
    archived = backups(build)
    require(len(archived) == 1, f"Expected one retained stale build beside {build}, found {archived}")
    require(snapshot(archived[0]) == expected, "Stale build contents were modified or lost during relocation")
    require(not (build / "keep me.txt").exists(), "Fresh build unexpectedly reuses stale artifacts")


def unchanged_cache(wrapper, root):
    source = fixture(root / "unchanged source with spaces")
    build = source / "build with spaces"
    configure(wrapper, source, build)
    preserve_marker(build)
    configure(wrapper, source, build, value="second configure with spaces")
    require(not backups(build), "Unchanged build cache was incorrectly archived")
    require((build / "keep me.txt").read_text() == "original build artifact\n",
            "Unchanged build lost existing artifacts")
    # Equivalent physical paths should also reuse the same cache.
    alias = root / "build alias with spaces"
    alias.symlink_to(build, target_is_directory=True)
    configure(wrapper, source, alias, value="symlink cache reuse")
    require(not backups(build) and not backups(alias), "Path alias caused a false relocation")
    require((build / "keep me.txt").exists(), "Path alias replaced an unchanged build")


def copied_tree(wrapper, root):
    original = fixture(root / "original checkout with spaces")
    original_build = original / "build"
    configure(wrapper, original, original_build, value="original checkout")
    expected = preserve_marker(original_build)
    copy = root / "copied checkout with spaces"
    shutil.copytree(original, copy)
    copied_build = copy / "build"
    configure(wrapper, copy, copied_build, value="copied checkout")
    preserved_backup(copied_build, expected)
    require(snapshot(original_build) == expected, "Configuring the copy modified the original build")


def moved_tree(wrapper, root):
    original = fixture(root / "tree before move")
    configure(wrapper, original, original / "build")
    expected = preserve_marker(original / "build")
    moved = root / "tree after move"
    original.rename(moved)
    configure(wrapper, moved, moved / "build", value="relocated whole source tree")
    preserved_backup(moved / "build", expected)


def moved_build_same_source(wrapper, root):
    source = fixture(root / "unchanged source external build")
    old_build = root / "external build before move"
    configure(wrapper, source, old_build)
    expected = preserve_marker(old_build)
    moved_build = root / "external build after move"
    old_build.rename(moved_build)
    configure(wrapper, source, moved_build, value="same source new build path")
    preserved_backup(moved_build, expected)


def existing_backup(wrapper, root):
    source = fixture(root / "backup collision before move")
    configure(wrapper, source, source / "build")
    expected = preserve_marker(source / "build")
    moved = root / "backup collision after move"
    source.rename(moved)
    previous_backup = moved / "build.stale"
    previous_backup.mkdir()
    (previous_backup / "keep previous backup.txt").write_text("earlier retained build\n")
    previous = snapshot(previous_backup)
    configure(wrapper, moved, moved / "build")
    require(snapshot(previous_backup) == previous, "An existing stale backup was overwritten")
    created = [path for path in backups(moved / "build") if path != previous_backup]
    require(len(created) == 1 and snapshot(created[0]) == expected,
            "A second stale build was not preserved in its own backup")


def unsafe_locations(wrapper, root):
    source = fixture(root / "unsafe in source build")
    (source / "do not modify.txt").write_text("source contents\n")
    before = snapshot(source)
    configure(wrapper, source, source, valid=False)
    require(snapshot(source) == before and not backups(source), "In-source refusal modified source files")
    configure(wrapper, pathlib.Path("."), pathlib.Path("."), cwd=source, valid=False)
    require(snapshot(source) == before and not backups(source), "Relative in-source refusal modified source files")
    alias = root / "unsafe alias"
    alias.symlink_to(source, target_is_directory=True)
    configure(wrapper, source, alias, valid=False)
    require(snapshot(source) == before and not backups(alias), "In-source alias refusal modified source files")

    parent = root / "unsafe ancestor build"
    child = fixture(parent / "source")
    before = snapshot(parent)
    configure(wrapper, child, parent, valid=False)
    require(parent.exists() and snapshot(parent) == before and not backups(parent),
            "Ancestor build refusal modified the source tree")

    other_source = fixture(root / "another source project")
    before = snapshot(other_source)
    configure(wrapper, source, other_source, valid=False)
    require(snapshot(other_source) == before, "Refusing another source project modified it")
    metadata = source / ".git"
    metadata.mkdir()
    (metadata / "sentinel").write_text("Git metadata must remain untouched\n")
    before = snapshot(source)
    configure(wrapper, source, metadata / "build", valid=False)
    require(snapshot(source) == before, "Git metadata refusal modified the source tree")


def main():
    wrapper = pathlib.Path(sys.argv[1]).resolve()
    require(wrapper.is_file(), f"Configure wrapper is missing: {wrapper}")
    require(shutil.which("cmake") is not None, "CMake is required for relocation tests")
    with tempfile.TemporaryDirectory(prefix="ueransim build relocation ") as temporary:
        root = pathlib.Path(temporary)
        unchanged_cache(wrapper, root)
        copied_tree(wrapper, root)
        moved_tree(wrapper, root)
        moved_build_same_source(wrapper, root)
        existing_backup(wrapper, root)
        unsafe_locations(wrapper, root)
    print("Build relocation tests passed (cache reuse, copied/moved paths, spaces, arguments, safe refusal)")


if __name__ == "__main__":
    main()
