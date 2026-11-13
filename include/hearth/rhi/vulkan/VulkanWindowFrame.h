#pragma once

#include <cstdint>

namespace TinyEngine {

	// The publish descriptor. The extract writes each target's slot-free
	// BUILD; at the frame boundary each window's SwapBuffers (game thread,
	// after the slot-free wait, before the enqueue) hands this to every
	// backend's PublishWindowFrame, which moves the builds of the targets
	// that window owns into Frames[Slot] and takes the boundary snapshots.
	// The publish is the only extract-side code that sees a slot; the
	// record reads Frames[list.FrameSlot()] as before.
	struct WindowFrame
	{
		uint32_t WindowIndex = 0; // the window's identity on the device (its slot-table index)
		uint32_t Slot = 0;        // the full frame slot this frame records into
		uint32_t SlotBase = 0;    // the window's first slot (Slot - the frame-in-flight index)
		bool IsMain = false;      // WindowProps::IsMainWindow (F-3)
	};

	// A registered framebuffer target's default owner: the main window,
	// resolved through WindowFrame::IsMain at publish (no main-context
	// lookup at registration). Never a window index.
	inline constexpr uint32_t kMainWindowOwner = UINT32_MAX;

}
