// license:GPLv3+
// Windows implementation of native input polling

#ifdef _WIN32

#include "NativeInput.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
	constexpr USAGE UsagePageGenericDesktop = 0x01;
	constexpr USAGE UsageJoystick = 0x04;
	constexpr USAGE UsageGamepad = 0x05;
	constexpr USAGE UsageMultiAxisController = 0x08;
	constexpr USAGE UsageAxisMin = 0x30;
	constexpr USAGE UsageAxisMax = 0x38;

	struct KeyBindingState {
		int keyCode;
		VpeInputAction action;
		float previousValue;
	};

	struct AxisDescriptor {
		int axisId;
		USAGE usagePage;
		USAGE usage;
		HIDP_VALUE_CAPS valueCaps;
		float previousValue;
		float rawValue;
		int64_t timestampUsec;
	};

	struct HidDevice {
		int deviceIndex;
		std::wstring path;
		std::string stableId;
		std::string displayName;
		HANDLE handle = INVALID_HANDLE_VALUE;
		PHIDP_PREPARSED_DATA preparsedData = nullptr;
		HIDP_CAPS caps = {};
		std::vector<AxisDescriptor> axes;
		std::vector<unsigned char> inputReport;
		OVERLAPPED readOverlap = {};
		HANDLE readEvent = NULL;
		bool readPending = false;
	};

	struct PendingAxisEvent {
		int deviceIndex;
		int axisId;
		float value;
	};

	std::thread g_pollingThread;
	std::atomic<bool> g_running(false);
	VpeInputEventCallback g_callback = nullptr;
	void* g_userData = nullptr;
	int g_pollIntervalUs = 500;
	HANDLE g_timer = NULL;
	HANDLE g_stopEvent = NULL;

	std::mutex g_bindingsMutex; // guards g_bindings and g_wasForeground between API calls and the polling thread
	std::vector<KeyBindingState> g_bindings;
	std::mutex g_hidDevicesMutex; // guards the live polling device snapshot and axis telemetry
	std::vector<HidDevice> g_hidDevices;

	LARGE_INTEGER g_frequency;
	LARGE_INTEGER g_startTime;
	bool g_wasForeground = false;

	int64_t GetTimestampUsecInternal() {
		if (g_frequency.QuadPart == 0) {
			return 0;
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return ((now.QuadPart - g_startTime.QuadPart) * 1000000LL) / g_frequency.QuadPart;
	}

	bool IsCurrentProcessForeground() {
		HWND foreground = GetForegroundWindow();
		if (foreground == NULL) {
			return false;
		}

		DWORD foregroundPid = 0;
		GetWindowThreadProcessId(foreground, &foregroundPid);
		return foregroundPid == GetCurrentProcessId();
	}

	std::string Narrow(const std::wstring& value) {
		if (value.empty()) {
			return {};
		}
		const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (size <= 1) {
			return {};
		}
		std::string result(static_cast<size_t>(size - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
		return result;
	}

	void CopyString(char* destination, int destinationSize, const std::string& value) {
		if (destination == nullptr || destinationSize <= 0) {
			return;
		}
		const size_t count = std::min(static_cast<size_t>(destinationSize - 1), value.size());
		if (count > 0) {
			memcpy(destination, value.data(), count);
		}
		destination[count] = '\0';
	}

	const char* AxisName(USAGE usage) {
		switch (usage) {
			case 0x30: return "X";
			case 0x31: return "Y";
			case 0x32: return "Z";
			case 0x33: return "RX";
			case 0x34: return "RY";
			case 0x35: return "RZ";
			case 0x36: return "Slider";
			case 0x37: return "Dial";
			case 0x38: return "Wheel";
			default: return "Axis";
		}
	}

	bool IsSupportedAxis(USAGE usage) {
		return usage >= UsageAxisMin && usage <= UsageAxisMax;
	}

	bool DeviceLooksLikeJoystick(const HIDP_CAPS& caps) {
		return caps.UsagePage == UsagePageGenericDesktop
			&& (caps.Usage == UsageJoystick || caps.Usage == UsageGamepad || caps.Usage == UsageMultiAxisController);
	}

	void EmitActionEvent(int64_t timestampUsec, VpeInputAction action, float value) {
		if (g_callback == nullptr) {
			return;
		}

		VpeInputEvent event{};
		event.timestampUsec = timestampUsec;
		event.eventType = VPE_INPUT_EVENT_ACTION;
		event.action = static_cast<int32_t>(action);
		event.value = value;
		g_callback(&event, g_userData);
	}

	void EmitAxisEvent(int64_t timestampUsec, int deviceIndex, int axisId, float value) {
		if (g_callback == nullptr) {
			return;
		}

		VpeInputEvent event{};
		event.timestampUsec = timestampUsec;
		event.eventType = VPE_INPUT_EVENT_AXIS;
		event.action = -1;
		event.deviceIndex = deviceIndex;
		event.axisId = axisId;
		event.value = value;
		g_callback(&event, g_userData);
	}

	void EmitReleaseForPressedKeys(int64_t timestampUsec) {
		for (auto& binding : g_bindings) {
			if (binding.previousValue <= 0.0f) {
				continue;
			}

			binding.previousValue = 0.0f;
			EmitActionEvent(timestampUsec, binding.action, 0.0f);
		}
	}

	void AddAxisDescriptors(HidDevice& device, const HIDP_VALUE_CAPS& valueCaps, int& axisId) {
		if (valueCaps.UsagePage != UsagePageGenericDesktop) {
			return;
		}

		if (valueCaps.IsRange) {
			for (USAGE usage = valueCaps.Range.UsageMin; usage <= valueCaps.Range.UsageMax; usage++) {
				if (!IsSupportedAxis(usage)) {
					continue;
				}
				device.axes.push_back({
					axisId++,
					valueCaps.UsagePage,
					usage,
					valueCaps,
					std::numeric_limits<float>::quiet_NaN(),
					0.0f,
					0,
				});
			}
		} else if (IsSupportedAxis(valueCaps.NotRange.Usage)) {
			device.axes.push_back({
				axisId++,
				valueCaps.UsagePage,
				valueCaps.NotRange.Usage,
				valueCaps,
				std::numeric_limits<float>::quiet_NaN(),
				0.0f,
				0,
			});
		}
	}

	// Returns NaN for a null-state reading (device reports "no valid data" for the axis).
	float NormalizeAxisValue(ULONG rawValue, const HIDP_VALUE_CAPS& valueCaps) {
		const LONG logicalMin = valueCaps.LogicalMin;
		const LONG logicalMax = valueCaps.LogicalMax;
		if (logicalMax == logicalMin) {
			return 0.0f;
		}

		// HidP_GetUsageValue returns the raw report bits in a ULONG without sign
		// extension; devices with a signed logical range (accelerometers such as
		// the Pinscape KL25Z) need it done manually from the report field width.
		LONG value;
		if (logicalMin < 0 && valueCaps.BitSize > 0 && valueCaps.BitSize < 32) {
			const ULONG signBit = 1UL << (valueCaps.BitSize - 1);
			const ULONG mask = (signBit << 1) - 1UL;
			const ULONG masked = rawValue & mask;
			value = (masked & signBit) ? static_cast<LONG>(masked | ~mask) : static_cast<LONG>(masked);
		} else {
			value = static_cast<LONG>(rawValue);
		}

		if (valueCaps.HasNull && (value < logicalMin || value > logicalMax)) {
			return std::numeric_limits<float>::quiet_NaN();
		}

		const float normalized = ((static_cast<float>(value) - static_cast<float>(logicalMin))
			/ (static_cast<float>(logicalMax) - static_cast<float>(logicalMin))) * 2.0f - 1.0f;
		return std::clamp(normalized, -1.0f, 1.0f);
	}

	void CloseHidDevice(HidDevice& device) {
		if (device.handle != INVALID_HANDLE_VALUE) {
			if (device.readPending) {
				CancelIoEx(device.handle, &device.readOverlap);
				// Cancellation is asynchronous and may be requested from a different
				// thread than the ReadFile issuer, so use CancelIoEx and then wait for
				// the IRP to stop touching the OVERLAPPED/report buffer.
				DWORD bytesRead = 0;
				GetOverlappedResult(device.handle, &device.readOverlap, &bytesRead, TRUE);
				device.readPending = false;
			}
			CloseHandle(device.handle);
			device.handle = INVALID_HANDLE_VALUE;
		}
		if (device.readEvent != NULL) {
			CloseHandle(device.readEvent);
			device.readEvent = NULL;
		}
		if (device.preparsedData != nullptr) {
			HidD_FreePreparsedData(device.preparsedData);
			device.preparsedData = nullptr;
		}
	}

	void CloseHidDevices(std::vector<HidDevice>& devices) {
		for (auto& device : devices) {
			CloseHidDevice(device);
		}
		devices.clear();
	}

	bool OpenHidDevice(const std::wstring& path, bool overlapped, HidDevice& device) {
		const DWORD flags = FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0);
		// Listing only needs to query HID metadata, which works with zero access
		// rights and doesn't clash with devices opened exclusively elsewhere.
		const DWORD desiredAccess = overlapped ? GENERIC_READ : 0;
		device.handle = CreateFileW(
			path.c_str(),
			desiredAccess,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			flags,
			NULL);
		if (device.handle == INVALID_HANDLE_VALUE) {
			return false;
		}

		device.path = path;
		device.stableId = Narrow(path);

		wchar_t productString[128] = {};
		if (HidD_GetProductString(device.handle, productString, sizeof(productString))) {
			device.displayName = Narrow(productString);
		}
		if (device.displayName.empty()) {
			HIDD_ATTRIBUTES attributes{};
			attributes.Size = sizeof(attributes);
			if (HidD_GetAttributes(device.handle, &attributes)) {
				char buffer[64] = {};
				snprintf(buffer, sizeof(buffer), "HID %04X:%04X", attributes.VendorID, attributes.ProductID);
				device.displayName = buffer;
			} else {
				device.displayName = "HID joystick";
			}
		}

		if (!HidD_GetPreparsedData(device.handle, &device.preparsedData)) {
			CloseHidDevice(device);
			return false;
		}

		if (HidP_GetCaps(device.preparsedData, &device.caps) != HIDP_STATUS_SUCCESS) {
			CloseHidDevice(device);
			return false;
		}

		std::vector<HIDP_VALUE_CAPS> valueCaps(device.caps.NumberInputValueCaps);
		USHORT valueCapsLength = device.caps.NumberInputValueCaps;
		if (valueCapsLength > 0 && HidP_GetValueCaps(HidP_Input, valueCaps.data(), &valueCapsLength, device.preparsedData) == HIDP_STATUS_SUCCESS) {
			int axisId = 0;
			for (USHORT i = 0; i < valueCapsLength; i++) {
				AddAxisDescriptors(device, valueCaps[i], axisId);
			}
		}

		if (!DeviceLooksLikeJoystick(device.caps)) {
			CloseHidDevice(device);
			return false;
		}
		if (device.axes.empty() || device.caps.InputReportByteLength == 0) {
			CloseHidDevice(device);
			return false;
		}

		device.inputReport.resize(device.caps.InputReportByteLength);
		if (overlapped) {
			device.readEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
			if (device.readEvent == NULL) {
				CloseHidDevice(device);
				return false;
			}
			device.readOverlap.hEvent = device.readEvent;
		}
		return true;
	}

	void EnumerateHidDevices(std::vector<HidDevice>& devices, bool openForPolling) {
		CloseHidDevices(devices);

		GUID hidGuid;
		HidD_GetHidGuid(&hidGuid);
		HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if (deviceInfoSet == INVALID_HANDLE_VALUE) {
			return;
		}

		for (DWORD index = 0;; index++) {
			SP_DEVICE_INTERFACE_DATA interfaceData{};
			interfaceData.cbSize = sizeof(interfaceData);
			if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &hidGuid, index, &interfaceData)) {
				break;
			}

			DWORD requiredSize = 0;
			SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, nullptr, 0, &requiredSize, nullptr);
			if (requiredSize == 0) {
				continue;
			}

			std::vector<unsigned char> detailBuffer(requiredSize);
			auto* detailData = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
			detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
			if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, detailData, requiredSize, nullptr, nullptr)) {
				continue;
			}

			HidDevice device{};
			if (!OpenHidDevice(detailData->DevicePath, openForPolling, device)) {
				continue;
			}
			device.deviceIndex = static_cast<int>(devices.size());
			devices.push_back(std::move(device));
		}

		SetupDiDestroyDeviceInfoList(deviceInfoSet);
	}

	void CopyDeviceInfo(const HidDevice& source, VpeInputDeviceInfo& destination) {
		destination = {};
		destination.deviceIndex = source.deviceIndex;
		destination.axisCount = static_cast<int32_t>(source.axes.size());
		destination.isConnected = 1;
		CopyString(destination.stableId, VPE_INPUT_DEVICE_ID_SIZE, source.stableId);
		CopyString(destination.displayName, VPE_INPUT_DEVICE_NAME_SIZE, source.displayName);
	}

	void CopyAxisInfo(const AxisDescriptor& source, VpeInputAxisInfo& destination) {
		destination = {};
		destination.axisId = source.axisId;
		destination.usagePage = source.usagePage;
		destination.usage = source.usage;
		destination.kind = VPE_AXIS_KIND_POSITION;
		destination.rawValue = source.rawValue;
		destination.timestampUsec = source.timestampUsec;
		CopyString(destination.name, VPE_INPUT_AXIS_NAME_SIZE, AxisName(source.usage));
	}

	void ParseInputReport(HidDevice& device, int64_t timestampUsec, std::vector<PendingAxisEvent>& pendingEvents) {
		if (device.preparsedData == nullptr || device.inputReport.empty()) {
			return;
		}

		auto* report = reinterpret_cast<PCHAR>(device.inputReport.data());
		const ULONG reportLength = static_cast<ULONG>(device.inputReport.size());
		for (auto& axis : device.axes) {
			ULONG value = 0;
			const NTSTATUS status = HidP_GetUsageValue(
				HidP_Input,
				axis.usagePage,
				axis.valueCaps.LinkCollection,
				axis.usage,
				&value,
				device.preparsedData,
				report,
				reportLength);
			if (status != HIDP_STATUS_SUCCESS) {
				continue;
			}

			const float normalized = NormalizeAxisValue(value, axis.valueCaps);
			if (std::isnan(normalized)) {
				continue;
			}
			axis.rawValue = normalized;
			axis.timestampUsec = timestampUsec;
			if (std::isnan(axis.previousValue) || std::fabs(normalized - axis.previousValue) > 0.0001f) {
				axis.previousValue = normalized;
				pendingEvents.push_back({ device.deviceIndex, axis.axisId, normalized });
			}
		}
	}

	void StartHidRead(HidDevice& device, int64_t timestampUsec, std::vector<PendingAxisEvent>& pendingEvents) {
		if (device.handle == INVALID_HANDLE_VALUE || device.readPending || device.inputReport.empty()) {
			return;
		}

		ResetEvent(device.readEvent);
		DWORD bytesRead = 0;
		const BOOL ok = ReadFile(
			device.handle,
			device.inputReport.data(),
			static_cast<DWORD>(device.inputReport.size()),
			&bytesRead,
			&device.readOverlap);
		if (ok) {
			ParseInputReport(device, timestampUsec, pendingEvents);
			return;
		}

		const DWORD error = GetLastError();
		if (error == ERROR_IO_PENDING) {
			device.readPending = true;
		}
	}

	void CompleteHidRead(HidDevice& device, int64_t timestampUsec, std::vector<PendingAxisEvent>& pendingEvents) {
		if (!device.readPending || device.readEvent == NULL) {
			return;
		}
		if (WaitForSingleObject(device.readEvent, 0) != WAIT_OBJECT_0) {
			return;
		}

		DWORD bytesRead = 0;
		const BOOL ok = GetOverlappedResult(device.handle, &device.readOverlap, &bytesRead, FALSE);
		device.readPending = false;
		if (ok && bytesRead > 0) {
			ParseInputReport(device, timestampUsec, pendingEvents);
		}
	}

	void PollHidDevices(int64_t timestampUsec, std::vector<PendingAxisEvent>& pendingEvents) {
		pendingEvents.clear();
		{
			std::lock_guard<std::mutex> lock(g_hidDevicesMutex);
			for (auto& device : g_hidDevices) {
				CompleteHidRead(device, timestampUsec, pendingEvents);
				StartHidRead(device, timestampUsec, pendingEvents);
			}
		}

		for (const auto& event : pendingEvents) {
			EmitAxisEvent(timestampUsec, event.deviceIndex, event.axisId, event.value);
		}
	}

	void PollingThreadFunc() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

		const HANDLE handles[2] = { g_timer, g_stopEvent };
		std::vector<PendingAxisEvent> pendingAxisEvents;
		while (g_running.load(std::memory_order_acquire)) {
			const int64_t timestampUsec = GetTimestampUsecInternal();
			const bool isForeground = IsCurrentProcessForeground();
			if (!isForeground) {
				std::lock_guard<std::mutex> lock(g_bindingsMutex);
				if (g_wasForeground) {
					EmitReleaseForPressedKeys(timestampUsec);
				}
				g_wasForeground = false;
			} else {
				{
					std::lock_guard<std::mutex> lock(g_bindingsMutex);
					g_wasForeground = true;

					for (auto& binding : g_bindings) {
						SHORT keyState = GetAsyncKeyState(binding.keyCode);
						bool isPressed = (keyState & 0x8000) != 0;
						float currentValue = isPressed ? 1.0f : 0.0f;

						if (currentValue != binding.previousValue) {
							binding.previousValue = currentValue;
							EmitActionEvent(timestampUsec, binding.action, currentValue);
						}
					}
				}

				PollHidDevices(timestampUsec, pendingAxisEvents);
			}

			if (g_pollIntervalUs > 0 && g_timer != NULL) {
				LARGE_INTEGER dueTime;
				dueTime.QuadPart = -(static_cast<LONGLONG>(g_pollIntervalUs) * 10);
				SetWaitableTimer(g_timer, &dueTime, 0, NULL, NULL, FALSE);
				DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
				if (waitResult == WAIT_OBJECT_0 + 1) {
					break;
				}
			} else {
				DWORD waitResult = WaitForSingleObject(g_stopEvent, 1);
				if (waitResult == WAIT_OBJECT_0) {
					break;
				}
			}
		}
	}
}

extern "C" {

VPE_API int VpeInputInit(void) {
	// Only establish the timestamp epoch once; a re-init while a consumer is
	// live must not make timestamps jump backwards.
	if (g_frequency.QuadPart == 0) {
		if (!QueryPerformanceFrequency(&g_frequency)) {
			return 0;
		}
		QueryPerformanceCounter(&g_startTime);
	}
	if (g_timer == NULL) {
		g_timer = CreateWaitableTimer(NULL, FALSE, NULL);
	}
	if (g_stopEvent == NULL) {
		g_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	}
	return 1;
}

VPE_API int VpeInputGetProtocolVersion(void) {
	return VPE_INPUT_PROTOCOL_VERSION;
}

VPE_API void VpeInputShutdown(void) {
	VpeInputStopPolling();
	{
		std::lock_guard<std::mutex> lock(g_bindingsMutex);
		g_bindings.clear();
	}
	{
		std::lock_guard<std::mutex> lock(g_hidDevicesMutex);
		CloseHidDevices(g_hidDevices);
	}
	if (g_timer != NULL) {
		CloseHandle(g_timer);
		g_timer = NULL;
	}
	if (g_stopEvent != NULL) {
		CloseHandle(g_stopEvent);
		g_stopEvent = NULL;
	}
}

VPE_API void VpeInputSetBindings(const VpeInputBinding* bindings, int count) {
	std::lock_guard<std::mutex> lock(g_bindingsMutex);
	g_bindings.clear();
	g_wasForeground = false;

	for (int i = 0; i < count; i++) {
		if (bindings[i].bindingType == VPE_BINDING_KEYBOARD) {
			g_bindings.push_back({
				bindings[i].keyCode,
				static_cast<VpeInputAction>(bindings[i].action),
				0.0f,
			});
		}
	}
}

VPE_API int VpeInputStartPolling(VpeInputEventCallback callback, void* userData, int pollIntervalUs) {
	if (g_running.load(std::memory_order_acquire)) {
		return 0;
	}

	g_callback = callback;
	g_userData = userData;
	g_pollIntervalUs = pollIntervalUs;
	g_wasForeground = false;
	if (g_stopEvent != NULL) {
		ResetEvent(g_stopEvent);
	}
	{
		std::lock_guard<std::mutex> lock(g_hidDevicesMutex);
		EnumerateHidDevices(g_hidDevices, true);
	}
	g_running.store(true, std::memory_order_release);

	try {
		g_pollingThread = std::thread(PollingThreadFunc);
		return 1;
	} catch (...) {
		g_running.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(g_hidDevicesMutex);
			CloseHidDevices(g_hidDevices);
		}
		return 0;
	}
}

VPE_API void VpeInputStopPolling(void) {
	if (g_running.load(std::memory_order_acquire)) {
		g_running.store(false, std::memory_order_release);
		if (g_stopEvent != NULL) {
			SetEvent(g_stopEvent);
		}
		if (g_pollingThread.joinable()) {
			g_pollingThread.join();
		}
	}
	{
		std::lock_guard<std::mutex> lock(g_hidDevicesMutex);
		CloseHidDevices(g_hidDevices);
	}
}

VPE_API int VpeInputListDevices(VpeInputDeviceInfo* devices, int maxDevices) {
	// While polling is active, serve the polling snapshot: it is the only set of
	// device indices consistent with the indices carried by axis events, and its
	// axis values are live. A fresh enumeration may order devices differently.
	if (g_running.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> lock(g_hidDevicesMutex);
		const int count = static_cast<int>(g_hidDevices.size());
		if (devices != nullptr && maxDevices > 0) {
			const int copyCount = std::min(count, maxDevices);
			for (int i = 0; i < copyCount; i++) {
				CopyDeviceInfo(g_hidDevices[i], devices[i]);
			}
		}
		return count;
	}

	std::vector<HidDevice> enumeratedDevices;
	EnumerateHidDevices(enumeratedDevices, false);
	const int count = static_cast<int>(enumeratedDevices.size());
	if (devices != nullptr && maxDevices > 0) {
		const int copyCount = std::min(count, maxDevices);
		for (int i = 0; i < copyCount; i++) {
			CopyDeviceInfo(enumeratedDevices[i], devices[i]);
		}
	}
	CloseHidDevices(enumeratedDevices);
	return count;
}

VPE_API int VpeInputListDeviceAxes(int deviceIndex, VpeInputAxisInfo* axes, int maxAxes) {
	if (g_running.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> lock(g_hidDevicesMutex);
		if (deviceIndex < 0 || deviceIndex >= static_cast<int>(g_hidDevices.size())) {
			return 0;
		}
		const auto& device = g_hidDevices[deviceIndex];
		const int count = static_cast<int>(device.axes.size());
		if (axes != nullptr && maxAxes > 0) {
			const int copyCount = std::min(count, maxAxes);
			for (int i = 0; i < copyCount; i++) {
				CopyAxisInfo(device.axes[i], axes[i]);
			}
		}
		return count;
	}

	std::vector<HidDevice> enumeratedDevices;
	EnumerateHidDevices(enumeratedDevices, false);
	if (deviceIndex < 0 || deviceIndex >= static_cast<int>(enumeratedDevices.size())) {
		CloseHidDevices(enumeratedDevices);
		return 0;
	}

	const auto& device = enumeratedDevices[deviceIndex];
	const int count = static_cast<int>(device.axes.size());
	if (axes != nullptr && maxAxes > 0) {
		const int copyCount = std::min(count, maxAxes);
		for (int i = 0; i < copyCount; i++) {
			CopyAxisInfo(device.axes[i], axes[i]);
		}
	}
	CloseHidDevices(enumeratedDevices);
	return count;
}

VPE_API int64_t VpeGetTimestampUsec(void) {
	return GetTimestampUsecInternal();
}

VPE_API void VpeSetThreadPriority(void) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

} // extern "C"

#endif // _WIN32
