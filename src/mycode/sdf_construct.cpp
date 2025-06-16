#define VMA_IMPLEMENTATION
#include "nvvk/descriptorsets_vk.hpp"               // Descriptor set helper
#include "nvvkhl/alloc_vma.hpp"                     // Our allocator
#include "nvvkhl/application.hpp"                   // For Application and IAppElememt
#include "nvvkhl/gbuffer.hpp"                       // G-Buffer helper
#include "nvvkhl/shaders/dh_comp.h"                 // Workgroup size and count
#include "nvvkhl/element_benchmark_parameters.hpp"  // For testing

#include "../vk_context.hpp"
#include "nvvk/extensions_vk.hpp"

