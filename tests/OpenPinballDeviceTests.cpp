// license:GPLv3+
#include "OpenPinballInput.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace OpenPinballDevice;

static void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

static void CheckDescriptor() {
	// A joystick collection followed by the Pico's OPD collection on the
	// same HID interface. The signature index comes from the Input item.
	unsigned char bytes[] = {
		0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x04,
		0x09, 0x30, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xc0,
		0x05, 0x05, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x05,
		0x79, 71, 0x09, 0x00, 0x15, 0x00, 0x26, 0xff, 0x00,
		0x75, 0x08, 0x95, 28, 0x81, 0x00, 0xc0,
	};
	const auto descriptor = FindDescriptor(bytes, sizeof(bytes));
	Check(descriptor.reportId == 5 && descriptor.stringIndex == 71 && descriptor.payloadSize == 28,
		"Find the OPD report in a composite HID descriptor");
	Check(FindDescriptor(bytes, sizeof(bytes) - 2).stringIndex == 0, "Reject a truncated descriptor");
	bytes[24] = 4;
	Check(FindDescriptor(bytes, sizeof(bytes)).stringIndex == 0, "Reject another input field sharing the OPD report ID");
	bytes[24] = 5;
	bytes[20] = 3;
	Check(FindDescriptor(bytes, sizeof(bytes)).stringIndex == 0, "Reject a different application usage");
	bytes[20] = 2;
	bytes[sizeof(bytes) - 2] = 0x04;
	Check(FindDescriptor(bytes, sizeof(bytes)).stringIndex == 71, "Allow other data-array flags");
	bytes[sizeof(bytes) - 2] = 0x02;
	Check(FindDescriptor(bytes, sizeof(bytes)).stringIndex == 0, "Reject a variable input field");

	const unsigned char unnumbered[] = {
		0x05, 0x05, 0x09, 0x02, 0xa1, 0x01, 0x79, 71, 0x09, 0x00,
		0x75, 0x08, 0x95, 20, 0x81, 0x00, 0xc0,
	};
	const auto shortReport = FindDescriptor(unnumbered, sizeof(unnumbered));
	Check(shortReport.reportId == 0 && shortReport.stringIndex == 71 && shortReport.payloadSize == 20,
		"Accept an unnumbered acceleration-only report");
}

static bool CheckUsbString(const char* signature, bool truncate = false) {
	return VerifySignature(71, [&](uint8_t index, uint16_t language, auto& bytes) {
		if (index == 0 && language == 0) {
			bytes[0] = 4;
			bytes[1] = 3;
			bytes[2] = 9;
			bytes[3] = 4;
			return 4;
		}
		if (index != 71 || language != 0x0409) return -1;
		bytes.fill(0);
		const int size = static_cast<int>(2 + std::strlen(signature) * 2);
		bytes[0] = static_cast<unsigned char>(size);
		bytes[1] = 3;
		for (int i = 2; i < size; i += 2) bytes[i] = signature[(i - 2) / 2];
		return truncate ? size - 2 : size;
	}) == ProbeResult::Found;
}

static void CheckPollingState() {
	Input input;
	std::vector<VpeInputEvent> events;
	input.Start([](const VpeInputEvent* event, void* data) {
		static_cast<std::vector<VpeInputEvent>*>(data)->push_back(*event);
	}, &events);
	const int first = input.Connect("first", "Pinscape Pico", {5, 71, 28}, 0);
	const int second = input.Connect("second", "Other OPD", {0, 71, 20}, 0);
	VpeInputDeviceInfo devices[2]{};
	Check(input.ListDevices(devices, 2) == 2 && std::strcmp(devices[0].displayName, "Pinscape Pico (OPD)") == 0
		&& devices[0].axisCount == 6 && devices[1].axisCount == 2, "Expose separate devices and only complete axes");

	Keys keys{};
	input.BeginPoll(10, true, keys);
	unsigned char report[29]{};
	report[0] = 5;
	report[14] = 2; // Left flipper, bit 9.
	report[18] = 0x80; // Acceleration X = -32768.
	events.clear();
	input.Receive(first, report, sizeof(report) - 1, 10);
	Check(events.empty(), "Ignore a truncated report");
	input.Receive(first, report, sizeof(report), 10);
	Check(events.size() == 7 && events[0].action == VPE_INPUT_LEFT_FLIPPER && events[0].value == 1
		&& events[1].value == -1 && events[1].deviceIndex == first, "Publish a button and all six signed axes");
	VpeInputAxisInfo axes[6]{};
	Check(input.ListAxes(first, axes, 6) == 6 && axes[0].rawValue == -1 && axes[0].timestampUsec == 10
		&& axes[0].kind == VPE_AXIS_KIND_ACCELERATION && axes[2].kind == VPE_AXIS_KIND_VELOCITY,
		"Expose the live axis snapshot");

	keys[VPE_INPUT_LEFT_FLIPPER] = true;
	input.BeginPoll(20, true, keys);
	input.Receive(second, report + 1, 20, 20);
	events.clear();
	report[14] = 0;
	input.Receive(first, report, sizeof(report), 20);
	Check(events.empty(), "A repeated timestamp suppresses motion and other sources preserve a held button");
	keys[VPE_INPUT_LEFT_FLIPPER] = false;
	input.BeginPoll(30, true, keys);
	Check(events.empty(), "An unnumbered controller still holds the shared button");
	input.SetGameplayEnabled(0);
	input.Disconnect(second, 30);
	Check(events.size() == 1 && events[0].eventType == VPE_INPUT_EVENT_DEVICES_CHANGED
		&& input.ListAxes(second, nullptr, 0) == 0, "Hot unplug updates the snapshot during pause");
	input.SetGameplayEnabled(1);
	input.BeginPoll(40, true, keys);
	Check(events.size() == 2 && events[1].eventType == VPE_INPUT_EVENT_ACTION && events[1].value == 0,
		"Release the shared action on resume");
	input.Stop();
	Check(input.ListDevices(nullptr, 0) == 0, "Clear snapshots when polling stops");
}

static void CheckGenericButtons() {
	Input input;
	std::vector<VpeInputEvent> actions;
	input.Start([](const VpeInputEvent* event, void* data) {
		if (event->eventType == VPE_INPUT_EVENT_ACTION) {
			static_cast<std::vector<VpeInputEvent>*>(data)->push_back(*event);
		}
	}, &actions);
	const int first = input.Connect("mapped", "OPD", {5, 71, 28}, 0);
	const int other = input.Connect("other", "OPD", {5, 71, 28}, 0);
	VpeInputButtonInfo buttons[32]{};
	Check(VpeInputListDeviceButtons(first, buttons, 32) == 32 && buttons[31].buttonId == 31
		&& std::strcmp(buttons[31].name, "Generic Button 32") == 0, "Expose one-based OPD button labels");

	const VpeInputButtonBinding binding{VPE_INPUT_TILT, 31, "mapped"};
	VpeInputSetButtonBindings(&binding, 1);
	Keys keys{};
	input.BeginPoll(10, true, keys);
	unsigned char report[29]{};
	report[0] = 5;
	report[12] = 0x80; // Generic button 32, separate from predefined buttons.
	input.Receive(other, report, sizeof(report), 20);
	Check(actions.empty(), "Do not use the same button number on another device");
	input.Receive(first, report, sizeof(report), 20);
	Check(actions.size() == 1 && actions.back().action == VPE_INPUT_TILT && actions.back().value == 1,
		"Route the selected generic OPD button");
	Check(VpeInputListDeviceButtons(first, buttons, 32) == 32 && buttons[31].isPressed,
		"Expose the generic button level");

	keys[VPE_INPUT_TILT] = true;
	input.BeginPoll(30, true, keys);
	report[12] = 0;
	report[14] = 0x80; // Predefined physical tilt replaces the held generic button.
	input.Receive(first, report, sizeof(report), 30);
	keys[VPE_INPUT_TILT] = false;
	input.BeginPoll(40, true, keys);
	Check(actions.size() == 1, "Combine generic, predefined and keyboard tilt without a spurious release");
	report[14] = 0;
	input.Receive(first, report, sizeof(report), 40);
	Check(actions.size() == 2 && actions.back().value == 0, "Release only when all tilt sources release");

	input.SetGameplayEnabled(0);
	report[12] = 0x80;
	input.Receive(first, report, sizeof(report), 50);
	input.SetGameplayEnabled(1);
	input.BeginPoll(60, true, keys);
	Check(actions.size() == 2, "Ignore a generic button pressed during pause");
	report[12] = 0;
	input.Receive(first, report, sizeof(report), 60);
	report[12] = 0x80;
	input.Receive(first, report, sizeof(report), 60);
	Check(actions.size() == 3 && actions.back().value == 1, "Re-arm after the button releases");
	input.Disconnect(first, 70);
	Check(actions.size() == 4 && actions.back().value == 0 && VpeInputListDeviceButtons(first, nullptr, 0) == 0,
		"Release the action and clear button metadata on unplug");
	VpeInputSetButtonBindings(nullptr, 0);
	input.Stop();
}

int main() {
	CheckDescriptor();
	Check(CheckUsbString("OpenPinballDeviceStruct/1.0"), "Read the signature in the advertised USB language");
	Check(!CheckUsbString("OpenPinballDeviceStruct/1.0", true), "Reject a truncated USB string");
	Check(!CheckUsbString("OpenPinballDeviceStruct/2.0"), "Reject an incompatible USB signature");
	CheckPollingState();
	CheckGenericButtons();

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
