// license:GPLv3+
#pragma once

#include "OpenPinballDevice.h"

#include <array>
#include <limits>
#include <vector>

namespace OpenPinballDevice {
	constexpr size_t MaxDescriptorSize = 4096;
	constexpr size_t MaxReportSize = 4096;
	enum class ProbeResult { Found, NotOpd, Retry };

	constexpr int64_t NextProbe(ProbeResult result, int64_t timestamp) {
		// Recheck access failures in case the OS is still setting permissions.
		return result == ProbeResult::NotOpd ? std::numeric_limits<int64_t>::max() : timestamp + 5000000;
	}

	struct Descriptor {
		uint8_t reportId = 0;
		uint8_t stringIndex = 0;
		size_t payloadSize = 0;
	};

	// OPD puts its whole payload in one opaque byte-array Input item. Walk the
	// surrounding HID items to locate it even on a composite HID interface.
	inline Descriptor FindDescriptor(const unsigned char* bytes, size_t size) {
		struct Globals {
			uint32_t usagePage = 0;
			uint32_t reportSize = 0;
			uint32_t reportCount = 0;
			uint8_t reportId = 0;
		} globals;
		struct Locals {
			uint32_t usage = 0;
			uint32_t stringIndex = 0;
			bool hasUsage = false;
			bool complex = false;
		} locals;
		std::vector<Globals> stack;
		std::array<uint64_t, 256> inputBits{};
		Descriptor match;
		size_t depth = 0;
		bool isPinball = false;

		for (size_t offset = 0; offset < size;) {
			const auto item = bytes[offset++];
			if (item == 0xfe) {
				if (size - offset < 2 || bytes[offset] > size - offset - 2) return {};
				offset += 2 + bytes[offset];
				continue;
			}
			const size_t code = item & 3;
			const size_t length = code == 3 ? 4 : code;
			if (length > size - offset) return {};
			const auto value = static_cast<uint32_t>(ReadUnsigned(bytes + offset, length));
			offset += length;

			switch (item & 0xfc) {
				case 0x04: globals.usagePage = value; break;
				case 0x74: globals.reportSize = value; break;
				case 0x84:
					if (value == 0 || value > 255) return {};
					globals.reportId = static_cast<uint8_t>(value);
					break;
				case 0x94: globals.reportCount = value; break;
				case 0xa4: stack.push_back(globals); break;
				case 0xb4:
					if (stack.empty()) return {};
					globals = stack.back();
					stack.pop_back();
					break;
				case 0x08:
					locals.complex |= locals.hasUsage;
					locals.hasUsage = true;
					locals.usage = length == 4 ? value : (globals.usagePage << 16) | value;
					break;
				case 0x78:
					locals.complex |= locals.stringIndex != 0;
					locals.stringIndex = value;
					break;
				case 0x18: case 0x28: // Usage Minimum/Maximum
				case 0x88: case 0x98: // String Minimum/Maximum
				case 0xa8:            // Delimiter
					locals.complex = true;
					break;
				case 0xa0:
					if (depth++ == 0) {
						isPinball = value == 1 && !locals.complex && locals.hasUsage
							&& locals.usage == ((uint32_t{UsagePage} << 16) | Usage);
					}
					break;
				case 0xc0:
					if (depth == 0) return {};
					if (--depth == 0) isPinball = false;
					break;
				case 0x80: {
					if (globals.reportSize > MaxReportSize * 8 || globals.reportCount > MaxReportSize * 8) return {};
					inputBits[globals.reportId] += uint64_t{globals.reportSize} * globals.reportCount;
					if (isPinball && (value & 0x03) == 0 && globals.reportSize == 8
						&& globals.reportCount >= 16 && globals.reportCount <= MaxReportSize - (globals.reportId != 0)
						&& !locals.complex && locals.hasUsage && locals.usage == (uint32_t{UsagePage} << 16)
						&& locals.stringIndex > 0 && locals.stringIndex <= 255) {
						if (match.stringIndex != 0) return {};
						match = {globals.reportId, static_cast<uint8_t>(locals.stringIndex), globals.reportCount};
					}
					break;
				}
			}
			if ((item & 0x0c) == 0) locals = {};
		}

		if (depth != 0 || !stack.empty() || inputBits[match.reportId] != match.payloadSize * 8) return {};
		return match;
	}

	// Read the USB language table first. The OPD signature itself is ASCII
	// encoded as a USB UTF-16LE string descriptor.
	template <typename ReadString>
	ProbeResult VerifySignature(uint8_t index, ReadString readString) {
		std::array<unsigned char, 255> bytes{};
		const auto length = [&](int size) {
			return size >= 2 && bytes[1] == 3 && bytes[0] <= size && bytes[0] >= 2
				&& bytes[0] % 2 == 0 ? bytes[0] : 0;
		};
		const int languages = readString(0, 0, bytes);
		if (languages < 0) return ProbeResult::Retry;
		if (length(languages) < 4) return ProbeResult::NotOpd;
		const auto language = static_cast<uint16_t>(ReadUnsigned(bytes.data() + 2, 2));
		const int received = readString(index, language, bytes);
		if (received < 0) return ProbeResult::Retry;
		const int size = length(received);
		std::array<wchar_t, 126> signature{};
		for (int i = 2; i < size; i += 2) {
			if (bytes[i + 1] != 0) return ProbeResult::NotOpd;
			signature[(i - 2) / 2] = bytes[i];
		}
		return size > 2 && SupportsVersion(std::wstring_view(signature.data(), (size - 2) / 2))
			? ProbeResult::Found : ProbeResult::NotOpd;
	}
}
