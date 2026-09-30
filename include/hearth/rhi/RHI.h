#pragma once

// The RHI umbrella. `te::rhi` (the `te` alias already exists). The API
// surface is backend-neutral; the Vulkan implementation lives under
// Platform/Vulkan/RHI/ so the exit criterion "no Vk* outside
// Platform/Vulkan/" stays literally true.

#include "TinyEngine/RHI/Types.h"
#include "TinyEngine/RHI/States.h"
#include "TinyEngine/RHI/Format.h"
#include "TinyEngine/RHI/BarrierDesc.h"
#include "TinyEngine/RHI/CommandList.h"
#include "TinyEngine/RHI/Device.h"
