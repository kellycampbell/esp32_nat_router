"""Produce the HaLow component's CMake-time artifacts for a PlatformIO build.

pioarduino's espidf builder configures the ESP-IDF CMake project but then
compiles and links with SCons, reading only the source/flag lists out of the
CMake reply. Anything a component builds with add_custom_command or
add_custom_target is therefore never run, and morsemicro__halow relies on two
such steps:

  * components/firmware converts the board's BCF blob from .bin to .mbin and
    embeds both it and the transceiver .mbin as generated .S stubs. The builder
    does drive ninja for generated sources (_ensure_generated_sources), but it
    swallows failures, and convert-bin-to-mbin.py needs pyelftools, which is
    absent from pioarduino's ESP-IDF venv even though ESP-IDF's own
    requirements.core.txt lists it. Without it the build dies later with
    "Source `.../bcf_<board>.bin.mbin.S' not found".

  * components/morselib merges liblibmorse.a with libmmhostap.a and runs
    librarymangler.py over the result, publishing the merged libmorse.a as an
    IMPORTED library. SCons links (and ldgen reads) that path, so a build
    without it fails in ldgen with "libmorse.a: No such file".

The IMPORTED target has a second consequence: pioarduino builds its link list
from the CMake file API, which reports real library targets but not the
IMPORTED_LOCATION of imported ones. libmorse.a and the two archives reachable
only through morselib's INTERFACE link libraries (mmpktmem, mmutils) are
therefore absent from the link. That goes unnoticed while nothing references
mmhalow.c -- it sits unextracted in libmorsemicro__halow.a -- and turns into a
wall of undefined mmwlan_*/mmpkt_* symbols the moment the application calls
into the HaLow API. So this script also appends those archives to LIBS.

This runs as a post: script, which is still SConscript-evaluation time: the
CMake project has been configured and build.ninja exists, yet SCons has not
started executing, so files created here are in place before the first compile.

ninja is incremental, so on an up-to-date tree both targets are no-ops. Note
that ESP-IDF makes these custom commands order-only dependent on every
component archive, so the first run after a clean also builds the IDF tree
under CMake in addition to SCons' own copy.
"""

import re
import subprocess
from pathlib import Path

Import("env")  # noqa: F821  (injected by SCons)

# Component-relative ninja targets. libmorsefirmware.a is named rather than the
# .S stubs themselves so the BCF filename (a per-board sdkconfig value) does not
# have to be rediscovered here; building it generates the stubs SCons compiles.
NINJA_TARGETS = [
    "esp-idf/morsemicro__halow/components/firmware/libmorsefirmware.a",
    "esp-idf/morsemicro__halow/components/morselib/libmorse.a",
    "esp-idf/morsemicro__halow/components/mmpktmem/libmmpktmem.a",
    "esp-idf/morsemicro__halow/components/mmutils/libmmutils.a",
]

# Archives the CMake file API does not surface, in the order they go on the link
# line. liblibmorse.a (unmangled) is deliberately excluded: librarymangler.py
# renames everything outside protected_syms.txt, so only the merged, mangled
# libmorse.a resolves against the mmwlan_* symbols mmhalow.c calls.
IMPORTED_ARCHIVES = [
    "esp-idf/morsemicro__halow/components/morselib/libmorse.a",
    "esp-idf/morsemicro__halow/components/mmpktmem/libmmpktmem.a",
    "esp-idf/morsemicro__halow/components/mmutils/libmmutils.a",
]

build_dir = Path(env.subst("$BUILD_DIR"))  # noqa: F821
ninja_buildfile = build_dir / "build.ninja"

if not ninja_buildfile.is_file():
    print("halow_cmake_artifacts: no build.ninja yet, skipping")
    Return()  # noqa: F821


def idf_python():
    """The interpreter ninja runs convert-bin-to-mbin.py with, per build.ninja."""
    for line in ninja_buildfile.read_text(encoding="utf8", errors="replace").splitlines():
        if "convert-bin-to-mbin.py" in line and line.lstrip().startswith("COMMAND ="):
            match = re.search(r"&&\s+(\S+python\S*)\s", line)
            if match:
                return match.group(1)
    return ""


def ensure_pyelftools(python_exe):
    try:
        subprocess.check_call(
            [python_exe, "-c", "import elftools"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return
    except (subprocess.CalledProcessError, OSError):
        pass

    core_dir = Path(env.subst("$PROJECT_CORE_DIR"))  # noqa: F821
    uv_exe = core_dir / "penv" / "bin" / "uv"
    if not uv_exe.is_file():
        uv_exe = core_dir / "penv" / "Scripts" / "uv.exe"
    install = (
        [str(uv_exe), "pip", "install", "--python", python_exe, "pyelftools"]
        if uv_exe.is_file()
        else [python_exe, "-m", "pip", "install", "pyelftools"]
    )
    print("halow_cmake_artifacts: installing pyelftools for the BCF converter")
    subprocess.check_call(install)


python_exe = idf_python()
if python_exe:
    ensure_pyelftools(python_exe)

ninja_exe = Path(env.PioPlatform().get_package_dir("tool-ninja") or "") / "ninja"  # noqa: F821
print("halow_cmake_artifacts: building", ", ".join(NINJA_TARGETS))
subprocess.check_call([str(ninja_exe), "-C", str(build_dir), *NINJA_TARGETS])

# File nodes rather than plain strings: SCons turns a bare string in LIBS into
# -l<name>, which ld then cannot find. LIBS lands in $_LIBFLAGS, last on the
# link line, after every object and ESP-IDF archive that needs these symbols.
#
# --start-group cannot be expressed through LIBS for the same reason, so the
# list is simply repeated instead: morselib and mmpktmem reference each other,
# and a second pass resolves what the first left open.
archives = [env.File(str(build_dir / a)) for a in IMPORTED_ARCHIVES]  # noqa: F821
env.Append(LIBS=archives + archives)  # noqa: F821
