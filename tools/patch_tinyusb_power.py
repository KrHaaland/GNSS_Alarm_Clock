# PlatformIO pre-script: make Adafruit TinyUSB honor USB_CONFIG_POWER.
#
# The pinned TinyUSB 2.4.1 (see platformio.ini for why it is pinned) defines a
# USB_CONFIG_POWER macro but never uses it - bMaxPower in the config descriptor
# is a hardcoded 100 (mA). The clock draws more than that (MCU + GNSS +
# backlight + supercap charging), so patch the env's libdeps copy to pass the
# macro instead; the actual value comes from -DUSB_CONFIG_POWER in build_flags.
# Idempotent, and re-applies automatically if libdeps is regenerated.
import os
import re

Import("env")  # noqa: F821 - provided by PlatformIO/SCons

# Both copies must be patched: the lib_deps 2.4.1 copy AND the copy bundled
# inside the Adafruit SAMD core package — the build compiles both and the
# linker has been observed picking the bundled one for these symbols.
CANDIDATES = [
    os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"],
                 "Adafruit TinyUSB Library", "src", "arduino",
                 "Adafruit_USBD_Device.cpp"),
    os.path.join(
        env.PioPlatform().get_package_dir("framework-arduino-samd-adafruit")
        or "", "libraries", "Adafruit_TinyUSB_Arduino", "src", "arduino",
        "Adafruit_USBD_Device.cpp"),
]

for F in CANDIDATES:
    try:
        with open(F) as f:
            src = f.read()
    except (FileNotFoundError, NotADirectoryError):
        print("patch_tinyusb_power: not found (yet): %s" % F)
        continue
    if "USB_CONFIG_POWER)," in src:
        continue  # already patched
    new, n = re.subn(
        r"(TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP \| TU_BIT\(7\),\s*\n\s*)100\)",
        lambda m: m.group(1) + "USB_CONFIG_POWER)",
        src, count=1)
    if n == 1:
        with open(F, "w") as f:
            f.write(new)
        print("patch_tinyusb_power: patched %s" % F)
    else:
        print("patch_tinyusb_power: WARNING - pattern not found in %s "
              "(TinyUSB version changed?); bMaxPower stays 100 mA" % F)
