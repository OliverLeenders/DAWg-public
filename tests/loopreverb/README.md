# Loop Reverb: the clean-room tests

The clean side's own tests for the reverb in `engine/src/loopreverb/`, copied from the clean-room workspace
(`reverb-cleanroom`, commit `c0c6ac1`, SPEC 1.7 draft): the acceptance tests of its specification (A1–A25 except the
listening tests A16, A17, A20 and A24), the behaviour tests, the interface tests (A12), their measurements and their small test runner. They
test the reverb itself, through the workspace's copy of the node API (`engine/src/loopreverb/dawg_sdk/`); DAWg's
adapter is tested in `tests/LoopReverbTests.cpp`.

`shim/` (DAWg-side) makes the copied node API visible as `dawg::` for these tests. Apart from DAWg's clang-format
(whitespace, include order), only `node/Nodes.cpp` differs: it leaves out the workspace's example node; and A21 in
`node/AcceptanceTests.cpp` allows 80 dB instead of 100 (GitHub's Windows runners land at 88). Don't edit the
rest here; changes go through the specification and the clean side.

Run them with ctest in a release configuration, or directly:

    build/tests/loopreverb/Release/dawg_loop_reverb_tests.exe [part of a test name]

The tests take three to four minutes in Release. A11 limits the CPU time (SPEC 1.7: 0.9 s per 60 s at the defaults,
1.5 s at any setting, best of three on an idle machine), so a Debug build fails it, and so can a busy machine. A21
compares with samples the clean side recorded from a g++ build; MSVC's output differs from them by about −88 dB. The
DAWg side checked A21 against the accepted build's own render tool, built with MSVC: bit-identical (the clean room's
`records/MONITOR.md`, 2026-10-28).
