#pragma once

// The RHI umbrella. `te::rhi` (the `te` alias already exists). The API
// surface is backend-neutral; the Vulkan implementation lives under
// Platform/Vulkan/RHI/ so the exit criterion "no backend identifiers
// outside Platform/Vulkan/" stays literally true.

#include "hearth/rhi/Types.h"
#include "hearth/rhi/States.h"
#include "hearth/rhi/Format.h"
#include "hearth/rhi/BarrierDesc.h"
#include "hearth/rhi/CommandList.h"
#include "hearth/rhi/Device.h"
