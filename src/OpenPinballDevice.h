// license:GPLv3+
#pragma once

#include "NativeInput.h"
#include <algorithm>
#include <cstddef>
#include <string_view>

namespace OpenPinballDevice {
	constexpr uint16_t UsagePage = 0x05;
	constexpr uint16_t Usage = 0x02;
	constexpr size_t ReportIdSize = 1;

	struct Axis {
		int id;
		const char* name;
		VpeAxisKind kind;
		size_t offset;
	};

	// Offsets in the OPD 1.x payload, excluding the HID report ID.
	constexpr Axis Axes[] = {
		{0, "Nudge X Acceleration", VPE_AXIS_KIND_ACCELERATION, 16},
		{1, "Nudge Y Acceleration", VPE_AXIS_KIND_ACCELERATION, 18},
		{2, "Nudge X Velocity", VPE_AXIS_KIND_VELOCITY, 20},
		{3, "Nudge Y Velocity", VPE_AXIS_KIND_VELOCITY, 22},
		{4, "Plunger Position", VPE_AXIS_KIND_POSITION, 24},
		{5, "Plunger Velocity", VPE_AXIS_KIND_VELOCITY, 26},
	};

	constexpr bool SupportsVersion(std::wstring_view signature) {
		constexpr std::wstring_view prefix = L"OpenPinballDeviceStruct/1.";
		if (signature.substr(0, prefix.size()) != prefix || signature.size() == prefix.size()) {
			return false;
		}
		for (const auto c : signature.substr(prefix.size())) {
			if (c < L'0' || c > L'9') return false;
		}
		return true;
	}

	inline uint64_t ReadUnsigned(const unsigned char* bytes, size_t size) {
		uint64_t value = 0;
		for (size_t i = 0; i < size; ++i) {
			value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
		}
		return value;
	}

	struct Report {
		const unsigned char* payload;
		size_t size;

		static bool Read(const unsigned char* bytes, size_t size, uint8_t reportId, Report& result) {
			// All 1.x reports begin with a timestamp and two button masks. Fields
			// after that header can be absent in shorter descriptors.
			if (size < ReportIdSize || bytes[0] != reportId) return false;
			return ReadPayload(bytes + ReportIdSize, size - ReportIdSize, result);
		}

		static bool ReadPayload(const unsigned char* bytes, size_t size, Report& result) {
			if (size < 16) return false;
			result = {bytes, size};
			return true;
		}

		uint64_t Timestamp() const { return ReadUnsigned(payload, 8); }
		uint32_t PinballButtons() const { return static_cast<uint32_t>(ReadUnsigned(payload + 12, 4)); }

		bool TryReadAxis(const Axis& axis, float& value) const {
			if (axis.offset + 2 > size) return false;
			const int bits = static_cast<int>(ReadUnsigned(payload + axis.offset, 2));
			value = static_cast<float>(bits < 32768 ? bits : bits - 65536) / 32768.0f;
			return true;
		}
	};

	// Each device boots with its own clock origin. Keep its intervals in VPE's
	// host clock domain without feeding future or backward times to physics.
	class Clock {
	public:
		bool Translate(uint64_t deviceUsec, int64_t hostUsec, int64_t& sampleUsec) {
			if (m_hasSample && deviceUsec == m_deviceUsec) return false;
			// Reanchor after a stall or accumulated device/host clock drift.
			constexpr int64_t MaxClockLagUsec = 20000;
			if (!m_hasSample || deviceUsec < m_deviceUsec || hostUsec - m_sampleUsec > MaxClockLagUsec) {
				m_sampleUsec = hostUsec;
			} else {
				const uint64_t remaining = static_cast<uint64_t>(std::max<int64_t>(hostUsec - m_sampleUsec, 0));
				m_sampleUsec += static_cast<int64_t>(std::min(deviceUsec - m_deviceUsec, remaining));
			}
			m_hasSample = true;
			m_deviceUsec = deviceUsec;
			sampleUsec = m_sampleUsec;
			return true;
		}

	private:
		bool m_hasSample = false;
		uint64_t m_deviceUsec = 0;
		int64_t m_sampleUsec = 0;
	};

	// Keep the last delivered level while gameplay is paused. A release that
	// happened during the pause is reconciled on resume, while a new press
	// during the pause must be released before it can drive gameplay.
	class ActionLatch {
	public:
		bool Update(bool pressed, bool enabled, bool& value) {
			const bool newPress = pressed && !m_wasPressed;
			m_wasPressed = pressed;
			if (!pressed) m_ignoreUntilReleased = false;
			if (!enabled) {
				if (newPress) m_ignoreUntilReleased = true;
				return false;
			}
			value = pressed && !m_ignoreUntilReleased;
			if (value == m_published) return false;
			m_published = value;
			return true;
		}

	private:
		bool m_published = false;
		bool m_ignoreUntilReleased = false;
		bool m_wasPressed = false;
	};

	// OPD preassigned button bits 0 through 24. Bits 25 and 26 are volume
	// controls, for which NativeInput has no action.
	constexpr VpeInputAction ButtonActions[] = {
		VPE_INPUT_START, VPE_INPUT_EXIT_GAME, VPE_INPUT_EXTRA_BALL,
		VPE_INPUT_COIN_INSERT_1, VPE_INPUT_COIN_INSERT_2, VPE_INPUT_COIN_INSERT_3, VPE_INPUT_COIN_INSERT_4,
		VPE_INPUT_PLUNGE, VPE_INPUT_LOCKBAR,
		VPE_INPUT_LEFT_FLIPPER, VPE_INPUT_RIGHT_FLIPPER,
		VPE_INPUT_LEFT_STAGED_FLIPPER, VPE_INPUT_RIGHT_STAGED_FLIPPER,
		VPE_INPUT_LEFT_MAGNASAVE, VPE_INPUT_RIGHT_MAGNASAVE,
		VPE_INPUT_TILT, VPE_INPUT_SLAM_TILT, VPE_INPUT_COIN_DOOR,
		VPE_INPUT_SERVICE_1, VPE_INPUT_SERVICE_2, VPE_INPUT_SERVICE_3, VPE_INPUT_SERVICE_4,
		VPE_INPUT_LEFT_NUDGE, VPE_INPUT_CENTER_NUDGE, VPE_INPUT_RIGHT_NUDGE,
	};
}
