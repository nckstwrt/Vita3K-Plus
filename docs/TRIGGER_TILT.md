# Analog trigger tilt

This opt-in motion input lets an SDL gamepad without motion sensors emulate
left/right Vita tilt. It is not a full gyroscope replacement.

- L2 / LT tilts left; R2 / RT tilts right.
- Partial trigger pressure gives partial tilt.
- Releasing both triggers returns to level. Equal pressure cancels out.
- Existing trigger button mappings are not consumed or changed.
- Detected physical motion sensors take priority.

## Enable

Close Vita3K before editing its `config.yml`. Keep your existing settings and
add or update these keys (do not create duplicate keys):

```yaml
disable-motion: false
trigger-tilt-motion: true
trigger-tilt-max-angle-degrees: 25
trigger-tilt-sensitivity: 1
trigger-tilt-deadzone: 0.05
trigger-tilt-smoothing: 10
trigger-tilt-invert: false
```

Start Vita3K again. No new controller driver or Qt installation is needed when
using a packaged Windows build. The feature is disabled by default.

`trigger-tilt-max-angle-degrees` limits tilt (0–85 degrees); sensitivity scales
trigger pressure (0–4); deadzone ignores small differences (0–0.95).
Smoothing is an exponential response rate in inverse seconds (0–60): larger
values respond faster, and zero disables smoothing. Set invert to true if a
game responds in the opposite direction.

For per-game settings, create the game's custom configuration through the
existing settings dialog, close the emulator, and add the same `trigger-tilt-*`
options as XML attributes on its existing `<system>` element. For example:

```xml
<system trigger-tilt-motion="true" trigger-tilt-max-angle-degrees="25"
        trigger-tilt-sensitivity="1" trigger-tilt-deadzone="0.05"
        trigger-tilt-smoothing="10" trigger-tilt-invert="false" />
```

This is an attribute example, not a complete config: preserve the element's
other attributes and the surrounding file. Existing custom configs without
these attributes inherit the global trigger-tilt settings. The normal
`disable-motion` setting must remain false.

## Scope and verification

The motion signal represents rotation around Vita Y. Acceleration and
orientation agree with the tilt angle, and angular velocity is the rate of
change of that angle. Holding a trigger holds a tilt rather than causing
endless rotation.

The Windows v1.1-based build was manually tested with an EvoFox XInput
controller in Uncharted: Golden Abyss (PCSA00029). The tester confirmed the
L2/R2 balancing input works. This is not a claim of full-game completion,
compatibility with all controllers, or Android/Linux/macOS testing.

The implementation uses the existing configured SDL trigger axes (bindings
4 and 5). If multiple gamepads are connected, the largest left and right
trigger values across them are combined. Disconnect unused gamepads if their
inputs interfere. A detected physical motion device disables the virtual
fallback, even if that physical device is not the one being held.

While motion sampling is active, the log reports trigger values and generated
motion twice per second. Use this to distinguish input detection from a
game-specific response.

## Runnable check

After configuring the project normally:

```powershell
cmake --build build/windows-vs2022 --config Release --target trigger_tilt_test
ctest --test-dir build/windows-vs2022 -C Release -R '^trigger_tilt$' --output-on-failure
```

The check uses an SDL virtual gamepad and exercises real SDL trigger reads,
analog pressure, deadzone, equal triggers, inversion, smoothing/recentering,
zero held angular velocity, normalized orientation/gravity agreement, physical
sensor priority, and disabled motion. It requires no game or firmware.

## Uncharted chapter 13: bright-light parchment

This is a separate camera puzzle, not a trigger-tilt problem. In the general
Vita3K+ settings, set **Camera → Back Camera → Solid Color** to pure white
(`#FFFFFF`), apply, and restart the game. The same tester confirmed this
setting lets the chapter 13 puzzle progress in the v1.1-based Windows build.
The workaround uses existing camera emulation; this contribution does not
modify camera or rendering code.

## Credits

Vita3K and Vita3K+ provide the emulator and existing controller/motion stack.
This contribution adds only the opt-in trigger-driven tilt and its check.
The existing GPL license and copyright notices are retained; see
[`COPYING.txt`](../COPYING.txt).
