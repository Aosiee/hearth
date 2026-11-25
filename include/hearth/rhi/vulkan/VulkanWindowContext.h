#pragma once

#include <cstdint>

namespace hearth::rhi {

	// A window the device renders into, as the device sees it. The
	// program's per-window frame derives from this and registers it with
	// the device (VulkanDevice::AllocateWindowSlot, SetMainContext).
	class VulkanWindowContext
	{
	public:
		// Declared by the program when it made the window; the device never
		// infers it from a slot or an order.
		virtual bool IsMainWindow() const = 0;
		// The frame slot the loop thread is building (the retire queue tags
		// each retired object with the main window's). Loop thread only.
		virtual uint32_t GetCurrentFrameSlot() const = 0;

	protected:
		~VulkanWindowContext() = default;
	};

} // namespace hearth::rhi
