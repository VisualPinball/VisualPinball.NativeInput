// license:GPLv3+
#include "ControllerInput.h"
#include "NativeButtons.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

static void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "%s (SDL: %s)\n", message, SDL_GetError());
		std::exit(1);
	}
}

struct Observed {
	ControllerInput& input;
	std::vector<VpeInputEvent> events;
	std::vector<VpeInputDeviceInfo> devices;

	void Snapshot() {
		const int count = input.AppendDevices(nullptr, 0, 1);
		devices.resize(count);
		devices[0].deviceIndex = 77;
		Check(input.AppendDevices(devices.data(), count, 1) == count, "Append controller snapshot");
		Check(devices[0].deviceIndex == 77, "Keep the preexisting device snapshot");
	}

	VpeInputDeviceInfo Find(const char* name) const {
		for (const auto& device : devices) {
			if (std::strcmp(device.displayName, name) == 0) return device;
		}
		return {};
	}

	bool SawAxis(int device, int axis, float value) const {
		return std::any_of(events.begin(), events.end(), [&](const VpeInputEvent& event) {
			return event.eventType == VPE_INPUT_EVENT_AXIS && event.deviceIndex == device
				&& event.axisId == axis && event.value == value;
		});
	}
};

static SDL_JoystickID Attach(const char* name, SDL_JoystickType type, Uint16 axes, Uint16 hats = 0) {
	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.name = name;
	desc.type = type;
	desc.naxes = axes;
	desc.nhats = hats;
	desc.nbuttons = 1;
	const auto id = SDL_AttachVirtualJoystick(&desc);
	Check(id != 0, "Attach a virtual controller");
	return id;
}

int main() {
	const int newDeviceIndex = 77;
	std::array<VpeInputButtonInfo, 2> newButtons{{{0, 0, "Button 1"}, {1, 0, "Button 2"}}};
	NativeButtons::GetState().Connect(newDeviceIndex, "new-board", {newButtons[0], newButtons[1]}, {0});
	Check(VpeInputListDeviceButtons(newDeviceIndex, newButtons.data(), 2) == 2, "List button metadata before the first report");
	VpeInputStartControlCapture(1);
	NativeButtons::GetState().Update(newDeviceIndex, [](const auto&) -> std::optional<bool> { return std::nullopt; },
		[](int, float) { return .25f; });
	Check(VpeInputReadDeviceButtons(newDeviceIndex, newButtons.data(), 2) == 0, "An axis-only report does not sample buttons");
	const auto sampleButton = [&](int id, bool pressed) {
		NativeButtons::GetState().Update(newDeviceIndex, [&](const auto& button) -> std::optional<bool> {
			if (button.buttonId != id) return std::nullopt;
			return pressed;
		});
	};
	sampleButton(0, true);
	Check(VpeInputReadDeviceButtons(newDeviceIndex, newButtons.data(), 2) == 0, "Wait for all button reports");
	sampleButton(1, true);
	Check(VpeInputReadDeviceButtons(newDeviceIndex, newButtons.data(), 2) == 2
		&& newButtons[0].isPressed && newButtons[1].isPressed, "Combine button levels from separate reports");
	sampleButton(1, false);
	Check(VpeInputReadDeviceButtons(newDeviceIndex, newButtons.data(), 2) == 2
		&& newButtons[0].isPressed && !newButtons[1].isPressed, "Keep the last level of buttons outside the current report");
	VpeInputControlBinding capturedButton{};
	Check(VpeInputReadControlCapture(&capturedButton, 1) == 0, "A held button is not captured on its first sample");
	sampleButton(1, true);
	sampleButton(1, false);
	Check(VpeInputReadControlCapture(&capturedButton, 1) == 1 && capturedButton.controlId == 1
		&& capturedButton.kind == VPE_CONTROL_BUTTON, "Capture a new press after that button has been sampled");
	VpeInputStartControlCapture(0);
	NativeButtons::GetState().Disconnect(newDeviceIndex);

	ControllerInput input;
	input.Start();
	Observed observed{input};
	const auto callback = [](const VpeInputEvent* event, void* data) {
		auto& state = *static_cast<Observed*>(data);
		state.events.push_back(*event);
		if (event->eventType == VPE_INPUT_EVENT_DEVICES_CHANGED) state.Snapshot();
	};

	const auto rawId = Attach("PinscapePico", SDL_JOYSTICK_TYPE_UNKNOWN, 6, 1);
	const auto padId = Attach("VPE Test Pad", SDL_JOYSTICK_TYPE_GAMEPAD, SDL_GAMEPAD_AXIS_COUNT);
	SDL_Joystick* raw = SDL_OpenJoystick(rawId);
	SDL_Joystick* pad = SDL_OpenJoystick(padId);
	Check(raw != nullptr && pad != nullptr, "Open the virtual devices for sample injection");
	input.Poll(1000, true, callback, &observed);
	const auto pinscape = observed.Find("PinscapePico");
	const auto gamepad = observed.Find("VPE Test Pad (Gamepad)");
	Check(pinscape.deviceIndex >= ControllerInput::FirstDeviceIndex && pinscape.axisCount == 6,
		"Publish all raw joystick axes after discovery");
	Check(gamepad.deviceIndex >= ControllerInput::FirstDeviceIndex && gamepad.axisCount == 6,
		"Publish standardized gamepad axes");
	Check(observed.events.front().eventType == VPE_INPUT_EVENT_DEVICES_CHANGED, "Publish identity before axis events");
	std::array<VpeInputAxisInfo, 6> axes{};
	input.ListAxes(pinscape.deviceIndex, axes.data(), static_cast<int>(axes.size()));
	Check(axes[0].usagePage == 1 && axes[3].usage == 0x33, "Expose Pinscape X/RX usages for existing presets");
	input.ListAxes(gamepad.deviceIndex, axes.data(), static_cast<int>(axes.size()));
	Check(axes[0].usagePage == VPE_GAMEPAD_AXIS_USAGE_PAGE && std::strcmp(axes[0].name, "Left Stick X") == 0,
		"Expose standardized axis metadata");
	std::array<VpeInputButtonInfo, 5> rawButtons{};
	VpeInputButtonInfo padButton{};
	Check(VpeInputListDeviceButtons(pinscape.deviceIndex, rawButtons.data(), 5) == 5 && rawButtons[0].buttonId == 0
		&& std::strcmp(rawButtons[1].name, "Hat 1 Up") == 0 && std::strcmp(rawButtons[2].name, "Hat 1 Right") == 0,
		"Expose raw joystick buttons and POV directions");
	const auto rawButton = rawButtons[0];
	Check(VpeInputListDeviceButtons(gamepad.deviceIndex, &padButton, 1) >= 1
		&& padButton.buttonId == SDL_GAMEPAD_BUTTON_SOUTH, "Expose standardized gamepad buttons");
	constexpr int leftTrigger = 0x30000;
	const auto triggerPressed = [&]() {
		std::array<VpeInputButtonInfo, 16> buttons{};
		const int count = VpeInputReadDeviceButtons(gamepad.deviceIndex, buttons.data(), static_cast<int>(buttons.size()));
		const auto end = buttons.begin() + std::min(count, static_cast<int>(buttons.size()));
		const auto match = std::find_if(buttons.begin(), end, [leftTrigger](const auto& button) { return button.buttonId == leftTrigger; });
		Check(match != end, "Advertise the gamepad left trigger as a bindable button");
		return match->isPressed != 0;
	};
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN), "Release the trigger");
	input.Poll(1010, true, callback, &observed);
	Check(!triggerPressed(), "The released trigger is up");
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX), "Press the trigger");
	input.Poll(1020, true, callback, &observed);
	Check(triggerPressed(), "The pressed trigger is down");
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN), "Release the trigger again");
	input.Poll(1030, true, callback, &observed);
	Check(!triggerPressed(), "The trigger returns to up");

	VpeInputStartControlCapture(2);
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX), "Pull one trigger for capture");
	input.Poll(1040, true, callback, &observed);
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN), "Release the single trigger");
	input.Poll(1050, true, callback, &observed);
	VpeInputControlBinding triggerCombo[8]{};
	Check(VpeInputReadControlCapture(triggerCombo, 8) == 0, "A trigger alone does not count as a combo");
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX)
		&& SDL_SetJoystickVirtualButton(pad, 0, true), "Pull a trigger and press a button for a combo");
	input.Poll(1060, true, callback, &observed);
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN)
		&& SDL_SetJoystickVirtualButton(pad, 0, false), "Release the trigger combo");
	input.Poll(1070, true, callback, &observed);
	Check(VpeInputReadControlCapture(triggerCombo, 8) == 2
		&& triggerCombo[0].kind == VPE_CONTROL_BUTTON && triggerCombo[0].controlId == padButton.buttonId
		&& triggerCombo[1].kind == VPE_CONTROL_BUTTON && triggerCombo[1].controlId == leftTrigger,
		"Capture the trigger and button without a duplicate axis");
	triggerCombo[0].action = triggerCombo[1].action = VPE_INPUT_START;
	VpeInputSetControlBindings(triggerCombo, 2);
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX), "Pull the mapped trigger");
	input.Poll(1080, true, callback, &observed);
	Check(NativeButtons::GetState().PressedActions() == 0, "A mapped trigger combo requires both controls");
	Check(SDL_SetJoystickVirtualButton(pad, 0, true), "Complete the mapped trigger combo");
	input.Poll(1090, true, callback, &observed);
	Check(NativeButtons::GetState().PressedActions() == (uint64_t{1} << VPE_INPUT_START),
		"Use the captured trigger combo for a gameplay action");
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN)
		&& SDL_SetJoystickVirtualButton(pad, 0, false), "Restore the virtual gamepad to rest");
	input.Poll(1095, true, callback, &observed);

	VpeInputButtonBinding bindings[] = {{VPE_INPUT_LEFT_NUDGE, rawButton.buttonId}, {VPE_INPUT_LEFT_NUDGE, padButton.buttonId}};
	std::snprintf(bindings[0].deviceId, sizeof(bindings[0].deviceId), "%s", pinscape.stableId);
	std::snprintf(bindings[1].deviceId, sizeof(bindings[1].deviceId), "%s", gamepad.stableId);
	VpeInputSetButtonBindings(bindings, 2);
	Check(SDL_SetJoystickVirtualButton(raw, 0, true) && SDL_SetJoystickVirtualButton(pad, 0, true),
		"Inject joystick and gamepad presses");
	input.Poll(1100, true, callback, &observed);
	const uint64_t nudge = uint64_t{1} << VPE_INPUT_LEFT_NUDGE;
	Check(NativeButtons::GetState().PressedActions() == nudge, "Combine configured joystick and gamepad presses");
	Check(SDL_SetJoystickVirtualButton(pad, 0, false), "Release the gamepad button");
	input.Poll(1200, true, callback, &observed);
	Check(NativeButtons::GetState().PressedActions() == nudge, "A joystick can keep the action held");

	observed.events.clear();
	Check(SDL_SetJoystickVirtualAxis(raw, 0, -32768), "Inject a raw sample");
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 16384), "Inject a left-stick sample");
	input.Poll(2000, true, callback, &observed);
	Check(observed.SawAxis(pinscape.deviceIndex, 0, -1.f), "Deliver a signed raw sample");
	Check(observed.SawAxis(gamepad.deviceIndex, 0, .5f), "Deliver a signed standardized sample");

	observed.events.clear();
	input.SetGameplayEnabled(false);
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, -16384), "Inject a sample during pause");
	input.Poll(3000, true, callback, &observed);
	Check(observed.events.empty(), "Suppress axis events during pause");
	input.ListAxes(gamepad.deviceIndex, axes.data(), static_cast<int>(axes.size()));
	Check(axes[0].rawValue == -.5f && axes[0].timestampUsec == 3000, "Keep the calibration snapshot live during pause");
	input.SetGameplayEnabled(true);
	input.Poll(4000, true, callback, &observed);
	Check(observed.SawAxis(gamepad.deviceIndex, 0, -.5f), "Refresh held axes on resume");

	const auto makeBinding = [](int action, int group, int kind, int id, float threshold, const char* device) {
		VpeInputControlBinding binding{action, group, kind, id, threshold, {}};
		std::snprintf(binding.deviceId, sizeof(binding.deviceId), "%s", device);
		return binding;
	};
	const auto hatUp = makeBinding(VPE_INPUT_LEFT_NUDGE, 0, VPE_CONTROL_BUTTON, rawButtons[1].buttonId, 0, pinscape.stableId);
	const VpeInputControlBinding controls[] = {
		hatUp,
		makeBinding(VPE_INPUT_LEFT_NUDGE, 1, VPE_CONTROL_AXIS_BELOW, 0, -.75f, gamepad.stableId),
		makeBinding(VPE_INPUT_SLAM_TILT, 0, VPE_CONTROL_BUTTON, rawButtons[1].buttonId, 0, pinscape.stableId),
		makeBinding(VPE_INPUT_SLAM_TILT, 0, VPE_CONTROL_AXIS_ABOVE, 0, .5f, gamepad.stableId),
	};
	const VpeInputControlBinding gameplay[] = {
		makeBinding(VPE_INPUT_UPPER_LEFT_FLIPPER, 0, VPE_CONTROL_BUTTON, rawButton.buttonId, 0, pinscape.stableId),
		makeBinding(VPE_INPUT_START, 0, VPE_CONTROL_BUTTON, padButton.buttonId, 0, gamepad.stableId),
		makeBinding(VPE_INPUT_PLUNGE, 0, VPE_CONTROL_AXIS_ABOVE, 0, .1f, gamepad.stableId),
	};
	VpeInputSetControlBindings(gameplay, 3);
	Check(SDL_SetJoystickVirtualButton(pad, 0, true), "Press Start on the virtual gamepad");
	input.Poll(4020, true, callback, &observed);
	const uint64_t flipper = uint64_t{1} << VPE_INPUT_UPPER_LEFT_FLIPPER;
	const uint64_t start = uint64_t{1} << VPE_INPUT_START;
	Check(NativeButtons::GetState().PressedActions() == (flipper | start),
		"Map the joystick flipper and gamepad Start to gameplay actions");
	Check(SDL_SetJoystickVirtualButton(pad, 0, false), "Release Start");
	Check(SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 16384), "Move the gamepad stick");
	input.Poll(4050, true, callback, &observed);
	Check(NativeButtons::GetState().PressedActions() == flipper,
		"Release Start independently; ordinary gameplay actions do not accept axis thresholds");

	VpeInputSetControlBindings(controls, 4);
	Check(SDL_SetJoystickVirtualAxis(pad, 0, 24576) && SDL_SetJoystickVirtualHat(raw, 0, SDL_HAT_RIGHTUP),
		"Inject a stick and diagonal POV combination");
	input.Poll(4100, true, callback, &observed);
	const uint64_t slam = uint64_t{1} << VPE_INPUT_SLAM_TILT;
	Check(NativeButtons::GetState().PressedActions() == slam, "The complete combo suppresses its newly pressed subset");
	VpeInputListDeviceButtons(pinscape.deviceIndex, rawButtons.data(), 5);
	Check(rawButtons[1].isPressed && rawButtons[2].isPressed, "A diagonal holds both cardinal directions");
	Check(SDL_SetJoystickVirtualAxis(pad, 0, 14746), "Dither just below the positive press threshold");
	input.Poll(4150, true, callback, &observed);
	Check(NativeButtons::GetState().PressedActions() == slam, "Threshold hysteresis keeps the combo pressed during jitter");
	Check(SDL_SetJoystickVirtualAxis(pad, 0, -24576), "Move the axis to the alternative threshold");
	input.Poll(4200, true, callback, &observed);
	Check(NativeButtons::GetState().PressedActions() == nudge, "The negative axis threshold is an independent alternative");
	Check(SDL_SetJoystickVirtualAxis(pad, 0, 0), "Release the axis");
	input.Poll(4300, true, callback, &observed);
	Check(NativeButtons::GetState().PressedActions() == 0, "Releasing the combo does not replay its held subset");

	input.SetGameplayEnabled(false);
	VpeInputStartControlCapture(2);
	Check(SDL_SetJoystickVirtualButton(pad, 0, true), "Press a fresh button for capture");
	input.Poll(4400, true, callback, &observed);
	Check(SDL_SetJoystickVirtualAxis(pad, 0, 24576), "Move a stick while the button is held");
	input.Poll(4500, true, callback, &observed);
	Check(SDL_SetJoystickVirtualButton(pad, 0, false) && SDL_SetJoystickVirtualAxis(pad, 0, 19661),
		"Release the button while the stick still sits inside the capture hysteresis");
	input.Poll(4600, true, callback, &observed);
	VpeInputControlBinding captured[8]{};
	Check(VpeInputReadControlCapture(captured, 8) == 0, "Wait for the captured axis to release through its deadband");
	Check(SDL_SetJoystickVirtualAxis(pad, 0, 0), "Release the stick");
	input.Poll(4700, true, callback, &observed);
	Check(VpeInputReadControlCapture(captured, 8) == 2 && captured[0].controlId == padButton.buttonId
		&& captured[0].kind == VPE_CONTROL_BUTTON && captured[1].kind == VPE_CONTROL_AXIS_ABOVE
		&& std::strcmp(captured[0].deviceId, gamepad.stableId) == 0,
		"Capture the overlapping inputs during pause and ignore the hat held before capture");
	VpeInputStartControlCapture(0);
	input.SetGameplayEnabled(true);
	VpeInputSetButtonBindings(bindings, 2);

	const std::string rawStableId = pinscape.stableId;
	Check(SDL_DetachVirtualJoystick(rawId), "Unplug the raw joystick");
	SDL_CloseJoystick(raw);
	input.Poll(5000, true, callback, &observed);
	Check(observed.Find("PinscapePico").isConnected == 0, "Remove disconnected devices before the next scan");
	Check(input.ListAxes(pinscape.deviceIndex, nullptr, 0) == 0, "Remove the disconnected axis snapshot");
	Check(VpeInputListDeviceButtons(pinscape.deviceIndex, nullptr, 0) == 0
		&& NativeButtons::GetState().PressedActions() == 0, "Drop the held button when the joystick disconnects");
	const auto newRawId = Attach("PinscapePico", SDL_JOYSTICK_TYPE_UNKNOWN, 6, 1);
	input.Poll(260000, true, callback, &observed);
	const auto replacement = observed.Find("PinscapePico");
	Check(replacement.isConnected && replacement.deviceIndex != pinscape.deviceIndex
		&& rawStableId == replacement.stableId, "Reconnect with a stable mapping and fresh event index");

	Check(SDL_DetachVirtualJoystick(newRawId), "Detach the replacement fixture");
	Check(SDL_DetachVirtualJoystick(padId), "Detach the gamepad fixture");
	SDL_CloseJoystick(pad);
	input.Stop();
	Check(input.AppendDevices(nullptr, 0, 0) == 0, "Clear the controller snapshot on stop");
	input.Start();
	const auto restartedId = Attach("VPE Restart", SDL_JOYSTICK_TYPE_UNKNOWN, 0);
	input.Poll(270000, true, callback, &observed);
	Check(observed.Find("VPE Restart").isConnected && observed.Find("VPE Restart").axisCount == 0,
		"Detect button-only devices after restarting");
	Check(SDL_DetachVirtualJoystick(restartedId), "Detach the restart fixture");
	input.Stop();
	VpeInputSetButtonBindings(nullptr, 0);
}
