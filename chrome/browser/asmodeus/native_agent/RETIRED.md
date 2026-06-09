# RETIRED — superseded by Cicero

This v4 native voice agent is **retired** (2026-06-04). It has been copied-and-
owned + ported into the standalone, de-ambiented **Cicero** library at
`/Users/admin/workspace/Legion/cicero/` (+ the embodiment at
`/Users/admin/workspace/Legion/as-embodiments/cicero/`), which is now the single
live voice agent — no parallel systems (CLAUDE.md).

- The DSP/ML core (AEC, resampler, whisper STT, Silero VAD, Piper TTS, the audio
  ring) was copied verbatim into Cicero; the FSM became the de-ambiented
  `ConversationEngine`; every part is exposed as an AgentSpaces Handle.
- Cicero is proven end-to-end: a LIVE two-agent spoken conversation over real
  audio hardware (see `as-embodiments/cicero/docs/LIVE-VOICE-KEYSTONE.md`).

`BUILD.gn` has been renamed to `BUILD.gn.retired` so this target no longer
builds. The source is kept for provenance only; do not extend it — change Cicero.
