// license:GPLv3+
#pragma once

#include "NativeInput.h"

#include <mutex>
#include <string>
#include <vector>

namespace NativeButtons {
	class State {
	public:
		void Connect(int deviceIndex, const std::string& stableId, std::vector<VpeInputButtonInfo> buttons);
		void Disconnect(int deviceIndex);
		void Configure(const VpeInputButtonBinding* bindings, int count);
		int List(int deviceIndex, VpeInputButtonInfo* buttons, int maxButtons);
		uint64_t PressedActions();

		template <typename Sample>
		bool Update(int deviceIndex, Sample sample) {
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& device : m_devices) {
				if (device.index != deviceIndex) continue;
				bool changed = false;
				for (auto& button : device.buttons) {
					const bool pressed = sample(button);
					changed |= pressed != (button.isPressed != 0);
					button.isPressed = pressed;
				}
				return changed;
			}
			return false;
		}

	private:
		struct Device {
			int index;
			std::string id;
			std::vector<VpeInputButtonInfo> buttons;
		};

		std::mutex m_mutex;
		std::vector<Device> m_devices;
		std::vector<VpeInputButtonBinding> m_bindings;
	};

	State& GetState();
	std::vector<VpeInputButtonInfo> OpdGenericButtons();
	bool UpdateOpdGenericButtons(int deviceIndex, uint32_t mask);
}
