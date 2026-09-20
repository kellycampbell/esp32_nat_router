"""Layer the HaLow sdkconfig overlay onto an ESP-IDF PlatformIO build.

pioarduino's espidf builder passes -DSDKCONFIG=<project>/sdkconfig.<env> to
CMake but never -DSDKCONFIG_DEFAULTS, so ESP-IDF falls back to plain
"sdkconfig.defaults" (project.cmake). Its `custom_sdkconfig` option is not an
alternative: write_sdkconfig_file() returns early unless the Arduino framework
is in use.

ESP-IDF does honour the SDKCONFIG_DEFAULTS *environment* variable, so set it
here. Note that doing so replaces the fallback rather than adding to it, which
is why sdkconfig.defaults is listed explicitly and first.

For every entry, ESP-IDF also loads "<entry>.<idf_target>" when that file
exists, so sdkconfig.defaults.esp32s3 keeps working and a
sdkconfig.defaults.halow.esp32s3 would be picked up too.

Select the Morse Micro board profile per env with `custom_halow_profile`; it
supplies the SPI pin map, chip variant and BCF filename.
"""

import os
from pathlib import Path

Import("env")  # noqa: F821  (injected by SCons)

project_dir = Path(env.subst("$PROJECT_DIR"))  # noqa: F821

defaults = [
    project_dir / "sdkconfig.defaults",
    project_dir / "sdkconfig.defaults.halow",
]

profile = env.GetProjectOption("custom_halow_profile", "")  # noqa: F821
if profile:
    profile_path = Path(profile)
    if not profile_path.is_absolute():
        profile_path = project_dir / profile_path
    if profile_path.is_file():
        defaults.append(profile_path)
    else:
        # Board profiles live under managed_components, which is populated by
        # the IDF component manager during the build. Warn rather than fail so
        # a clean checkout can still reach the point where it gets fetched.
        print("halow_sdkconfig: board profile not found, skipping:", profile_path)

missing = [str(p) for p in defaults if not p.is_file()]
if missing:
    # ESP-IDF raises FATAL_ERROR on a non-existent SDKCONFIG_DEFAULTS entry,
    # so fail here with a message that names the file instead.
    raise SystemExit("halow_sdkconfig: missing required defaults: " + ", ".join(missing))

os.environ["SDKCONFIG_DEFAULTS"] = ";".join(str(p) for p in defaults)
print("halow_sdkconfig: SDKCONFIG_DEFAULTS =", os.environ["SDKCONFIG_DEFAULTS"])
