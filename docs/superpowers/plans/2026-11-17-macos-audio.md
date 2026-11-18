# macOS Audio Forwarding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Play forwarded Android audio on macOS through the existing CLI/GUI feature.

**Architecture:** A native AudioQueue backend consumes the existing scrcpy RAW stream. The POSIX session remains responsible for ADB, control sockets and disconnect pause. CMake selects the matching native backend.

**Tech Stack:** C11, AudioToolbox, pthreads, CMake, Qt 6, socket-pair/fake-output tests.

**Spec:** `docs/superpowers/specs/2026-11-17-macos-audio-design.md`

## Global Constraints

- Work on approved `feat/20261117`; preserve unrelated untracked files.
- Preserve `audio_player_start(intptr_t)`, `bool audio_player_ready(struct audio_player *)`, and `void audio_player_stop(struct audio_player *)`.
- RAW PCM: 48 kHz, stereo, signed 16-bit little-endian; maximum wire payload 4096 bytes.
- Three 10 ms output buffers, 100 ms bounded ring; producer stall timeout 1 second.
- No first-frame readiness dependency, borrowed-socket close, SDL or FFmpeg.
- Manual audio default stays off; full macOS builds and release CI enable it.
- Update README/user-guide language pairs; strict current-platform build and regression.

## Review Focus

- Paused phone produces no PCM: output must become ready without deadlocking autoplay.
- Device stops consuming: full buffer wait must time out or cancel promptly.
- Disconnect races callback: no callback may access freed state after stop returns.
- Partial or malformed wire input: no partial samples, excessive allocation or blocked stop.
- Audio fails beside UHID/clipboard: existing control and teardown remain functional.

## Task 1: Native playback and lifecycle tests

**Files:** create `platform/macos/audio_player.c`, `tests/test_audio_macos.c`;
modify audio selection and test wiring in `CMakeLists.txt`.

**Interfaces:** consumes the `src/audio_player.h` contract and scrcpy socket positioned
after the dummy byte; produces native AudioQueue playback and thread-safe readiness.

- [x] Add fake AudioQueue implementations using SDK declarations, with failure injection,
  captured PCM and manually driven callbacks. Assert 48000 Hz/2 channels/16 bits,
  exact PCM ordering, trailing silence, readiness without PCM, disabled/wrong codec,
  invalid/oversized/unaligned sizes, partial reads/EOF, setup/enqueue/start failures,
  stop before header, stop while full, timeout while full and dispose-before-free.
- [x] Run the test compile against the missing backend; confirm the expected missing
  implementation failure before writing production code.
- [x] Implement the backend with a worker, bounded ring, callback refill, mutex/condition
  cancellation, atomics for ready, and synchronous queue disposal. Use timed waits
  with a 1-second deadline for a full ring; callbacks never read sockets.
- [x] Select AudioToolbox/Threads for Apple builds; use the selected native source and
  libraries for `test_auxiliary`, rather than unconditionally selecting ALSA.
- [x] Build and run `ctest --test-dir build --output-on-failure -R audio_macos`;
  require all fake-output assertions to pass, then run ASan/UBSan on this target.

## Task 2: Shipping integration and complete regression

**Files:** modify `CMakeLists.txt`, `.github/workflows/build.yml`, `README.md`,
`README.zh-CN.md`, `docs/USER_GUIDE.md`, `docs/USER_GUIDE.zh-CN.md`, `docs/AUDIO.md`,
`AGENTS.md`; adjust other current platform statements only where needed.

**Interfaces:** reuse existing GUI `OTG_GUI_HAVE_AUDIO`, `--audio`, per-device audio
settings, `audio-ready`/`audio-unavailable`, and POSIX session teardown.

- [x] Enable macOS full-profile and CI audio, inspect GUI definitions and packaging
  runtime paths, preserve manual-off and Linux/Windows conditions.
- [x] Synchronize user-facing platform support, build commands, default output and
  failure/permission descriptions across paired docs. Check linked files exist.
- [x] Configure `build/` with `-DOTG_PROFILE=full`, strict C/C++ warning flags; build
  and run the complete CTest suite, including GUI per-device audio, fake ADB pause,
  shared UHID/clipboard, and runtime bundle tests. Resolve failures with evidence.
- [x] Run a local native-output smoke check with synthetic PCM when output is available;
  record hardware limitations separately. Check `otool -L` for AudioToolbox linkage.
- [x] Inspect the complete diff and cross-platform source/library conditions. Review
  the implementation; deliver usage, test evidence and remaining real-device checks.

## Commit policy

Keep implementation changes available for user inspection. Any created commits use
English messages and explicitly selected files; never stage unrelated user files.

## Results and remaining validation

Full macOS build and 27/27 CTests passed; audio-off minimal build and 17/17 CTests
passed. ASan/UBSan and ThreadSanitizer passed for the fake-output tests. Native
AudioQueue readiness/EOF and 1.2 seconds of silent PCM passed. Intel backend syntax
check passed. Installed scrcpy runtime matches its source and the bridge uses
system AudioToolbox. DMG layout unit tests passed (3/3).

Local environment accommodations, not project defaults: remove obsolete CLASSPATH
for tests; explicitly add the SDK libc++ headers; set Qt deprecation warnings to
the CI Qt 6.8 baseline; give Homebrew macdeployqt its separate QtSvg library path.
These avoid changing system configuration but limit the local checks to that
environment; a complete distributed DMG still needs release CI.

Independent review found no blocking defects. Deferred minor test improvements:
wait for invalid-codec parsing before stopping, and add a deterministic barrier
for the in-flight callback/stop interleaving (current coverage is stress-based).

Android real-device sound/autoplay/pause, headphone hot switching, Bluetooth
outputs, sleep/resume and long playback remain unverified. Linux/Windows runtime
and Intel Mac execution were not run locally. Application output selection and
automatic output recreation are outside the approved scope; use system output
settings and reconnect on output failure.
