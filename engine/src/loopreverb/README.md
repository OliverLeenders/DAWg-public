# Loop Reverb: the clean-room implementation

The files here, except `dawg_sdk/`, are the reverb as written by an independent implementer from a functional
specification, without access to DAWg or any other reverb's code. They are copied from the clean-room workspace
(`reverb-cleanroom`, commit `c0c6ac1`, written against the SPEC.md 1.7 draft; the last revision accepted in its
`records/MONITOR.md` is `1e26190`, against 1.6), changed by DAWg's clang-format (whitespace) and by three small
DAWg-side changes: `dsp::same` for exact float compares (Clang's `-Wfloat-equal`), and `ReverbNode::clear()` for
DAWg's output protection, which calls the node's own `clearState()`. Don't edit them otherwise; changes to the
reverb's behaviour go through the specification and the clean side.

* `SPEC.md`: the functional specification (1.7 draft) the reverb was written from. `DESIGN.md`: the implementer's design,
  with its measurements and references. Both unchanged. DAWg's summary is `docs/design/loop-reverb.md`.
* `Reverb.h`, `Reverb.cpp`: the node (`dawg::reverb::ReverbNode`).
* `Pattern.h`, `Pattern.cpp`: the seeded patterns. `Dsp.h`: delay lines, allpasses and filters. `Simd.h`: four
  lines at a time (SSE2 or NEON).
* `dawg_sdk/`: the workspace's copy of DAWg's node API, placed in `dawg::reverb` so the files above compile as
  they are. Not part of the clean-room delivery's design; written by the DAWg side.

`engine/src/LoopReverb.cpp` adapts the node to DAWg's `Node` API and registers it as `dawg.loopReverb`.
