# OV3660 Camera FPS Benchmark

Benchmark run on ESP32-CAM with OV3660 sensor.

## Test Config

| Setting | Value |
|---|---:|
| ESP-IDF | v5.4 |
| CPU frequency | 160 MHz |
| PSRAM | 8 MB detected, 4 MB mapped |
| Pixel format | JPEG |
| JPEG quality | 63 |
| XCLK | 20 MHz |
| Frame buffers | 4 |
| Grab mode | CAMERA_GRAB_LATEST |
| Test duration | 5000 ms per resolution |
| WiFi/WebRTC/Anedya | Disabled |

## Results

| Resolution | Size | Frames | FPS | Avg frame | Avg JPEG | Invalid | Failures |
|---|---:|---:|---:|---:|---:|---:|---:|
| 96X96 | 96x96 | 160 | 31.95 | 31.30 ms | 1,024 B | 0 | 0 |
| QQVGA | 160x120 | 143 | 28.55 | 35.03 ms | 1,379 B | 0 | 0 |
| 128X128 | 128x128 | 161 | 32.02 | 31.23 ms | 3,611 B | 0 | 0 |
| QCIF | 176x144 | 151 | 30.06 | 33.26 ms | 3,611 B | 0 | 0 |
| HQVGA | 240x176 | 143 | 28.54 | 35.04 ms | 4,831 B | 0 | 0 |
| 240X240 | 240x240 | 161 | 32.00 | 31.25 ms | 4,831 B | 0 | 0 |
| QVGA | 320x240 | 143 | 28.53 | 35.05 ms | 4,831 B | 0 | 0 |
| 320X320 | 320x320 | 160 | 32.00 | 31.25 ms | 3,575 B | 0 | 0 |
| CIF | 400x296 | 143 | 28.54 | 35.04 ms | 6,103 B | 0 | 0 |
| HVGA | 480x320 | 164 | 32.63 | 30.64 ms | 5,845 B | 0 | 0 |
| VGA | 640x480 | 143 | 28.50 | 35.09 ms | 8,933 B | 0 | 0 |
| SVGA | 800x600 | 143 | 28.44 | 35.16 ms | 12,949 B | 0 | 0 |
| XGA | 1024x768 | 143 | 28.45 | 35.15 ms | 19,980 B | 0 | 0 |
| HD | 1280x720 | 89 | 17.66 | 56.64 ms | 23,419 B | 0 | 0 |
| SXGA | 1280x1024 | 76 | 15.03 | 66.53 ms | 31,783 B | 0 | 0 |
| UXGA | 1600x1200 | 72 | 14.34 | 69.74 ms | 44,901 B | 0 | 0 |
| FHD | 1920x1080 | 88 | 17.59 | 56.86 ms | 48,593 B | 0 | 0 |
| P_HD | 720x1280 | 79 | 15.79 | 63.32 ms | 22,434 B | 0 | 0 |
| P_3MP | 864x1536 | 81 | 16.04 | 62.36 ms | 31,347 B | 0 | 0 |
| QXGA | 2048x1536 | 57 | 11.29 | 88.54 ms | 71,713 B | 0 | 0 |

## Skipped

These frame sizes were skipped because the OV3660 max frame size is QXGA
(`2048x1536`, enum value `19`) in the Espressif camera driver.

| Resolution | Size |
|---|---:|
| QHD | 2560x1440 |
| WQXGA | 2560x1600 |
| P_FHD | 1080x1920 |
| QSXGA | 2560x1920 |
| 5MP | 2592x1944 |

## Notes

- Best measured FPS: HVGA (`480x320`) at `32.63 FPS`.
- Low and mid resolutions mostly stayed around `28-32 FPS`.
- Larger frames dropped after HD-size output.
- The monitor showed two `NO-SOI` messages during QQVGA. The benchmark result still reported `invalid=0` and `failures=0`, so the counted output frames were valid JPEGs.
