# macOS audio forwarding

## Intent and scope

Complete the existing Android-to-computer audio feature on macOS, for both CLI
and GUI. The user approved the native AudioQueue design on 2026-11-17 and
development on `feat/20261117`.

## Approaches

- **Selected: AudioToolbox AudioQueue**, using its C API. This fits the existing
  platform-specific ALSA/WASAPI backends and introduces no third-party dependency.
- SDL would provide portable output but add a dependency to the pure-C CLI and
  duplicate an abstraction already provided by `audio_player`.

## Design

- Add `platform/macos/audio_player.c`, implementing the existing
  `audio_player_start(intptr_t)`, `audio_player_ready()` and `audio_player_stop()`
  contract. Continue using the POSIX scrcpy session for ADB/server/socket ownership.
- Input is scrcpy's RAW codec, 12-byte frame headers and 48 kHz, stereo, signed
  16-bit little-endian PCM. Validate bounded, nonempty, whole-sample frames.
- Open the default macOS output through AudioQueue. Report ready after the RAW
  codec and output initialization, without requiring a first PCM frame.
- Builder choice: three 10 ms output buffers and a bounded 100 ms PCM ring.
  Callbacks copy buffered PCM, fill underruns with silence, and never perform
  socket I/O. A worker parses the stream and applies cancellable backpressure;
  a stalled output must fail within a bounded interval rather than hang exit.
- Stop wakes socket reads and buffer waits, joins the worker, and synchronously
  disposes the queue before releasing callback state. The borrowed socket is not
  closed by the player. EOF/output errors clear readiness. Playback failure is
  nonfatal to HID/control, matching the current implementation.
- Reuse per-device GUI audio settings, CLI `--audio`, audio-ready notifications,
  companion autoplay gating, and the existing bounded media pause on disconnect.
- Enable macOS audio in `OTG_PROFILE=full` and both macOS CI/package architectures.
  Manual builds keep the existing default `BUILD_AUDIO=OFF`.
- Update paired READMEs, paired user guides, AUDIO documentation and relevant
  platform limitation statements. Existing screenshots show Linux's identical
  audio checkbox and do not need replacement for this platform enablement.

## Constraints and verification

Core stays C11; GUI stays Qt/C++17. No SDL, FFmpeg, microphone permission, new
Android component, output selector, or alternate audio protocol is needed.
Linux/Windows retain their backend and lifecycle. Android 11+ and authorized ADB
remain prerequisites; protected/noncapturable audio remains unsupported.

Offline tests use socket pairs and a fake AudioQueue to verify exact PCM delivery,
silence on underrun, readiness before PCM, malformed input, failed output setup,
bounded buffering, cancellation, and callback cleanup. Existing fake ADB and GUI
tests cover shared sessions, disconnect pause, UHID/clipboard and per-device
settings. Run a strict full macOS build and complete CTest suite, inspect other
platform branches, and perform an optional real local-output smoke test. Host
simulation/builds do not establish Android hardware or all-output-device support.
