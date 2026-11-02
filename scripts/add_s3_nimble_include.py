from pathlib import Path


Import("env")  # noqa: F821 -- provided by PlatformIO

if env.BoardConfig().get("build.mcu") == "esp32s3":
    framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs"))
    nimble_headers = framework / "esp32s3/include/bt/include/esp32c3/include"
    if not (nimble_headers / "esp_bt.h").is_file():
        raise RuntimeError(f"ESP32-S3 NimBLE header is missing: {nimble_headers / 'esp_bt.h'}")
    env.Append(CPPPATH=[str(nimble_headers)])