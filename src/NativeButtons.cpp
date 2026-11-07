// license:GPLv3+
#include "NativeButtons.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <utility>

namespace NativeButtons {
	namespace {
		bool IsNudgeOrTilt(int action) {
			return action == VPE_INPUT_LEFT_NUDGE || action == VPE_INPUT_RIGHT_NUDGE
				|| action == VPE_INPUT_CENTER_NUDGE || action == VPE_INPUT_TILT || action == VPE_INPUT_SLAM_TILT;
		}
	}

	State& GetState() {
		static State state;
		return state;
	}

	void State::Connect(int deviceIndex, const std::string& stableId, std::vector<VpeInputButtonInfo> buttons) {
		if (buttons.empty()) return;
		std::lock_guard<std::mutex> lock(m_mutex);
		m_devices.push_back({deviceIndex, stableId.substr(0, VPE_INPUT_DEVICE_ID_SIZE - 1), std::move(buttons)});
	}

	void State::Disconnect(int deviceIndex) {
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto removed = std::remove_if(m_devices.begin(), m_devices.end(), [deviceIndex](const Device& device) {
			return device.index == deviceIndex;
		});
		m_devices.erase(removed, m_devices.end());
	}

	void State::Configure(const VpeInputButtonBinding* bindings, int count) {
		std::vector<VpeInputButtonBinding> valid;
		for (int i = 0; bindings != nullptr && i < count; ++i) {
			const auto& binding = bindings[i];
			if (!IsNudgeOrTilt(binding.action) || binding.buttonId < 0 || binding.deviceId[0] == '\0'
				|| std::find(std::begin(binding.deviceId), std::end(binding.deviceId), '\0') == std::end(binding.deviceId)) continue;
			valid.push_back(binding);
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		m_bindings.swap(valid);
	}

	int State::List(int deviceIndex, VpeInputButtonInfo* buttons, int maxButtons) {
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const auto& device : m_devices) {
			if (device.index != deviceIndex) continue;
			const int count = static_cast<int>(device.buttons.size());
			if (buttons != nullptr && maxButtons > 0) std::copy_n(device.buttons.begin(), std::min(count, maxButtons), buttons);
			return count;
		}
		return 0;
	}

	uint64_t State::PressedActions() {
		std::lock_guard<std::mutex> lock(m_mutex);
		uint64_t actions = 0;
		for (const auto& binding : m_bindings) {
			for (const auto& device : m_devices) {
				if (device.id != binding.deviceId) continue;
				const bool pressed = std::any_of(device.buttons.begin(), device.buttons.end(), [&](const auto& button) {
					return button.buttonId == binding.buttonId && button.isPressed != 0;
				});
				if (pressed) actions |= uint64_t{1} << binding.action;
			}
		}
		return actions;
	}

	std::vector<VpeInputButtonInfo> OpdGenericButtons() {
		std::vector<VpeInputButtonInfo> buttons(32);
		for (int i = 0; i < 32; ++i) {
			buttons[i].buttonId = i;
			std::snprintf(buttons[i].name, sizeof(buttons[i].name), "Generic Button %d", i + 1);
		}
		return buttons;
	}

	bool UpdateOpdGenericButtons(int deviceIndex, uint32_t mask) {
		return GetState().Update(deviceIndex, [mask](const auto& button) {
			return (mask & (uint32_t{1} << button.buttonId)) != 0;
		});
	}
}

extern "C" {
	VPE_API void VpeInputSetButtonBindings(const VpeInputButtonBinding* bindings, int count) {
		NativeButtons::GetState().Configure(bindings, count);
	}

	VPE_API int VpeInputListDeviceButtons(int deviceIndex, VpeInputButtonInfo* buttons, int maxButtons) {
		return NativeButtons::GetState().List(deviceIndex, buttons, maxButtons);
	}
}
