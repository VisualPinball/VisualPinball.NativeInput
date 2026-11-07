// license:GPLv3+
#pragma once

#include "NativeInput.h"

#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace NativeButtons {
	class State {
	public:
		void Connect(int deviceIndex, const std::string& stableId, std::vector<VpeInputButtonInfo> buttons,
			const std::vector<int>& axisIds = {});
		void Disconnect(int deviceIndex);
		void Configure(const VpeInputButtonBinding* bindings, int count);
		void ConfigureControls(const VpeInputControlBinding* controls, int count);
		void StartCapture(int mode);
		int ReadCapture(VpeInputControlBinding* controls, int maxControls);
		int List(int deviceIndex, VpeInputButtonInfo* buttons, int maxButtons);
		uint64_t PressedActions();

		template <typename Sample>
		bool Update(int deviceIndex, Sample sample) {
			return Update(deviceIndex, sample, [](int, float previous) { return previous; });
		}

		template <typename SampleButton, typename SampleAxis>
		bool Update(int deviceIndex, SampleButton sampleButton, SampleAxis sampleAxis) {
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& device : m_devices) {
				if (device.index != deviceIndex) continue;
				bool changed = false;
				for (auto& button : device.buttons) {
					const bool pressed = sampleButton(button);
					const bool wasPressed = button.isPressed != 0;
					changed |= pressed != wasPressed;
					if (m_captureMode && device.buttonsSampled && pressed && !wasPressed)
						CaptureCandidate(device.id, VPE_CONTROL_BUTTON, button.buttonId, 0.0f);
					button.isPressed = pressed;
				}
				device.buttonsSampled = true;
				for (auto& axis : device.axes) {
					const float value = sampleAxis(axis.id, axis.value);
					changed |= m_hasAxisBindings && value != axis.value
						&& (std::isfinite(value) || std::isfinite(axis.value));
					if (m_captureMode && std::isfinite(axis.value)) {
						if (value >= CaptureThreshold && axis.value < CaptureThreshold)
							CaptureCandidate(device.id, VPE_CONTROL_AXIS_ABOVE, axis.id, CaptureThreshold);
						if (value <= -CaptureThreshold && axis.value > -CaptureThreshold)
							CaptureCandidate(device.id, VPE_CONTROL_AXIS_BELOW, axis.id, -CaptureThreshold);
					}
					axis.value = value;
				}
				if (m_captureMode) AdvanceCapture();
				return changed;
			}
			return false;
		}

	private:
		static constexpr float CaptureThreshold = 0.65f;

		struct Axis {
			int id;
			float value = std::numeric_limits<float>::quiet_NaN();
		};

		struct Device {
			int index;
			std::string id;
			std::vector<VpeInputButtonInfo> buttons;
			std::vector<Axis> axes;
			bool buttonsSampled = false;
		};

		struct Binding {
			int action;
			int group;
			struct Member {
				VpeInputControlBinding control;
				bool pressed = false;
			};
			std::vector<Member> controls;
			bool wasPressed = false;
			bool pressed = false;
			bool suppressed = false;
		};

		bool ControlPressed(const VpeInputControlBinding& control, bool wasPressed) const;
		void CaptureCandidate(const std::string& id, int kind, int controlId, float threshold);
		void AdvanceCapture();

		std::mutex m_mutex;
		std::vector<Device> m_devices;
		std::vector<Binding> m_bindings;
		bool m_hasAxisBindings = false;
		int m_captureMode = 0;
		bool m_captureReleased = false;
		std::vector<VpeInputControlBinding> m_capture;
		std::vector<VpeInputControlBinding> m_captureCandidates;
	};

	State& GetState();
	std::vector<VpeInputButtonInfo> OpdGenericButtons();
}
