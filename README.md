# VPE Native Input Polling

High-performance native input polling library for Visual Pinball Engine.

## Purpose

Provides OS-level input polling to achieve sub-millisecond input latency by bypassing Unity's Input System buffer. The input polling runs on a dedicated thread at 500-2000 Hz and forwards events to the simulation thread via a callback.

## Architecture

```
┌─────────────────────────────────────┐
│   OS Input Events (Keyboard/HID)    │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│  Native Input Polling Thread        │
│  (500-2000 Hz)                      │
├─────────────────────────────────────┤
│  • GetAsyncKeyState (Windows)       │
│  • X11 key polling (Linux)          │
│  • CoreGraphics/AppKit (macOS)      │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│  Lock-Free SPSC Ring Buffer         │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│  Simulation Thread (1000 Hz)        │
└─────────────────────────────────────┘
```

## Building

### Windows (Visual Studio 2022)

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Output: `../VisualPinball.Engine/VisualPinball.Unity/VisualPinball.Unity/Plugins/win-x64/VpeNativeInput.dll`

### Windows (MinGW)

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Linux

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Requires: `libx11-dev`

### macOS

```bash
cd VisualPinball.NativeInput
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Requires: AppKit and ApplicationServices frameworks (provided by macOS)

### CI / NuGet

GitHub Actions builds `win-x64`, `win-x86`, `linux-x64`, `osx-x64`, and `osx-arm64` binaries and packs them into a single NuGet package.

Publishing happens automatically for tags matching `v*`.

## Usage from C#

```csharp
using VisualPinball.Unity.Simulation;

// Initialize
NativeInputApi.VpeInputInit();

// Set bindings
var bindings = new[] {
    new NativeInputApi.InputBinding {
        Action = (int)NativeInputApi.InputAction.LeftFlipper,
        BindingType = (int)NativeInputApi.BindingType.Keyboard,
        KeyCode = (int)NativeInputApi.KeyCode.LShift
    }
};
NativeInputApi.VpeInputSetBindings(bindings, bindings.Length);

// Start polling
NativeInputApi.VpeInputStartPolling(OnInputEvent, IntPtr.Zero, 500);

// Callback
[MonoPInvokeCallback(typeof(NativeInputApi.InputEventCallback))]
static void OnInputEvent(ref NativeInputApi.InputEvent evt, IntPtr userData) {
    Console.WriteLine($"Input: {evt.Action} = {evt.Value} @ {evt.TimestampUsec}μs");
}

// Stop polling
NativeInputApi.VpeInputStopPolling();

// Shutdown
NativeInputApi.VpeInputShutdown();
```

## Platform Status

| Platform | Status | API Used |
|----------|--------|----------|
| Windows  | ✅ Implemented | GetAsyncKeyState, QueryPerformanceCounter |
| Linux    | ✅ Implemented | X11 keyboard state polling |
| macOS    | ✅ Implemented | CoreGraphics keyboard state polling |

## Performance

- **Latency**: <0.1ms from physical input to event callback
- **CPU Usage**: 5-10% of one core (mostly sleeping)
- **Overhead**: ~10-20ns per P/Invoke call
- **Precision**: Monotonic microsecond timestamps across platforms

## Future Enhancements

1. **Gamepad support**: XInput (Windows), SDL/libinput (Linux), GCController (macOS)
2. **Analog inputs**: Plunger, steering, analog triggers
3. **Force feedback**: Haptics for nudge, tilt, button feedback
4. **Hot-plugging**: Detect device connect/disconnect
5. **Configuration**: JSON-based key mapping files
6. **Profiles**: Per-table input profiles

## License

GPLv3+ (same as VPE)
