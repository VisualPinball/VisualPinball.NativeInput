// license:GPLv3+
#include "ControllerInput.h"
#include "NativeButtons.h"

#if defined(VPE_PLATFORM_WINDOWS)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

#if defined(VPE_PLATFORM_WINDOWS)
#include <windows.h>
#elif defined(VPE_PLATFORM_LINUX)
#include <filesystem>
#include <fstream>
#endif

namespace {
	constexpr int64_t ScanIntervalUsec = 250000;
	constexpr int RawGamepadButtonBase = 0x10000;
	constexpr int HatButtonBase = 0x20000;
	constexpr std::array<Uint8, 4> HatDirections = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};
	constexpr std::array<const char*, SDL_GAMEPAD_AXIS_COUNT> GamepadAxisNames = {
		"Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y", "Left Trigger", "Right Trigger"
	};

	const char* ValueOrEmpty(const char* value) { return value == nullptr ? "" : value; }

	std::vector<VpeInputButtonInfo> DescribeButtons(SDL_Joystick* joystick, SDL_Gamepad* gamepad) {
		std::vector<VpeInputButtonInfo> buttons;
		const int rawCount = std::max(SDL_GetNumJoystickButtons(joystick), 0);
		std::vector<bool> mapped(rawCount);
		std::vector<Uint8> mappedHatDirections(std::max(SDL_GetNumJoystickHats(joystick), 0));
		if (gamepad != nullptr) {
			int count = 0;
			std::unique_ptr<SDL_GamepadBinding*, decltype(&SDL_free)> bindings(SDL_GetGamepadBindings(gamepad, &count), SDL_free);
			for (int i = 0; bindings && i < count; ++i) {
				const auto& binding = *bindings.get()[i];
				if (binding.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON && binding.input.button >= 0
					&& binding.input.button < rawCount) mapped[binding.input.button] = true;
				if (binding.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON && binding.input_type == SDL_GAMEPAD_BINDTYPE_HAT
					&& binding.input.hat.hat >= 0 && static_cast<size_t>(binding.input.hat.hat) < mappedHatDirections.size()) {
					mappedHatDirections[binding.input.hat.hat] |= static_cast<Uint8>(binding.input.hat.hat_mask);
				}
			}
			for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
				const auto id = static_cast<SDL_GamepadButton>(i);
				if (!SDL_GamepadHasButton(gamepad, id)) continue;
				VpeInputButtonInfo button{};
				button.buttonId = i;
				const auto* name = SDL_GetGamepadStringForButton(id);
				std::snprintf(button.name, sizeof(button.name), "%s", ValueOrEmpty(name));
				buttons.push_back(button);
			}
		}
		for (int i = 0; i < rawCount; ++i) {
			if (mapped[i]) continue;
			VpeInputButtonInfo button{};
			button.buttonId = gamepad == nullptr ? i : RawGamepadButtonBase + i;
			std::snprintf(button.name, sizeof(button.name), "Button %d", i + 1);
			buttons.push_back(button);
		}
		constexpr const char* directions[] = {"Up", "Right", "Down", "Left"};
		for (size_t hat = 0; hat < mappedHatDirections.size(); ++hat) {
			for (size_t direction = 0; direction < HatDirections.size(); ++direction) {
				if (mappedHatDirections[hat] & HatDirections[direction]) continue;
				VpeInputButtonInfo button{};
				button.buttonId = HatButtonBase + static_cast<int>(hat * HatDirections.size() + direction);
				std::snprintf(button.name, sizeof(button.name), "Hat %zu %s", hat + 1, directions[direction]);
				buttons.push_back(button);
			}
		}
		return buttons;
	}

	bool UsesPinscapeLayout(const char* name) {
		return SDL_strncasecmp(name, "mjrnet Pinscape Controller", sizeof("mjrnet Pinscape Controller") - 1) == 0
			|| SDL_strcasecmp(name, "Pinscape Controller") == 0
			|| SDL_strcasecmp(name, "PinscapePico") == 0
			|| SDL_strcasecmp(name, "Pinscape Pico") == 0
			|| SDL_strcasecmp(name, "mjrnet PinscapePico") == 0
			|| SDL_strcasecmp(name, "mjrgh PinscapePico") == 0;
	}

	bool IsWindowsHidPath(const char* path) {
#if defined(VPE_PLATFORM_WINDOWS)
		// The Windows HID backend retains these raw axes and their saved
		// mapping IDs. Standard gamepad axes use a separate SDL device.
		return SDL_strncasecmp(path, R"(\\?\hid#)", 8) == 0;
#else
		(void)path;
		return false;
#endif
	}

	std::string StableLocation(std::string_view path) {
#if defined(VPE_PLATFORM_LINUX)
		// Kernel node numbers change on reconnect. The input or HID physical
		// path (USB port and interface, or Bluetooth address) survives it.
		if (path.rfind("/dev/input/", 0) == 0) {
			const auto node = std::filesystem::path(path).filename();
			std::ifstream file(std::filesystem::path("/sys/class/input") / node / "device/phys");
			std::string phys;
			if (std::getline(file, phys)) return phys;
		} else if (path.rfind("/dev/hidraw", 0) == 0) {
			const auto node = std::filesystem::path(path).filename();
			std::ifstream file(std::filesystem::path("/sys/class/hidraw") / node / "device/uevent");
			for (std::string line; std::getline(file, line);) {
				if (line.rfind("HID_PHYS=", 0) == 0) return line.substr(9);
			}
		}
		return {};
#else
		return std::string(path);
#endif
	}

	std::string IdBase(SDL_Joystick* joystick, const char* path, bool gamepad) {
		char guid[33]{};
		SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));
		const auto identity = StableLocation(path) + '\n' + ValueOrEmpty(SDL_GetJoystickSerial(joystick));
		uint64_t hash = 14695981039346656037ULL;
		for (unsigned char byte : identity) hash = (hash ^ byte) * 1099511628211ULL;
		char id[80];
		std::snprintf(id, sizeof(id), "sdl-%s:%s:%016llx:", gamepad ? "gamepad" : "joystick",
			guid, static_cast<unsigned long long>(hash));
		return id;
	}

	void PumpDeviceNotifications() {
#if defined(VPE_PLATFORM_WINDOWS)
		// SDL's joystick notification window lives on this thread. Service it
		// without initializing SDL video or pumping the host application's UI.
		MSG message;
		for (int i = 0; i < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
			DispatchMessageW(&message);
		}
#endif
	}
}

struct ControllerInput::Device {
	SDL_JoystickID id;
	SDL_Joystick* joystick = nullptr;
	SDL_Gamepad* gamepad = nullptr;
	VpeInputDeviceInfo info{};
	std::vector<VpeInputAxisInfo> axes;

	explicit Device(SDL_JoystickID id) : id(id) {}
	~Device() {
		if (gamepad != nullptr) SDL_CloseGamepad(gamepad);
		else if (joystick != nullptr) SDL_CloseJoystick(joystick);
	}
};

ControllerInput::ControllerInput() = default;
ControllerInput::~ControllerInput() = default;

void ControllerInput::Start() {
	// Only the joystick/gamepad subsystem is initialized here. IOKit attaches
	// its run loop and Win32 creates its notification window on this thread.
	SDL_SetMainReady();
	SDL_SetHintWithPriority(SDL_HINT_NO_SIGNAL_HANDLERS, "1", SDL_HINT_OVERRIDE);
	SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_THREAD, "0", SDL_HINT_OVERRIDE);
	SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_RAWINPUT, "0", SDL_HINT_OVERRIDE);
	SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE);
	m_started = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	if (m_started) {
		SDL_SetJoystickEventsEnabled(false);
		SDL_SetGamepadEventsEnabled(false);
		SDL_SetEventEnabled(SDL_EVENT_JOYSTICK_UPDATE_COMPLETE, false);
	} else {
		SDL_LogError(SDL_LOG_CATEGORY_INPUT, "VPE controller input initialization failed: %s", SDL_GetError());
	}
	m_wasPublishing = false;
	m_nextScan = 0;
	m_nextIndex = FirstDeviceIndex;
}

void ControllerInput::Stop() {
	{
		std::lock_guard<std::mutex> lock(m_snapshotMutex);
		for (const auto& device : m_devices) NativeButtons::GetState().Disconnect(device->info.deviceIndex);
		m_devices.clear();
	}
	if (m_started) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
	m_started = false;
}

void ControllerInput::SetGameplayEnabled(bool enabled) {
	if (enabled) m_refreshOnResume.store(true);
	m_gameplayEnabled.store(enabled);
}

bool ControllerInput::Reconcile(int64_t timestamp) {
	bool changed = false;
	const auto removed = std::remove_if(m_devices.begin(), m_devices.end(), [](const auto& device) {
		if (SDL_JoystickConnected(device->joystick)) return false;
		NativeButtons::GetState().Disconnect(device->info.deviceIndex);
		return true;
	});
	if (removed != m_devices.end()) {
		m_devices.erase(removed, m_devices.end());
		changed = true;
	}
	if (timestamp < m_nextScan) return changed;
	m_nextScan = timestamp + ScanIntervalUsec;

	int count = 0;
	std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)> ids(SDL_GetJoysticks(&count), SDL_free);
	if (!ids) return changed;
	for (int i = 0; i < count; ++i) {
		const auto id = ids.get()[i];
		if (std::any_of(m_devices.begin(), m_devices.end(), [id](const auto& device) { return device->id == id; })) continue;

		const auto* path = ValueOrEmpty(SDL_GetJoystickPathForID(id));
		const auto* name = ValueOrEmpty(SDL_GetJoystickNameForID(id));
		const bool pinscape = UsesPinscapeLayout(name);
		const bool standardized = SDL_IsGamepad(id) && !pinscape;
		if (!standardized && IsWindowsHidPath(path)) continue;

		auto device = std::make_unique<Device>(id);
		if (standardized) {
			device->gamepad = SDL_OpenGamepad(id);
			if (device->gamepad == nullptr) continue;
			device->joystick = SDL_GetGamepadJoystick(device->gamepad);
		} else {
			device->joystick = SDL_OpenJoystick(id);
		}
		if (device->joystick == nullptr) continue;

		const auto base = IdBase(device->joystick, path, standardized);
		int slot = 0;
		std::string stableId;
		do {
			stableId = base + std::to_string(slot++);
		} while (std::any_of(m_devices.begin(), m_devices.end(), [&](const auto& existing) {
			return stableId == existing->info.stableId;
		}));

		device->info.deviceIndex = m_nextIndex++;
		device->info.isConnected = 1;
		std::snprintf(device->info.stableId, sizeof(device->info.stableId), "%s", stableId.c_str());
		const auto* label = standardized ? ValueOrEmpty(SDL_GetGamepadName(device->gamepad)) : name;
		std::snprintf(device->info.displayName, sizeof(device->info.displayName), "%.*s%s",
			static_cast<int>(sizeof(device->info.displayName) - sizeof(" (Gamepad)")),
			*label != '\0' ? label : "Controller", standardized ? " (Gamepad)" : "");

		const int axisCount = standardized ? static_cast<int>(SDL_GAMEPAD_AXIS_COUNT) : SDL_GetNumJoystickAxes(device->joystick);
		constexpr const char* PinscapeAxisNames[] = { "X", "Y", "Z", "RX", "RY", "RZ" };
		for (int axis = 0; axis < axisCount; ++axis) {
			VpeInputAxisInfo info{};
			info.axisId = axis;
			info.kind = VPE_AXIS_KIND_POSITION;
			if (standardized) {
				if (!SDL_GamepadHasAxis(device->gamepad, static_cast<SDL_GamepadAxis>(axis))) continue;
				info.usagePage = VPE_GAMEPAD_AXIS_USAGE_PAGE;
				info.usage = axis;
				std::snprintf(info.name, sizeof(info.name), "%s", GamepadAxisNames[axis]);
			} else if (pinscape && axisCount >= 6 && axis < 6) {
				// The known six-axis Pinscape joystick layout, as exposed by
				// SDL, matches the usage-based preset used by Windows HID.
				info.usagePage = 0x01;
				info.usage = 0x30 + axis;
				std::snprintf(info.name, sizeof(info.name), "%s", PinscapeAxisNames[axis]);
			} else {
				std::snprintf(info.name, sizeof(info.name), "Axis %d", axis + 1);
			}
			device->axes.push_back(info);
		}
		auto buttons = DescribeButtons(device->joystick, device->gamepad);
		if (device->axes.empty() && buttons.empty()) continue;
		std::vector<int> axisIds;
		for (const auto& axis : device->axes) axisIds.push_back(axis.axisId);
		NativeButtons::GetState().Connect(device->info.deviceIndex, stableId, std::move(buttons), axisIds);
		device->info.axisCount = static_cast<int>(device->axes.size());
		m_devices.push_back(std::move(device));
		changed = true;
	}
	return changed;
}

void ControllerInput::Poll(int64_t timestamp, bool foreground, VpeInputEventCallback callback, void* userData) {
	if (!m_started) return;
	PumpDeviceNotifications();
	SDL_UpdateJoysticks();
	// MFi notifications can change SDL's device list on the OS thread.
	SDL_LockJoysticks();
	const bool publish = foreground && m_gameplayEnabled.load();
	const bool refresh = publish && (m_refreshOnResume.exchange(false) || !m_wasPublishing);
	m_axisEvents.clear();
	bool changed;
	{
		std::lock_guard<std::mutex> lock(m_snapshotMutex);
		changed = Reconcile(timestamp);
		for (auto& device : m_devices) {
			for (auto& axis : device->axes) {
				const auto value = device->gamepad != nullptr
					? SDL_GetGamepadAxis(device->gamepad, static_cast<SDL_GamepadAxis>(axis.axisId))
					: SDL_GetJoystickAxis(device->joystick, axis.axisId);
				const float normalized = static_cast<float>(value) / 32768.0f;
				if (publish && (refresh || axis.timestampUsec == 0 || normalized != axis.rawValue)) {
					m_axisEvents.push_back({timestamp, VPE_INPUT_EVENT_AXIS, 0, device->info.deviceIndex, axis.axisId, normalized, 0});
				}
				axis.rawValue = normalized;
				axis.timestampUsec = timestamp;
			}
			auto axis = device->axes.begin();
			NativeButtons::GetState().Update(device->info.deviceIndex, [&](const auto& button) {
				if (button.buttonId >= HatButtonBase) {
					const auto offset = static_cast<size_t>(button.buttonId - HatButtonBase);
					const auto hat = SDL_GetJoystickHat(device->joystick, static_cast<int>(offset / HatDirections.size()));
					return (hat & HatDirections[offset % HatDirections.size()]) != 0;
				}
				if (device->gamepad == nullptr) return SDL_GetJoystickButton(device->joystick, button.buttonId);
				if (button.buttonId >= RawGamepadButtonBase) {
					return SDL_GetJoystickButton(device->joystick, button.buttonId - RawGamepadButtonBase);
				}
				return SDL_GetGamepadButton(device->gamepad, static_cast<SDL_GamepadButton>(button.buttonId));
			}, [&](int, float) { return (axis++)->rawValue; });
		}
	}
	SDL_UnlockJoysticks();
	m_wasPublishing = publish;
	if (callback == nullptr) return;
	if (changed) {
		const VpeInputEvent event{timestamp, VPE_INPUT_EVENT_DEVICES_CHANGED, 0, 0, 0, 0, 0};
		callback(&event, userData);
	}
	for (const auto& event : m_axisEvents) callback(&event, userData);
}

int ControllerInput::AppendDevices(VpeInputDeviceInfo* devices, int maxDevices, int existingCount) {
	std::lock_guard<std::mutex> lock(m_snapshotMutex);
	const int count = static_cast<int>(m_devices.size());
	if (devices != nullptr && maxDevices > existingCount) {
		for (int i = 0; i < std::min(count, maxDevices - existingCount); ++i) {
			devices[existingCount + i] = m_devices[i]->info;
		}
	}
	return existingCount + count;
}

int ControllerInput::ListAxes(int deviceIndex, VpeInputAxisInfo* axes, int maxAxes) {
	std::lock_guard<std::mutex> lock(m_snapshotMutex);
	for (const auto& device : m_devices) {
		if (device->info.deviceIndex != deviceIndex) continue;
		const int count = device->info.axisCount;
		if (axes != nullptr && maxAxes > 0) std::copy_n(device->axes.begin(), std::min(count, maxAxes), axes);
		return count;
	}
	return 0;
}
