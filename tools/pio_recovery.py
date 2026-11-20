# PlatformIO extra script: flash the TraceKit recovery firmware into the
# `ota_0` partition together with the application (docs/PLANO.md 4.14, D26).
#
#   [env:esp32-s3]
#   board_build.partitions = <one of partitions/tracekit_<size>.csv>
#   extra_scripts = post:<path to>/tools/pio_recovery.py
#
# `pio run -t upload` then also writes the recovery, building recovery/ for
# the board's chip first when its binary is missing or older than its
# sources. `pio run -t recovery` builds and flashes only the recovery.
#
# The application itself still goes to `factory`, exactly where PlatformIO
# puts it without this script.

import csv
import inspect
import os
import subprocess

Import("env")  # noqa: F821  (SCons)

def _library_root():
    """The TraceKit checkout this script belongs to."""
    if os.environ.get("TRACEKIT_ROOT"):
        return os.path.abspath(os.environ["TRACEKIT_ROOT"])
    try:
        here = inspect.getframeinfo(inspect.currentframe()).filename
        root = os.path.dirname(os.path.dirname(os.path.abspath(here)))
        if os.path.isfile(os.path.join(root, "recovery", "platformio.ini")):
            return root
    except Exception:  # SCons may exec the script without a usable frame
        pass
    # Fall back to walking up from the project.
    directory = env.subst("$PROJECT_DIR")  # noqa: F821
    while True:
        if os.path.isfile(os.path.join(directory, "recovery", "platformio.ini")):
            return directory
        parent = os.path.dirname(directory)
        if parent == directory:
            raise SystemExit("pio_recovery.py: TraceKit not found; set TRACEKIT_ROOT")
        directory = parent


def _recovery_offset(table):
    """Offset and size of the app/ota_0 partition in a partitions CSV."""
    with open(table, newline="") as handle:
        rows = [r for r in csv.reader(handle) if r and not r[0].strip().startswith("#")]
    for row in rows:
        fields = [f.strip() for f in row] + [""] * 5
        if fields[1] == "app" and fields[2] == "ota_0":
            if not fields[3]:
                raise SystemExit("pio_recovery.py: give ota_0 an explicit offset in %s" % table)
            return int(fields[3], 0), int(fields[4], 0)
    raise SystemExit(
        "pio_recovery.py: %s has no app/ota_0 partition for the recovery. "
        "Use one of partitions/tracekit_<size>.csv." % table)


def _newest(paths):
    newest = 0.0
    for path in paths:
        if os.path.isfile(path):
            newest = max(newest, os.path.getmtime(path))
            continue
        for directory, _, files in os.walk(path):
            if ".pio" in directory:
                continue
            for name in files:
                newest = max(newest, os.path.getmtime(os.path.join(directory, name)))
    return newest


ROOT = _library_root()
RECOVERY_DIR = os.path.join(ROOT, "recovery")
MCU = env.BoardConfig().get("build.mcu")  # noqa: F821
RECOVERY_BIN = os.path.join(RECOVERY_DIR, ".pio", "build", MCU, "firmware.bin")
OFFSET, SIZE = _recovery_offset(env.subst("$PARTITIONS_TABLE_CSV"))  # noqa: F821
SOURCES = [
    os.path.join(RECOVERY_DIR, "main"),
    os.path.join(RECOVERY_DIR, "CMakeLists.txt"),
    os.path.join(RECOVERY_DIR, "platformio.ini"),
    os.path.join(RECOVERY_DIR, "sdkconfig.defaults"),
    os.path.join(ROOT, "src", "update"),
    os.path.join(ROOT, "src", "security"),
    os.path.join(ROOT, "src", "transport", "WifiNetworks.cpp"),
]


def build_recovery(*_args, **_kwargs):
    stale = not os.path.isfile(RECOVERY_BIN) or os.path.getmtime(RECOVERY_BIN) < _newest(SOURCES)
    if stale:
        print("TraceKit recovery: building recovery/ for %s (once per change)" % MCU)
        python = env.subst("$PYTHONEXE")  # noqa: F821
        result = subprocess.call([python, "-m", "platformio", "run", "-d", RECOVERY_DIR, "-e", MCU])
        if result != 0 or not os.path.isfile(RECOVERY_BIN):
            raise SystemExit("TraceKit recovery: the build failed (pio run -d %s -e %s)" % (RECOVERY_DIR, MCU))
    size = os.path.getsize(RECOVERY_BIN)
    if size > SIZE:
        raise SystemExit("TraceKit recovery: %d bytes, over the %d-byte ota_0 partition" % (size, SIZE))
    print("TraceKit recovery: %s, %d KB of %d KB, at 0x%x" % (MCU, size // 1024, SIZE // 1024, OFFSET))
    return 0


# `pio run -t upload`: one more image for esptool, after the app's.
if "uploadfs" not in COMMAND_LINE_TARGETS:  # noqa: F821
    env.Append(  # noqa: F821
        FLASH_EXTRA_IMAGES=[(hex(OFFSET), RECOVERY_BIN)],
        UPLOADERFLAGS=[hex(OFFSET), RECOVERY_BIN],
    )
    env.AddPreAction("upload", build_recovery)  # noqa: F821

# `pio run -t recovery`: only the recovery, nothing else touched.
env.AddCustomTarget(  # noqa: F821
    name="recovery",
    dependencies=None,
    actions=[
        build_recovery,
        env.VerboseAction(env.AutodetectUploadPort, "Looking for upload port..."),  # noqa: F821
        '"$PYTHONEXE" "$UPLOADER" --chip %s --port "$UPLOAD_PORT" --baud $UPLOAD_SPEED write_flash -z 0x%x "%s"'
        % (MCU, OFFSET, RECOVERY_BIN),
    ],
    title="TraceKit recovery",
    description="Build recovery/ for this chip and flash it into ota_0",
)
