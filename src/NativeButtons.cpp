// license:GPLv3+
#include "NativeButtons.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

static_assert(sizeof(VpeInputControlBinding) == 280);

namespace NativeButtons {
	namespace {
		bool IsNudgeOrTilt(int action) {
			return action == VPE_INPUT_LEFT_NUDGE || action == VPE_INPUT_RIGHT_NUDGE
				|| action == VPE_INPUT_CENTER_NUDGE || action == VPE_INPUT_TILT || action == VPE_INPUT_SLAM_TILT;
		}

		bool SameControl(const VpeInputControlBinding& a, const VpeInputControlBinding& b) {
			return a.kind == b.kind && a.controlId == b.controlId && a.threshold == b.threshold
				&& std::strcmp(a.deviceId, b.deviceId) == 0;
		}
	}

	State& GetState() {
		static State state;
		return state;
	}

	void State::Connect(int deviceIndex, const std::string& stableId, std::vector<VpeInputButtonInfo> buttons,
		const std::vector<int>& axisIds) {
		if (buttons.empty() && axisIds.empty()) return;
		Device device{deviceIndex, stableId.substr(0, VPE_INPUT_DEVICE_ID_SIZE - 1), std::move(buttons), {}};
		for (int id : axisIds) device.axes.push_back({id});
		std::lock_guard<std::mutex> lock(m_mutex);
		m_devices.push_back(std::move(device));
	}

	void State::Disconnect(int deviceIndex) {
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto removed = std::remove_if(m_devices.begin(), m_devices.end(), [&](const Device& device) {
			if (device.index != deviceIndex) return false;
			if (m_captureMode && std::any_of(m_capture.begin(), m_capture.end(), [&](const auto& control) {
				return device.id == control.deviceId;
			})) {
				m_capture.clear();
				m_captureReleased = false;
			}
			return true;
		});
		m_devices.erase(removed, m_devices.end());
	}

	void State::Configure(const VpeInputButtonBinding* bindings, int count) {
		std::vector<VpeInputControlBinding> controls;
		for (int i = 0; bindings != nullptr && i < count; ++i) {
			const auto& binding = bindings[i];
			VpeInputControlBinding control{binding.action, i, VPE_CONTROL_BUTTON, binding.buttonId, 0.0f, {}};
			std::copy_n(binding.deviceId, VPE_INPUT_DEVICE_ID_SIZE, control.deviceId);
			controls.push_back(control);
		}
		ConfigureControls(controls.data(), static_cast<int>(controls.size()));
	}

	void State::ConfigureControls(const VpeInputControlBinding* controls, int count) {
		std::vector<Binding> groups;
		bool hasAxisBindings = false;
		for (int i = 0; controls != nullptr && i < count; ++i) {
			const auto& control = controls[i];
			if (!IsNudgeOrTilt(control.action) || control.group < 0) continue;
			auto group = std::find_if(groups.begin(), groups.end(), [&](const Binding& binding) {
				return binding.action == control.action && binding.group == control.group;
			});
			if (group == groups.end()) {
				groups.push_back({control.action, control.group, {}});
				group = std::prev(groups.end());
			}
			group->controls.push_back({control});
		}
		groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const Binding& group) {
			// Reject the entire group if any member is malformed.
			return std::any_of(group.controls.begin(), group.controls.end(), [](const auto& member) {
				const auto& control = member.control;
				const bool validThreshold = control.kind == VPE_CONTROL_BUTTON
					|| (control.kind == VPE_CONTROL_AXIS_ABOVE && control.threshold > 0.0f)
					|| (control.kind == VPE_CONTROL_AXIS_BELOW && control.threshold < 0.0f);
				return !validThreshold || control.controlId < 0 || !std::isfinite(control.threshold)
					|| std::fabs(control.threshold) > 1.0f || control.deviceId[0] == '\0'
					|| std::find(std::begin(control.deviceId), std::end(control.deviceId), '\0') == std::end(control.deviceId);
			});
		}), groups.end());
		for (auto& group : groups) {
			auto& members = group.controls;
			for (auto& member : members) {
				if (member.control.kind == VPE_CONTROL_BUTTON) member.control.threshold = 0.0f;
				else hasAxisBindings = true;
			}
			for (auto member = members.begin(); member != members.end(); ++member) {
				members.erase(std::remove_if(std::next(member), members.end(), [&](const auto& other) {
					return SameControl(member->control, other.control);
				}), members.end());
			}
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		m_bindings.swap(groups);
		m_hasAxisBindings = hasAxisBindings;
	}

	void State::StartCapture(int mode) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_captureMode = mode == 1 || mode == 2 ? mode : 0;
		m_captureReleased = false;
		m_capture.clear();
		m_captureCandidates.clear();
	}

	int State::ReadCapture(VpeInputControlBinding* controls, int maxControls) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_captureMode != 0) return 0;
		const int count = static_cast<int>(m_capture.size());
		if (controls != nullptr && maxControls > 0) std::copy_n(m_capture.begin(), std::min(count, maxControls), controls);
		return count;
	}

	void State::CaptureCandidate(const std::string& id, int kind, int controlId, float threshold) {
		if (m_captureReleased || m_captureCandidates.size() >= 8) return;
		VpeInputControlBinding control{0, 0, kind, controlId, threshold, {}};
		std::snprintf(control.deviceId, sizeof(control.deviceId), "%s", id.c_str());
		m_captureCandidates.push_back(control);
	}

	void State::AdvanceCapture() {
		const auto held = [this](const auto& control) { return ControlPressed(control, true); };
		if (!m_capture.empty() && !std::all_of(m_capture.begin(), m_capture.end(), held)) m_captureReleased = true;
		if (!m_captureReleased) {
			for (const auto& candidate : m_captureCandidates) {
				if (m_capture.size() >= (m_captureMode == 1 ? 1 : 8)) break;
				if (std::none_of(m_capture.begin(), m_capture.end(), [&](const auto& member) {
					return SameControl(member, candidate);
				})) m_capture.push_back(candidate);
			}
		} else if (std::none_of(m_capture.begin(), m_capture.end(), held)) {
			if (m_captureMode == 2 && m_capture.size() < 2) {
				m_capture.clear();
				m_captureReleased = false;
			} else {
				m_captureMode = 0;
			}
		}
		m_captureCandidates.clear();
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
		for (auto& binding : m_bindings) {
			binding.pressed = true;
			for (auto& member : binding.controls) {
				member.pressed = ControlPressed(member.control, member.pressed);
				binding.pressed &= member.pressed;
			}
			if (!binding.pressed) binding.suppressed = false;
		}
		// When a combo and a shorter subset complete together, publish only
		// the combo. The shorter binding must release before it can fire again.
		for (const auto& combo : m_bindings) {
			if (!combo.pressed || combo.wasPressed || combo.controls.size() < 2) continue;
			for (auto& shorter : m_bindings) {
				if (!shorter.pressed || shorter.wasPressed || shorter.controls.size() >= combo.controls.size()) continue;
				if (std::all_of(shorter.controls.begin(), shorter.controls.end(), [&](const auto& control) {
					return std::any_of(combo.controls.begin(), combo.controls.end(), [&](const auto& member) {
						return SameControl(control.control, member.control);
					});
				})) shorter.suppressed = true;
			}
		}
		uint64_t actions = 0;
		for (auto& binding : m_bindings) {
			if (binding.pressed && !binding.suppressed) actions |= uint64_t{1} << binding.action;
			binding.wasPressed = binding.pressed;
		}
		return actions;
	}

	bool State::ControlPressed(const VpeInputControlBinding& control, bool wasPressed) const {
		constexpr float releaseMargin = 0.1f;
		for (const auto& device : m_devices) {
			if (device.id != control.deviceId) continue;
			if (control.kind == VPE_CONTROL_BUTTON) {
				for (const auto& button : device.buttons) {
					if (button.buttonId == control.controlId && button.isPressed) return true;
				}
			} else {
				for (const auto& axis : device.axes) {
					if (axis.id != control.controlId || !std::isfinite(axis.value)) continue;
					if (control.kind == VPE_CONTROL_AXIS_ABOVE) {
						if (wasPressed ? axis.value > std::max(0.0f, control.threshold - releaseMargin) : axis.value >= control.threshold)
							return true;
					} else {
						if (wasPressed ? axis.value < std::min(0.0f, control.threshold + releaseMargin) : axis.value <= control.threshold)
							return true;
					}
				}
			}
		}
		return false;
	}

	std::vector<VpeInputButtonInfo> OpdGenericButtons() {
		std::vector<VpeInputButtonInfo> buttons(32);
		for (int i = 0; i < 32; ++i) {
			buttons[i].buttonId = i;
			std::snprintf(buttons[i].name, sizeof(buttons[i].name), "Generic Button %d", i + 1);
		}
		return buttons;
	}

}

extern "C" {
	VPE_API void VpeInputSetButtonBindings(const VpeInputButtonBinding* bindings, int count) {
		NativeButtons::GetState().Configure(bindings, count);
	}

	VPE_API int VpeInputListDeviceButtons(int deviceIndex, VpeInputButtonInfo* buttons, int maxButtons) {
		return NativeButtons::GetState().List(deviceIndex, buttons, maxButtons);
	}

	VPE_API void VpeInputSetControlBindings(const VpeInputControlBinding* controls, int count) {
		NativeButtons::GetState().ConfigureControls(controls, count);
	}

	VPE_API void VpeInputStartControlCapture(int mode) {
		NativeButtons::GetState().StartCapture(mode);
	}

	VPE_API int VpeInputReadControlCapture(VpeInputControlBinding* controls, int maxControls) {
		return NativeButtons::GetState().ReadCapture(controls, maxControls);
	}
}
