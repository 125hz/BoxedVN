# Input, audio and floating-point integration

Device logs `boxedvn-20260904-234948.log`, `235412.log`, `235702.log`, and
`235915.log` identify revision `993b6d91+dirty`. The native interface now responds
while the 64-bit application renders. Keyboard input, audio, and the two 32-bit
startup paths still fail. These findings do not establish device acceptance of
the changes below.

## Floating-point ABI collision

The cube's pre-graphics `BOXEDWINE_PE32_X87_PROBE` reports zero for conversion of
11 to double and `pass=0`. The graphics worker subsequently computes enormous
hash-table allocations; the other 32-bit process recursively enters `frexp`.

BoxedWine's `lib/softfloat` and FEX's `External/SoftFloat-3e` export the same C
names. FEX adds a `softfloat_state*` argument to operations including
`extF80_to_f64` and `extF80_mul`; BoxedWine's implementations lack that argument.
Linking both archives lets one ABI satisfy the other's references. The standalone
FEX instruction tests cannot expose this integration failure.

The maintained build now uses Clang's `redefine_extname` to prefix FEX's external
SoftFloat definitions and references. This leaves vendor source unchanged and
preserves internal inline primitives. Preprocessor aliases are unsuitable:
SoftFloat tests whether primitive names are macros to select implementations.
An archive audit rejects remaining unprefixed references/definitions. The same
namespace is applied to the VIXL build.

A native co-link regression exercises both original and state-parameter ABIs,
with C and C++ callers, integer conversion and subnormal multiplication. On the
Windows host, the original linkage reports `BoxedWine=1 FEX=0`; separated linkage
passes both libraries and the C++ caller. CI also runs it on native macOS ARM64.
The device cube probe remains the acceptance test for the real FEX call path.

## Input and frame rate

X11 initial keyboard focus was None, dropping all key events until a client
explicitly set focus. Initialize PointerRoot, matching
[Xorg's InitFocusClassDeviceStruct](https://raw.githubusercontent.com/mirror/xserver/master/dix/devices.c).
Route PointerRoot through the existing fullscreen coordinate mapping. Bounded
`BOXEDWINE_X11_KEY` and `BOXEDWINE_X64_X11_KEY` witnesses distinguish injection
from delivery to Wine. Toolbar taps hold for 100 ms so polling input can observe
the press. The touch keyboard and joystick hold keys until release/cancellation.

Reject SDL's synthetic touch-mouse events on iOS because the UIKit overlay already
injects touch in guest coordinates. Captures also showed negative SDL positions
while operating frontend controls. Hardware mouse events remain supported.

Per-frame DXMT call/return logging is replaced with five-second
`BOXEDWINE_DXMT_CADENCE` reports for present and nextDrawable. They separate time
inside native calls (including the frame limiter) from gaps between calls. DXMT
trace now follows the verbose toggle. These reduce diagnostic overhead and make
remaining frame-rate drops measurable; they do not establish a target FPS.

## Audio

The rendering application fails `dsound:get_mmdevenum` with `80040154` before
opening an audio endpoint. OSS's PE and Unix driver halves are already packaged.
Repair only missing MMDeviceEnumerator registrations in the selected prefix's
system.reg, and only for packaged DLL architectures. Use Wine 9's
[class UUID and Both threading model](https://raw.githubusercontent.com/wine-mirror/wine/wine-9.0/dlls/mmdevapi/mmdevapi_classes.idl).
Preserve existing registrations and unrelated registry data; write atomically
before launching Wine. `BOXEDWINE_X64_AUDIO_COM` reports the result.

## Frontend and validation boundary

Add a held WASD joystick with diagonal movement and cancellation release. Reduce
keyboard crowding and put arrow keys in a dedicated aligned row. Remove the live
view heading and 32-bit desktop action, move Run program first, and expose width
and height fields only under the Custom resolution selection.

Wine still executes as a Linux guest inside BoxedWine: FEX forwards its syscalls
to BoxedWine's kernel; X11, processes, files and OSS remain emulated services.
DXMT and Vulkan host graphics bridges do not replace that architecture.

Device retest: verbose off first; test Enter/menu navigation, key release, mouse
movement, sound, two minutes of frame cadence, the held joystick, then both
32-bit startup paths. A successful IPA build is not proof of those outcomes.
