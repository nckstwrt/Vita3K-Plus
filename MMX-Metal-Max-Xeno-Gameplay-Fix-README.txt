Metal Max Xeno (PCSG00972) - proven gameplay targeting fix integration

This source is based on the user-supplied Vita3K-Plus all-enhancements tree.
It carries over only the gameplay behavior proven in the v1.57 test build:

- v76/v77 validator semantic corrections retained for the first lock-on
  validator pass after arming.
- Positive decisive geometry at 0x810EDFA0 conditionally bypasses the special
  type-6 BEQ at 0x810ED458 so clear-line-of-sight enemies reach the normal
  classifier/shot path.
- Negative/zero geometry keeps the native type-6 rejection, preserving
  genuinely obstructed targets.
- Normal post-shot turn-based combat remains game-controlled.

Not included:
- v1.60 target +0x70/+0x74 UI experiment.
- v1.61 target +0xA8 UI experiment.
- Any Blocked/Fire text/UI override.
- The large historical MMX tracing/debug instrumentation.

Usage
-----
For Metal Max Xeno only, wait until SHOOTING appears, then press SELECT+START
once before the first Circle lock-on. The combo is not consumed by Vita3K.

Expected log markers include:
[MMX-V1.57-ARMED]
[MMX-FIX-VALIDATOR-RESOLVE]
[MMX-V1.57-GEOMETRY-GATE]
[MMX-V1.57-CONDITIONAL-TYPE6-BYPASS-TRANSLATE]

The floating Blocked/Fire label is intentionally left unchanged for now.
