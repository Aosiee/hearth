#pragma once

#include <cstdint>

namespace hearth::rhi {

	// A registered framebuffer target's default owner: the main window,
	// resolved through WindowFrame::IsMain at publish and at record (no
	// main-context lookup at registration). Never a window index.
	inline constexpr uint32_t kMainWindowOwner = UINT32_MAX;

	// The publish descriptor. The program's renderers write each target's
	// slot-free BUILD; at the frame boundary the program's window frame
	// (loop thread, after the slot-free wait, before the enqueue) hands
	// this to each renderer's publish step, which moves the builds of the
	// targets that window owns into the slot's frame state and takes the
	// boundary snapshots. The publish is the only build-side code that
	// sees a slot; the record reads its slot from its command list
	// (CommandList::FrameSlot). The same descriptor, built by the window's
	// submit thread from the window's immutable identity, picks the
	// framebuffer targets that window's framebuffer-target graph records
	// - plain values, no thread affinity.
	struct WindowFrame
	{
		uint32_t WindowIndex = 0; // the window's identity on the device (its slot-table index)
		uint32_t Slot = 0;        // the full frame slot this frame records into
		uint32_t SlotBase = 0;    // the window's first slot (Slot - the frame-in-flight index)
		bool IsMain = false;      // VulkanWindowContext::IsMainWindow

		// The one ownership rule: the publish and the record both ask it,
		// so a target is published and recorded by the same window.
		bool Owns(uint32_t ownerWindow) const
		{
			return ownerWindow == kMainWindowOwner ? IsMain : ownerWindow == WindowIndex;
		}
	};

}
