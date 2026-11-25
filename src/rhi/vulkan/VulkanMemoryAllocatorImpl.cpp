// VMA is a header only library split into "declare everywhere, define once".
// Every other file in the Vulkan backend includes VulkanCommon.h normally and
// only ever sees declarations. This is the one translation unit compiled with
// VMA_IMPLEMENTATION defined, so the actual function bodies exist somewhere.
#define VMA_IMPLEMENTATION
#include "hearth/rhi/vulkan/VulkanCommon.h"
