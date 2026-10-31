# CYD Music

Touchscreen music player firmware for **ESP32-2432S028R**, the original 2.8-inch resistive-touch Cheap Yellow Display (ST7789 variant, ESP32-WROOM). Landscape 320 × 240 interface with mint accents, artwork, progress, previous/play/pause/next, favorites, playlist pagination and All / Art / Lyrics / Favorites filters. Lyrics have their own paginated view.

## Prepare the microSD

Format a card as FAT32. **MP3 now plays directly: copy your `.mp3` files to the card**, safely eject it, insert it into CYD, and press RST. The card root and subfolders up to three levels deep are scanned (maximum 200 tracks). Uppercase `.MP3` is accepted. No conversion is needed.

The firmware streams MP3 through the bundled [minimp3 decoder](https://github.com/lieff/minimp3), converts mono to stereo, and resamples to 44.1 kHz for Bluetooth. Decoding runs in a separate task feeding a 12 KB PCM queue, so Bluetooth callbacks do not decode or access the card. VBR and CBR files are supported. MP3 progress is approximate; elapsed time is shown without a claimed total duration. Encoder padding may add a short silence between songs; playback is not gapless. Artwork and lyrics still use optional matching `.jpg` and `.txt` sidecars; embedded metadata is not displayed.

The existing WAV preparation tool remains optional for other audio formats. Install Python 3 and FFmpeg, then run:

```sh
python3 tools/prepare_music.py /path/to/music /path/to/sd-card --earbuds "Exact Earbud Name"
```

The tool converts MP3, FLAC, M4A, AAC, OGG, WAV and MP4 audio to **44,100 Hz, stereo, signed 16-bit PCM WAV**. WAV playback still requires that exact PCM format. WAV storage is approximately 10.6 MB per minute; MP3 retains its original smaller file size.

Place artwork and lyrics alongside the source music with the same base name (`Song.mp3`, `Song.jpg`, `Song.txt`). Embedded artwork is extracted when a sidecar image is absent. Artwork becomes a 96 × 96 baseline JPEG. Lyrics are plain text, up to 8192 characters, manually paginated; synchronized LRC and automatic internet lyrics lookup are not implemented. Built-in fonts primarily support ASCII; transliterate lyrics if necessary.

SD layout:

```text
/bluetooth.txt       exact advertised Bluetooth earbud/speaker name
/music/Song.mp3      or Song.wav
/music/Song.jpg      optional, 96 × 96 baseline JPEG
/music/Song.txt      optional plain-text lyrics
/favorites.txt      managed by the player
```

Up to 200 MP3/WAV tracks across the scanned folders, sorted by filename. Use distinct song filenames. Next/previous and automatic advance follow the active filter. The selected song begins playing when a Bluetooth sink connects. Pause before pairing if you prefer to choose a song first.

## Build and flash

Install PlatformIO (VS Code extension or CLI), open this directory, then:

```sh
pio run
pio run -t upload
pio device monitor
```

No firmware is flashed automatically by project creation. PlatformIO pins the Arduino ESP32 platform and library versions. The large application partition has no OTA slot.

## Bluetooth

1. Put your earbuds or speaker into Bluetooth pairing mode; disconnect it from your phone if needed.
2. The default target is `ES-BT09`. To override it, write another exact advertised name into `/bluetooth.txt`, then restart CYD.
3. The A2DP source discovers the matching device, performs Bluetooth pairing and SBC audio negotiation, and remembers the connection for automatic reconnect.
4. A short press and release of BOOT to remove the last Bluetooth bond and remembered address, then restart into fresh discovery. This preserves the player and SD card. The serial command `p` does the same. Use this if a stale pairing prevents connection, or after changing `bluetooth.txt`.

One Bluetooth audio output at a time. The CYD does not bypass pairing requirements. The controller runs in Classic-only mode, releasing unused BLE memory to leave room for pairing and MP3 playback. This targets Bluetooth Classic A2DP; ESP32-S3/C3 variants and BLE-only earbuds are not supported. There is no on-screen discovery list, PIN-entry screen, or local speaker output in this version. Legacy PIN defaults to `0000` (change it in setup if your device requires another PIN). Pairing uses no-input/no-output capabilities. Discovery accepts the exact configured name, ignoring letter case and surrounding spaces, from both EIR and standalone GAP name responses. Serial logs report scanned names, addresses, authentication results and A2DP connection state. The header distinguishes searching, connecting, connected and authentication failure.

## Hardware and touch calibration

Display HSPI: SCLK 14, MOSI 13, MISO 12, CS 15, DC 2; backlight 21. SD VSPI: SCLK 18, MOSI 23, MISO 19, CS 5. Touch is bit-banged on CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 so it does not reconfigure either active SPI host.

On first boot, tap and briefly hold the center of each of four crosses. The first three measure the mapping; the center target checks it before saving. The transform handles swapped axes, inverted directions and calibration boundaries automatically. Mapping is saved in ESP32 preferences and survives firmware uploads. To recalibrate, hold BOOT for at least 1.5 seconds and release it, or send `t` in the serial monitor. A short BOOT press still requests fresh Bluetooth pairing. This configuration uses the ST7789 display driver for the photographed board. Older ILI9341 boards, capacitive touch, or different wiring require configuration changes. Test touch coordinates, SD reading, audio continuity, first pairing, reconnect, and artwork/lyrics on your board before relying on it. File access is serialized between the decoder task and UI; Bluetooth reads the PCM queue. Artwork is cached in RAM before each track starts; oversized artwork is ignored. Use the preparation tool. Favorites writes can briefly interrupt playback.

References: [CYD pin map](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md), [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP), [Espressif A2DP](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_a2dp.html).

## Validation

PlatformIO release build passed with the MP3 decoder. Static RAM is about 64 KB and application flash about 1.22 MB (task stacks, Bluetooth memory and the PCM queue also use dynamic RAM). Native decoding checks passed with generated 44.1 kHz stereo CBR, 48 kHz mono VBR, 22.05 kHz mono and 8 kHz mono MP3 fixtures. Checks covered output pitch, amplitude, channel duplication, decoded length and decoder reset. The native harness is `tests/mp3_decode.cpp`; compile it with `src/mp3_stream.cpp` and `-Iinclude`.

The WAV preparation tool also passed format and artwork conversion checks. Hardware upload can be verified independently of listening tests; MP3 continuity with ES-BT09, touch calibration and card compatibility still need confirmation on the board.

Bundled minimp3 is provided under CC0; see `include/minimp3.LICENSE`.

Touch mapping checks passed for all eight combinations of swapped and inverted axes, including independent mapped points and rejection of degenerate samples. The device still needs the user to tap its four calibration targets.

Hardware connection check: after switching the controller from dual mode to Classic-only, startup free heap increased from about 20 KB to 34 KB and ES-BT09 reported A2DP connected (state 2), without a legacy PIN request. Audible playback quality still requires a listening check.

## Files named MP3 that contain other codecs

An `.mp3` extension does not guarantee MP3 audio. A song examined during hardware diagnosis contained AAC in an MP4 container despite its name. The player connected to ES-BT09, but decoded no audio. A genuine 44.1 kHz stereo MP3 copy is available in `music_ready/`; the full converted song passed the same decoder used by the firmware. Replace the mislabeled file on the SD card with this copy, then restart CYD. The original download is preserved.

The player includes volume controls on the Player screen. Playback diagnostics are printed over serial while connected; `s` reports status and `r` restarts the current track.

Upload recovery: this board experienced repeated temporary-stub transfer failures. `tools/rom_upload.py` now configures normal PlatformIO uploads to use the ESP32 ROM loader (`--no-stub`, uncompressed), which completed and verified a recovery upload at 115200 baud. The command flags were checked with PlatformIO envdump.

The card replacement was subsequently detected as the genuine 4.82 MB MP3, and the decoder filled the PCM queue. Touch calibration was validated and saved on the physical board; observed plus-button requests increased volume from 98 to 106 to 114.

Memory headroom: artwork storage is allocated only for tracks with a JPEG, and PCM buffering is 12 KB. This saves about 30 KB without artwork compared with the first MP3 build. The volume controls now have separate visible button hit areas and send absolute-volume requests to the connected earbuds. MP4-container files mislabeled as MP3 are rejected with an explicit on-screen message.

Physical playback verification after memory reduction: ES-BT09 connected, the real MP3 produced non-zero PCM samples, transmitted byte counts increased past 1.5 MB, and no PCM buffer underruns were recorded. Free heap was approximately 47–51 KB during active streaming. Play/Pause touch events changed the playback state as expected. Audible output still requires the user’s listening confirmation.

Pause/resume uses A2DP SUSPEND and a readiness-check/START sequence, while keeping the decoded PCM queue and file position. Media commands run in the Bluetooth event task and retry failed or missing acknowledgements. Frequent service events are sent only while connected, preserving the library’s connection timeout behavior. Native transition checks cover repeated pause/resume, rapid resume before suspend acknowledgement, delayed and failed commands, and disconnects. Serial `0` pauses and `1` resumes without reopening the file.

Pause/resume hardware validation passed on ES-BT09: three pause/resume sequences preserved the consumed PCM position while paused and resumed increasing transmitted audio bytes. A rapid pause/resume toggle also restarted streaming successfully. The player was left playing, with no PCM underruns during the test. Results and device logs are recorded in `tests/hardware_pause_resume.json`.
# CYD_Mp3_palyer

Screen redraws: dynamic widgets use one reusable 288×30, 4-bit sprite (about 4.3 KB). Timeline updates are sent only when the elapsed second or progress pixel changes; Play/Pause, favorites, volume and Bluetooth status update independently. Normal playback no longer clears the bottom 67 pixels twice per second. Volume, playback and Bluetooth changes do not clear the whole screen. Full repainting is limited to page/content changes and leaving calibration. Diagnostics include `uiClears` and `uiPushes` to check redraw behavior on the board.
