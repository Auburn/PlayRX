# Patches to the third-party code

`third_party/kepyur` is a git submodule (kepyur, with its own submodules wifi-sdr, devourer and libusb). Git
records only which upstream commit each one is at; this project's changes to them live here. Apply them to a
fresh checkout (after `git submodule update --init --recursive`):

| Patch | Apply in | What |
|---|---|---|
| `kepyur-0001-comb-luma.patch` … `kepyur-0004-smooth-lines.patch` (in order) | `third_party/kepyur` | video decoder: comb filter, sparkle repair, adaptive comb, smooth lines |
| `kepyur-0005-comb-top-line.patch`, `kepyur-0006-sparkle-repair-clicks.patch` (after those) | `third_party/kepyur` | from [Auburn/kepyur](https://github.com/Auburn/kepyur): the first picture line of each field in colour with the comb (it came out grey), the adaptive comb's colour filter kept out of denormal numbers, the sync levels measured without NaN under `-ffast-math`; sparkle repair: a stricter click test (an echo's notch at one brightness is not a click; a dip cut by the end of a block is judged with the next), chroma patched as far as its filter spreads a click, clicks close together patched as one, and a click patched in the comb's look at the line before too (it showed again one line down) |
| `wifi-sdr-0001-windows.patch` | `third_party/kepyur/third_party/wifi-sdr` | Windows streaming, late banks behind a USB hub, centre hint, firmware table offload (fast bring-up), late banks dropped mid-run instead of a restart, and counted as they are dropped (`RxStatus::mid_drops`, a step in `BankMeta::seq`) |
| `devourer-0001-windows-usb.patch` | `third_party/kepyur/third_party/wifi-sdr/external/devourer` | synchronous EP0 writes, retries, circuit breaker |
| `libusb-0001-winusb-raw-io.patch` | `third_party/kepyur/third_party/libusb` | WinUSB RAW_IO (Windows only) |

    cd third_party/kepyur && git apply ../../patches/kepyur-000*.patch
    cd third_party/wifi-sdr && git apply ../../../../patches/wifi-sdr-0001-windows.patch
    cd external/devourer && git apply ../../../../../../patches/devourer-0001-windows-usb.patch
    cd ../../../libusb && git apply ../../../../patches/libusb-0001-winusb-raw-io.patch

Regenerate one after changing that code: `git -C <submodule> diff --ignore-submodules > patches/<name>.patch`.
