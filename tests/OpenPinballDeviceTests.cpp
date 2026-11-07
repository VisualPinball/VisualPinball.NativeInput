// license:GPLv3+
#include "OpenPinballDevice.h"

#include <cstdio>
#include <cstdlib>

using namespace OpenPinballDevice;

static void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

int main() {
	Check(SupportsVersion(L"OpenPinballDeviceStruct/1.0"), "Recognize the Pico signature");
	Check(SupportsVersion(L"OpenPinballDeviceStruct/1.12"), "Accept a future minor version");
	Check(!SupportsVersion(L"OpenPinballDeviceStruct/2.0"), "Reject a different major version");
	Check(!SupportsVersion(L"OpenPinballDeviceStruct/1."), "Reject a missing minor version");
	Check(!SupportsVersion(L"OpenPinballDeviceStruct/1.0junk"), "Reject malformed versions");

	const unsigned char bytes[] = {
		0x06,                                      // report ID
		0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01, // timestamp
		0xff, 0xff, 0xff, 0xff,                     // generic buttons
		0, 0x80, 0x40, 0,                          // physical tilt and left nudge
		0x00, 0x80, 0xff, 0x7f,                     // acceleration (-32768, 32767)
		0x00, 0x40, 0x00, 0xc0,                     // velocity (+16384, -16384)
		0x00, 0x20, 0x00, 0xe0,                     // plunger (+8192, -8192)
	};
	Report report{};
	Check(!Report::Read(bytes, sizeof(bytes), 7, report), "Reject a different report ID");
	Check(!Report::Read(bytes, 16, 6, report), "Reject a truncated header");
	Check(Report::Read(bytes, sizeof(bytes), 6, report), "Decode a Pico report");
	Check(report.Timestamp() == UINT64_C(0x0123456789abcdef), "Read little-endian device timestamp");
	Check(report.PinballButtons() == ((1u << 15) | (1u << 22)), "Read only the predefined button mask");
	Check(ButtonActions[15] == VPE_INPUT_TILT && ButtonActions[22] == VPE_INPUT_LEFT_NUDGE, "Route tilt and nudge bits");
	const float expected[] = {-1.f, 32767.f / 32768.f, .5f, -.5f, .25f, -.25f};
	for (const auto& axis : Axes) {
		float value;
		Check(report.TryReadAxis(axis, value) && value == expected[axis.id], "Decode signed, named OPD axes");
	}
	Check(Report::Read(bytes, 21, 6, report), "Read an acceleration-only report");
	float value;
	Check(report.TryReadAxis(Axes[1], value), "Keep complete acceleration fields");
	Check(!report.TryReadAxis(Axes[2], value), "An absent velocity field is distinct from zero");
	Check(Report::Read(bytes, 22, 6, report) && !report.TryReadAxis(Axes[2], value), "Reject a partial axis field");

	Clock clock;
	int64_t sample;
	Check(clock.Translate(500000, 2000, sample) && sample == 2000, "Anchor device time in the host domain");
	Check(!clock.Translate(500000, 2500, sample), "Ignore repeated timestamps for motion samples");
	Check(clock.Translate(500750, 3000, sample) && sample == 2750, "Preserve a fresh report's interval");
	Check(clock.Translate(501250, 3100, sample) && sample == 3100, "Clamp timestamps to receipt time");
	Check(clock.Translate(100, 4000, sample) && sample == 4000, "Reanchor after a controller reset");
	Check(clock.Translate(200, 30000, sample) && sample == 30000, "Reanchor stale timestamps after a stall");

	ActionLatch latch;
	bool pressed;
	Check(latch.Update(true, true, pressed) && pressed, "Publish a button press");
	Check(!latch.Update(false, false, pressed), "Suppress a release during pause");
	Check(latch.Update(false, true, pressed) && !pressed, "Reconcile that release on resume");
	Check(!latch.Update(true, false, pressed), "Suppress a press during pause");
	Check(!latch.Update(true, true, pressed), "Don't replay a menu press still held on resume");
	Check(!latch.Update(false, true, pressed), "Re-arm after the ignored press is released");
	Check(latch.Update(true, true, pressed) && pressed, "Publish the next gameplay press");
	Check(!latch.Update(false, false, pressed), "Keep a published hold during pause");
	Check(!latch.Update(true, false, pressed), "Suppress another press after releasing that hold");
	Check(latch.Update(true, true, pressed) && !pressed, "Release the old hold without replaying the new press");
}
