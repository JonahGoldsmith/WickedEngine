#include "wiGraphicsDevice_Vulkan.h"

// pipeline state cache can be enabled here for Vulkan, but now it seems slower than not using it, so it's disabled by default:
//#define VULKAN_PIPELINE_CACHE_ENABLED

#ifdef WICKEDENGINE_BUILD_VULKAN
#include "wiVersion.h"

#if __has_include(<SDL3/SDL_filesystem.h>) && __has_include(<SDL3/SDL_iostream.h>) && __has_include(<SDL3/SDL_messagebox.h>) && __has_include(<SDL3/SDL_stdinc.h>) && __has_include(<SDL3/SDL_timer.h>)
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#endif

#include "Utility/spirv_reflect.h"
#include "Utility/vulkan/vk_video/vulkan_video_codec_h264std_decode.h"
#include "Utility/h264.h"
#include "wiMath.h"

#define VOLK_IMPLEMENTATION
#include "Utility/volk.h"

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "Utility/vk_mem_alloc.h"

#if defined(SDL3)
#include <SDL3/SDL_vulkan.h>
#elif defined(SDL2)
#include <SDL2/SDL_vulkan.h>
#include "sdl2.h"
#endif

#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <limits>
#include <vector>

#if defined(WICKED_MMGR_ENABLED)
// MMGR include must come after standard/project includes to avoid macro collisions in system headers.
#include "../forge-mmgr/FluidStudios/MemoryManager/mmgr.h"
#endif

#define VULKAN_LOG_ERROR(...) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define VULKAN_LOG(...) SDL_Log(__VA_ARGS__)
#define VULKAN_ASSERT_MSG(cond, ...) do { if (!(cond)) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__); SDL_assert(cond); } } while (0)

namespace wi
{

namespace
{
	inline void ShowVulkanErrorMessage(const std::string& message)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error!", message.c_str(), nullptr);
	}
	inline std::string ToUpperASCII(std::string value)
	{
		for (char& c : value)
		{
			c = (char)SDL_toupper((unsigned char)c);
		}
		return value;
	}
	inline std::string GetCurrentPathSDL()
	{
		char* cwd = SDL_GetCurrentDirectory();
		if (cwd == nullptr)
		{
			return ".";
		}
		std::string path = cwd;
		SDL_free(cwd);
		return path;
	}
	inline bool WriteFileBytesSDL(const std::string& path, const void* data, size_t size)
	{
		SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "wb");
		if (stream == nullptr)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file for write: %s", path.c_str());
			return false;
		}
		const size_t written = size > 0 ? SDL_WriteIO(stream, data, size) : 0;
		const bool closed = SDL_CloseIO(stream);
		return written == size && closed;
	}

	struct HostAllocationHeader
	{
		void* base = nullptr;
		size_t size = 0;
	};
	constexpr size_t GetHostAllocationMinAlignment()
	{
		return alignof(HostAllocationHeader) > sizeof(void*) ? alignof(HostAllocationHeader) : sizeof(void*);
	}
	constexpr size_t NormalizeHostAlignment(size_t alignment)
	{
		if (alignment < GetHostAllocationMinAlignment())
			return GetHostAllocationMinAlignment();
		return alignment;
	}
	inline void* HostRawAllocate(size_t size)
	{
#if defined(WICKED_MMGR_ENABLED)
		return mmgrAllocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_malloc, sizeof(void*), size);
#else
		return (malloc)(size);
#endif
	}
	inline void HostRawFree(void* ptr)
	{
		if (ptr == nullptr)
			return;
#if defined(WICKED_MMGR_ENABLED)
		mmgrDeallocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_free, ptr);
#else
		(free)(ptr);
#endif
	}
	inline HostAllocationHeader* GetHostAllocationHeader(void* memory)
	{
		return reinterpret_cast<HostAllocationHeader*>(reinterpret_cast<uint8_t*>(memory) - sizeof(HostAllocationHeader));
	}
	inline void* HostAlignedAllocate(size_t size, size_t alignment)
	{
		if (size == 0)
		{
			size = 1;
		}

		const size_t normalized_alignment = NormalizeHostAlignment(alignment);
		const size_t padding = (normalized_alignment - 1) + sizeof(HostAllocationHeader);
		if (size > (std::numeric_limits<size_t>::max)() - padding)
		{
			return nullptr;
		}

		void* raw_memory = HostRawAllocate(size + padding);
		if (raw_memory == nullptr)
		{
			return nullptr;
		}

		const uintptr_t raw_address = reinterpret_cast<uintptr_t>(raw_memory);
		const uintptr_t payload_base = raw_address + sizeof(HostAllocationHeader);
		const uintptr_t aligned_payload = align(payload_base, uintptr_t(normalized_alignment));
		auto* header = reinterpret_cast<HostAllocationHeader*>(aligned_payload - sizeof(HostAllocationHeader));
		header->base = raw_memory;
		header->size = size;
		return reinterpret_cast<void*>(aligned_payload);
	}
	inline void HostAlignedFree(void* memory)
	{
		if (memory == nullptr)
			return;

		HostAllocationHeader* header = GetHostAllocationHeader(memory);
		HostRawFree(header->base);
	}
	inline void* HostAlignedReallocate(void* original, size_t size, size_t alignment)
	{
		if (original == nullptr)
		{
			return HostAlignedAllocate(size, alignment);
		}
		if (size == 0)
		{
			HostAlignedFree(original);
			return nullptr;
		}

		HostAllocationHeader* old_header = GetHostAllocationHeader(original);
		void* replacement = HostAlignedAllocate(size, alignment);
		if (replacement == nullptr)
		{
			return nullptr;
		}

		std::memcpy(replacement, original, std::min(old_header->size, size));
		HostAlignedFree(original);
		return replacement;
	}
	static void* VMAAllocateCallback(void* /*pUserData*/, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return HostAlignedAllocate(size, alignment);
	}
	static void* VMAReallocateCallback(void* /*pUserData*/, void* pOriginal, size_t size, size_t alignment, VkSystemAllocationScope /*allocationScope*/)
	{
		return HostAlignedReallocate(pOriginal, size, alignment);
	}
	static void VMAFreeCallback(void* /*pUserData*/, void* pMemory)
	{
		HostAlignedFree(pMemory);
	}
	static void VMAInternalAllocationCallback(void* /*pUserData*/, size_t /*size*/, VkInternalAllocationType /*allocationType*/, VkSystemAllocationScope /*allocationScope*/)
	{
	}
	static void VMAInternalFreeCallback(void* /*pUserData*/, size_t /*size*/, VkInternalAllocationType /*allocationType*/, VkSystemAllocationScope /*allocationScope*/)
	{
	}
	static const VkAllocationCallbacks vma_allocation_callbacks = {
		nullptr,
		&VMAAllocateCallback,
		&VMAReallocateCallback,
		&VMAFreeCallback,
		&VMAInternalAllocationCallback,
		&VMAInternalFreeCallback,
	};

		// stb_ds array: transient texture subresource layout metadata for mapped uploads/readbacks.
		static void CreateTextureSubresourceDatasRaw(const TextureDesc& desc, void* data_ptr, SubresourceData*& subresource_datas, uint32_t alignment = 1)
	{
		const uint32_t bytes_per_block = GetFormatStride(desc.format);
		const uint32_t pixels_per_block = GetFormatBlockSize(desc.format);
		const uint32_t mips = GetMipCount(desc);
		arrsetlen(subresource_datas, GetTextureSubresourceCount(desc));
		size_t subresource_index = 0;
		size_t subresource_data_offset = 0;
		for (uint32_t layer = 0; layer < desc.array_size; ++layer)
		{
			for (uint32_t mip = 0; mip < mips; ++mip)
			{
				const uint32_t mip_width = std::max(1u, desc.width >> mip);
				const uint32_t mip_height = std::max(1u, desc.height >> mip);
				const uint32_t mip_depth = std::max(1u, desc.depth >> mip);
				const uint32_t num_blocks_x = (mip_width + pixels_per_block - 1) / pixels_per_block;
				const uint32_t num_blocks_y = (mip_height + pixels_per_block - 1) / pixels_per_block;
				SubresourceData& subresource_data = subresource_datas[subresource_index++];
				subresource_data.data_ptr = (uint8_t*)data_ptr + subresource_data_offset;
				subresource_data.row_pitch = align((uint32_t)num_blocks_x * bytes_per_block, alignment);
				subresource_data.slice_pitch = (uint32_t)subresource_data.row_pitch * num_blocks_y;
				subresource_data_offset += subresource_data.slice_pitch * mip_depth;
			}
		}
	}
}

namespace vulkan_internal
{
	static constexpr uint64_t timeout_value = 3'000'000'000ull; // 3 seconds
	static constexpr uint64_t dynamic_cbv_maxsize = 64 * 1024;

	// Converters:
	constexpr VkFormat _ConvertFormat(Format value)
	{
		switch (value)
		{
		case Format::UNKNOWN:
			return VK_FORMAT_UNDEFINED;
		case Format::R32G32B32A32_FLOAT:
			return VK_FORMAT_R32G32B32A32_SFLOAT;
		case Format::R32G32B32A32_UINT:
			return VK_FORMAT_R32G32B32A32_UINT;
		case Format::R32G32B32A32_SINT:
			return VK_FORMAT_R32G32B32A32_SINT;
		case Format::R32G32B32_FLOAT:
			return VK_FORMAT_R32G32B32_SFLOAT;
		case Format::R32G32B32_UINT:
			return VK_FORMAT_R32G32B32_UINT;
		case Format::R32G32B32_SINT:
			return VK_FORMAT_R32G32B32_SINT;
		case Format::R16G16B16A16_FLOAT:
			return VK_FORMAT_R16G16B16A16_SFLOAT;
		case Format::R16G16B16A16_UNORM:
			return VK_FORMAT_R16G16B16A16_UNORM;
		case Format::R16G16B16A16_UINT:
			return VK_FORMAT_R16G16B16A16_UINT;
		case Format::R16G16B16A16_SNORM:
			return VK_FORMAT_R16G16B16A16_SNORM;
		case Format::R16G16B16A16_SINT:
			return VK_FORMAT_R16G16B16A16_SINT;
		case Format::R32G32_FLOAT:
			return VK_FORMAT_R32G32_SFLOAT;
		case Format::R32G32_UINT:
			return VK_FORMAT_R32G32_UINT;
		case Format::R32G32_SINT:
			return VK_FORMAT_R32G32_SINT;
		case Format::D32_FLOAT_S8X24_UINT:
			return VK_FORMAT_D32_SFLOAT_S8_UINT;
		case Format::R10G10B10A2_UNORM:
			return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		case Format::R10G10B10A2_UINT:
			return VK_FORMAT_A2B10G10R10_UINT_PACK32;
		case Format::R11G11B10_FLOAT:
			return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
		case Format::R8G8B8A8_UNORM:
			return VK_FORMAT_R8G8B8A8_UNORM;
		case Format::R8G8B8A8_UNORM_SRGB:
			return VK_FORMAT_R8G8B8A8_SRGB;
		case Format::R8G8B8A8_UINT:
			return VK_FORMAT_R8G8B8A8_UINT;
		case Format::R8G8B8A8_SNORM:
			return VK_FORMAT_R8G8B8A8_SNORM;
		case Format::R8G8B8A8_SINT:
			return VK_FORMAT_R8G8B8A8_SINT;
		case Format::R16G16_FLOAT:
			return VK_FORMAT_R16G16_SFLOAT;
		case Format::R16G16_UNORM:
			return VK_FORMAT_R16G16_UNORM;
		case Format::R16G16_UINT:
			return VK_FORMAT_R16G16_UINT;
		case Format::R16G16_SNORM:
			return VK_FORMAT_R16G16_SNORM;
		case Format::R16G16_SINT:
			return VK_FORMAT_R16G16_SINT;
		case Format::D32_FLOAT:
			return VK_FORMAT_D32_SFLOAT;
		case Format::R32_FLOAT:
			return VK_FORMAT_R32_SFLOAT;
		case Format::R32_UINT:
			return VK_FORMAT_R32_UINT;
		case Format::R32_SINT:
			return VK_FORMAT_R32_SINT;
		case Format::D24_UNORM_S8_UINT:
			return VK_FORMAT_D24_UNORM_S8_UINT;
		case Format::R9G9B9E5_SHAREDEXP:
			return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
		case Format::R8G8_UNORM:
			return VK_FORMAT_R8G8_UNORM;
		case Format::R8G8_UINT:
			return VK_FORMAT_R8G8_UINT;
		case Format::R8G8_SNORM:
			return VK_FORMAT_R8G8_SNORM;
		case Format::R8G8_SINT:
			return VK_FORMAT_R8G8_SINT;
		case Format::R16_FLOAT:
			return VK_FORMAT_R16_SFLOAT;
		case Format::D16_UNORM:
			return VK_FORMAT_D16_UNORM;
		case Format::R16_UNORM:
			return VK_FORMAT_R16_UNORM;
		case Format::R16_UINT:
			return VK_FORMAT_R16_UINT;
		case Format::R16_SNORM:
			return VK_FORMAT_R16_SNORM;
		case Format::R16_SINT:
			return VK_FORMAT_R16_SINT;
		case Format::R8_UNORM:
			return VK_FORMAT_R8_UNORM;
		case Format::R8_UINT:
			return VK_FORMAT_R8_UINT;
		case Format::R8_SNORM:
			return VK_FORMAT_R8_SNORM;
		case Format::R8_SINT:
			return VK_FORMAT_R8_SINT;
		case Format::BC1_UNORM:
			return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		case Format::BC1_UNORM_SRGB:
			return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
		case Format::BC2_UNORM:
			return VK_FORMAT_BC2_UNORM_BLOCK;
		case Format::BC2_UNORM_SRGB:
			return VK_FORMAT_BC2_SRGB_BLOCK;
		case Format::BC3_UNORM:
			return VK_FORMAT_BC3_UNORM_BLOCK;
		case Format::BC3_UNORM_SRGB:
			return VK_FORMAT_BC3_SRGB_BLOCK;
		case Format::BC4_UNORM:
			return VK_FORMAT_BC4_UNORM_BLOCK;
		case Format::BC4_SNORM:
			return VK_FORMAT_BC4_SNORM_BLOCK;
		case Format::BC5_UNORM:
			return VK_FORMAT_BC5_UNORM_BLOCK;
		case Format::BC5_SNORM:
			return VK_FORMAT_BC5_SNORM_BLOCK;
		case Format::B8G8R8A8_UNORM:
			return VK_FORMAT_B8G8R8A8_UNORM;
		case Format::B8G8R8A8_UNORM_SRGB:
			return VK_FORMAT_B8G8R8A8_SRGB;
		case Format::BC6H_UF16:
			return VK_FORMAT_BC6H_UFLOAT_BLOCK;
		case Format::BC6H_SF16:
			return VK_FORMAT_BC6H_SFLOAT_BLOCK;
		case Format::BC7_UNORM:
			return VK_FORMAT_BC7_UNORM_BLOCK;
		case Format::BC7_UNORM_SRGB:
			return VK_FORMAT_BC7_SRGB_BLOCK;
		case Format::NV12:
			return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		}
		return VK_FORMAT_UNDEFINED;
	}
	constexpr VkCompareOp _ConvertComparisonFunc(ComparisonFunc value)
	{
		switch (value)
		{
		case ComparisonFunc::NEVER:
			return VK_COMPARE_OP_NEVER;
		case ComparisonFunc::LESS:
			return VK_COMPARE_OP_LESS;
		case ComparisonFunc::EQUAL:
			return VK_COMPARE_OP_EQUAL;
		case ComparisonFunc::LESS_EQUAL:
			return VK_COMPARE_OP_LESS_OR_EQUAL;
		case ComparisonFunc::GREATER:
			return VK_COMPARE_OP_GREATER;
		case ComparisonFunc::NOT_EQUAL:
			return VK_COMPARE_OP_NOT_EQUAL;
		case ComparisonFunc::GREATER_EQUAL:
			return VK_COMPARE_OP_GREATER_OR_EQUAL;
		case ComparisonFunc::ALWAYS:
			return VK_COMPARE_OP_ALWAYS;
		default:
			return VK_COMPARE_OP_NEVER;
		}
	}
	constexpr VkBlendFactor _ConvertBlend(Blend value)
	{
		switch (value)
		{
		case Blend::ZERO:
			return VK_BLEND_FACTOR_ZERO;
		case Blend::ONE:
			return VK_BLEND_FACTOR_ONE;
		case Blend::SRC_COLOR:
			return VK_BLEND_FACTOR_SRC_COLOR;
		case Blend::INV_SRC_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case Blend::SRC_ALPHA:
			return VK_BLEND_FACTOR_SRC_ALPHA;
		case Blend::INV_SRC_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case Blend::DEST_ALPHA:
			return VK_BLEND_FACTOR_DST_ALPHA;
		case Blend::INV_DEST_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case Blend::DEST_COLOR:
			return VK_BLEND_FACTOR_DST_COLOR;
		case Blend::INV_DEST_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case Blend::SRC_ALPHA_SAT:
			return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		case Blend::BLEND_FACTOR:
			return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case Blend::INV_BLEND_FACTOR:
			return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
			break;
		case Blend::SRC1_COLOR:
			return VK_BLEND_FACTOR_SRC1_COLOR;
		case Blend::INV_SRC1_COLOR:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		case Blend::SRC1_ALPHA:
			return VK_BLEND_FACTOR_SRC1_ALPHA;
		case Blend::INV_SRC1_ALPHA:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
		default:
			return VK_BLEND_FACTOR_ZERO;
		}
	}
	constexpr VkBlendOp _ConvertBlendOp(BlendOp value)
	{
		switch (value)
		{
		case BlendOp::ADD:
			return VK_BLEND_OP_ADD;
		case BlendOp::SUBTRACT:
			return VK_BLEND_OP_SUBTRACT;
		case BlendOp::REV_SUBTRACT:
			return VK_BLEND_OP_REVERSE_SUBTRACT;
		case BlendOp::MIN:
			return VK_BLEND_OP_MIN;
		case BlendOp::MAX:
			return VK_BLEND_OP_MAX;
		default:
			return VK_BLEND_OP_ADD;
		}
	}
	constexpr VkSamplerAddressMode _ConvertTextureAddressMode(TextureAddressMode value, const VkPhysicalDeviceVulkan12Features& features_1_2)
	{
		switch (value)
		{
		case TextureAddressMode::WRAP:
			return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		case TextureAddressMode::MIRROR:
			return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		case TextureAddressMode::CLAMP:
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		case TextureAddressMode::BORDER:
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		case TextureAddressMode::MIRROR_ONCE:
			if (features_1_2.samplerMirrorClampToEdge == VK_TRUE)
			{
				return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
			}
			return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		default:
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		}
	}
	constexpr VkBorderColor _ConvertSamplerBorderColor(SamplerBorderColor value)
	{
		switch (value)
		{
		case SamplerBorderColor::TRANSPARENT_BLACK:
			return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		case SamplerBorderColor::OPAQUE_BLACK:
			return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		case SamplerBorderColor::OPAQUE_WHITE:
			return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		default:
			return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		}
	}
	constexpr VkStencilOp _ConvertStencilOp(StencilOp value)
	{
		switch (value)
		{
		case wi::StencilOp::KEEP:
			return VK_STENCIL_OP_KEEP;
		case wi::StencilOp::STENCIL_ZERO:
			return VK_STENCIL_OP_ZERO;
		case wi::StencilOp::REPLACE:
			return VK_STENCIL_OP_REPLACE;
		case wi::StencilOp::INCR_SAT:
			return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
		case wi::StencilOp::DECR_SAT:
			return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
		case wi::StencilOp::INVERT:
			return VK_STENCIL_OP_INVERT;
		case wi::StencilOp::INCR:
			return VK_STENCIL_OP_INCREMENT_AND_WRAP;
		case wi::StencilOp::DECR:
			return VK_STENCIL_OP_DECREMENT_AND_WRAP;
		default:
			return VK_STENCIL_OP_KEEP;
		}
	}
	constexpr VkImageLayout _ConvertImageLayout(ResourceState value)
	{
		switch (value)
		{
		case ResourceState::UNDEFINED:
			return VK_IMAGE_LAYOUT_UNDEFINED;
		case ResourceState::RENDERTARGET:
			return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		case ResourceState::DEPTHSTENCIL:
			return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		case ResourceState::DEPTHSTENCIL_READONLY:
			return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		case ResourceState::SHADER_RESOURCE:
		case ResourceState::SHADER_RESOURCE_COMPUTE:
			return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		case ResourceState::UNORDERED_ACCESS:
			return VK_IMAGE_LAYOUT_GENERAL;
		case ResourceState::COPY_SRC:
		case ResourceState::COPY_DST:
			// we can't assume transfer layout because it's allowed for resource to be used by multiple queues like DX12 (decay to common state), so this is a workaround
			//	the problem is that image copy commands will require specifying the current layout, but different queues can often use textures in different layouts
			return VK_IMAGE_LAYOUT_GENERAL;
		case ResourceState::SHADING_RATE_SOURCE:
			return VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
		case ResourceState::VIDEO_DECODE_DPB:
			return VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
		case ResourceState::VIDEO_DECODE_SRC:
			return VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR;
		case ResourceState::VIDEO_DECODE_DST:
			return VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
		case ResourceState::SWAPCHAIN:
			return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		default:
			// combination of state flags will default to general
			//	whether the combination of states is valid needs to be validated by the user
			//	combining read-only states should be fine
			return VK_IMAGE_LAYOUT_GENERAL;
		}
	}
	constexpr VkShaderStageFlags _ConvertStageFlags(ShaderStage value)
	{
		switch (value)
		{
		case ShaderStage::MS:
			return VK_SHADER_STAGE_MESH_BIT_EXT;
		case ShaderStage::AS:
			return VK_SHADER_STAGE_TASK_BIT_EXT;
		case ShaderStage::VS:
			return VK_SHADER_STAGE_VERTEX_BIT;
		case ShaderStage::HS:
			return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		case ShaderStage::DS:
			return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
		case ShaderStage::GS:
			return VK_SHADER_STAGE_GEOMETRY_BIT;
		case ShaderStage::PS:
			return VK_SHADER_STAGE_FRAGMENT_BIT;
		case ShaderStage::CS:
			return VK_SHADER_STAGE_COMPUTE_BIT;
		default:
			return VK_SHADER_STAGE_ALL;
		}
	}
	constexpr VkImageAspectFlags _ConvertImageAspect(ImageAspect value)
	{
		switch (value)
		{
		default:
		case wi::ImageAspect::COLOR:
			return VK_IMAGE_ASPECT_COLOR_BIT;
		case wi::ImageAspect::DEPTH:
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		case wi::ImageAspect::STENCIL:
			return VK_IMAGE_ASPECT_STENCIL_BIT;
		case wi::ImageAspect::LUMINANCE:
			return VK_IMAGE_ASPECT_PLANE_0_BIT;
		case wi::ImageAspect::CHROMINANCE:
			return VK_IMAGE_ASPECT_PLANE_1_BIT;
		}
	}
	constexpr VkPipelineStageFlags2 _ConvertPipelineStage(ResourceState value)
	{
		VkPipelineStageFlags2 flags = VK_PIPELINE_STAGE_2_NONE;

		if (has_flag(value, ResourceState::SHADER_RESOURCE) ||
			has_flag(value, ResourceState::SHADER_RESOURCE_COMPUTE) ||
			has_flag(value, ResourceState::UNORDERED_ACCESS) ||
			has_flag(value, ResourceState::CONSTANT_BUFFER))
		{
			flags |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		}
		if (has_flag(value, ResourceState::COPY_SRC) ||
			has_flag(value, ResourceState::COPY_DST))
		{
			flags |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		}
		if (has_flag(value, ResourceState::RENDERTARGET))
		{
			flags |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		}
		if (has_flag(value, ResourceState::DEPTHSTENCIL) ||
			has_flag(value, ResourceState::DEPTHSTENCIL_READONLY))
		{
			flags |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		}
		if (has_flag(value, ResourceState::SHADING_RATE_SOURCE))
		{
			flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
		}
		if (has_flag(value, ResourceState::VERTEX_BUFFER))
		{
			flags |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
		}
		if (has_flag(value, ResourceState::INDEX_BUFFER))
		{
			flags |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
		}
		if (has_flag(value, ResourceState::INDIRECT_ARGUMENT))
		{
			flags |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		}
		if (has_flag(value, ResourceState::RAYTRACING_ACCELERATION_STRUCTURE))
		{
			flags |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
		}
		if (has_flag(value, ResourceState::PREDICATION))
		{
			flags |= VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
		}
		if (has_flag(value, ResourceState::VIDEO_DECODE_DST) ||
			has_flag(value, ResourceState::VIDEO_DECODE_SRC) ||
			has_flag(value, ResourceState::VIDEO_DECODE_DPB))
		{
			flags |= VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
		}

		return flags;
	}
	constexpr VkAccessFlags2 _ParseResourceState(ResourceState value)
	{
		VkAccessFlags2 flags = 0;

		if (has_flag(value, ResourceState::SHADER_RESOURCE))
		{
			flags |= VK_ACCESS_2_SHADER_READ_BIT;
		}
		if (has_flag(value, ResourceState::SHADER_RESOURCE_COMPUTE))
		{
			flags |= VK_ACCESS_2_SHADER_READ_BIT;
		}
		if (has_flag(value, ResourceState::UNORDERED_ACCESS))
		{
			flags |= VK_ACCESS_2_SHADER_READ_BIT;
			flags |= VK_ACCESS_2_SHADER_WRITE_BIT;
		}
		if (has_flag(value, ResourceState::COPY_SRC))
		{
			flags |= VK_ACCESS_2_TRANSFER_READ_BIT;
		}
		if (has_flag(value, ResourceState::COPY_DST))
		{
			flags |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
		}
		if (has_flag(value, ResourceState::RENDERTARGET))
		{
			flags |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
			flags |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		}
		if (has_flag(value, ResourceState::DEPTHSTENCIL))
		{
			flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		}
		if (has_flag(value, ResourceState::DEPTHSTENCIL_READONLY))
		{
			flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		}
		if (has_flag(value, ResourceState::VERTEX_BUFFER))
		{
			flags |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
		}
		if (has_flag(value, ResourceState::INDEX_BUFFER))
		{
			flags |= VK_ACCESS_2_INDEX_READ_BIT;
		}
		if (has_flag(value, ResourceState::CONSTANT_BUFFER))
		{
			flags |= VK_ACCESS_2_UNIFORM_READ_BIT;
		}
		if (has_flag(value, ResourceState::INDIRECT_ARGUMENT))
		{
			flags |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		}
		if (has_flag(value, ResourceState::PREDICATION))
		{
			flags |= VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
		}
		if (has_flag(value, ResourceState::SHADING_RATE_SOURCE))
		{
			flags |= VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
		}
		if (has_flag(value, ResourceState::VIDEO_DECODE_DST))
		{
			flags |= VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
		}
		if (has_flag(value, ResourceState::VIDEO_DECODE_SRC))
		{
			flags |= VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR;
		}
		if (has_flag(value, ResourceState::VIDEO_DECODE_DPB))
		{
			flags |= VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
		}

		return flags;
	}
	constexpr VkComponentSwizzle _ConvertComponentSwizzle(ComponentSwizzle value)
	{
		switch (value)
		{
		default:
			return VK_COMPONENT_SWIZZLE_IDENTITY;
		case wi::ComponentSwizzle::R:
			return VK_COMPONENT_SWIZZLE_R;
		case wi::ComponentSwizzle::G:
			return VK_COMPONENT_SWIZZLE_G;
		case wi::ComponentSwizzle::B:
			return VK_COMPONENT_SWIZZLE_B;
		case wi::ComponentSwizzle::A:
			return VK_COMPONENT_SWIZZLE_A;
		case wi::ComponentSwizzle::SWIZZLE_ZERO:
			return VK_COMPONENT_SWIZZLE_ZERO;
		case wi::ComponentSwizzle::SWIZZLE_ONE:
			return VK_COMPONENT_SWIZZLE_ONE;
		}
	}
	constexpr VkComponentMapping _ConvertSwizzle(Swizzle value)
	{
		VkComponentMapping mapping = {};
		mapping.r = _ConvertComponentSwizzle(value.r);
		mapping.g = _ConvertComponentSwizzle(value.g);
		mapping.b = _ConvertComponentSwizzle(value.b);
		mapping.a = _ConvertComponentSwizzle(value.a);
		return mapping;
	}


	bool checkExtensionSupport(const char* checkExtension, const VkExtensionProperties* available_extensions, uint32_t available_extension_count)
	{
		for (uint32_t i = 0; i < available_extension_count; ++i)
		{
			const auto& x = available_extensions[i];
			if (strcmp(x.extensionName, checkExtension) == 0)
			{
				return true;
			}
		}
		return false;
	}

	bool appendUniqueName(const char* name, const char*** inout_names)
	{
		if (name == nullptr || inout_names == nullptr)
		{
			return false;
		}

		const char** names = *inout_names;
		for (uint32_t i = 0; i < arrlenu(names); ++i)
		{
			if (strcmp(names[i], name) == 0)
			{
				return true;
			}
		}

		arrput(names, name);
		*inout_names = names;
		return true;
	}

	bool tryEnableExtension(const char* name, bool required, const VkExtensionProperties* available_extensions, uint32_t available_extension_count, const char*** inout_enabled_extensions, bool* out_enabled = nullptr)
	{
		const bool available = checkExtensionSupport(name, available_extensions, available_extension_count);
		if (out_enabled != nullptr)
		{
			*out_enabled = false;
		}

		if (!available)
		{
			return !required;
		}

		if (!appendUniqueName(name, inout_enabled_extensions))
		{
			return false;
		}

		if (out_enabled != nullptr)
		{
			*out_enabled = true;
		}
		return true;
	}

	bool ValidateLayers(const std::deque<const char*>& required,
		const VkLayerProperties* available, uint32_t available_count)
	{
		for (auto layer : required)
		{
			bool found = false;
			for (uint32_t i = 0; i < available_count; ++i)
			{
				auto& available_layer = available[i];
				if (strcmp(available_layer.layerName, layer) == 0)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				return false;
			}
		}

		return true;
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsMessengerCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
		VkDebugUtilsMessageTypeFlagsEXT message_type,
		const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
		void* user_data)
	{
		(void)message_type;
		(void)user_data;
		// Log debug message
		std::string ss;

		if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		{
			ss += "[Vulkan Warning]: ";
			ss += callback_data->pMessage;
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", ss.c_str());
		}
		else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		{
			ss += "[Vulkan Error]: ";
			ss += callback_data->pMessage;
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", ss.c_str());
		}

		return VK_FALSE;
	}

	inline std::string get_shader_cache_path()
	{
		return GetCurrentPathSDL() + "/pso_cache_vulkan";
	}

	struct BindingUsage
	{
		bool used = false;
		VkDescriptorSetLayoutBinding binding = {};
	};
	struct Buffer_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VmaAllocation allocation = nullptr;
		VkBuffer resource = VK_NULL_HANDLE;
		struct BufferSubresource
		{
			VkBufferView buffer_view = VK_NULL_HANDLE;
			VkDescriptorBufferInfo buffer_info = {};
			int index = -1; // bindless

			constexpr bool IsValid() const
			{
				return index >= 0;
			}
		};
		BufferSubresource srv;
		BufferSubresource uav;
		std::deque<BufferSubresource> subresources_srv;
		std::deque<BufferSubresource> subresources_uav;
		VkDeviceAddress address = 0;

		void destroy_subresources()
		{
			if (srv.IsValid())
			{
				if (srv.buffer_view != VK_NULL_HANDLE)
				{
					allocationhandler->Retire(allocationhandler->destroyer_bufferviews, srv.buffer_view);
					allocationhandler->Retire(allocationhandler->destroyer_bindlessUniformTexelBuffers, srv.index);
				}
				else
				{
					allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageBuffers, srv.index);
				}
				srv = {};
			}
			if (uav.IsValid())
			{
				if (uav.buffer_view != VK_NULL_HANDLE)
				{
					allocationhandler->Retire(allocationhandler->destroyer_bufferviews, uav.buffer_view);
					allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageTexelBuffers, uav.index);
				}
				else
				{
					allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageBuffers, uav.index);
				}
				uav = {};
			}
			for (auto& x : subresources_srv)
			{
				if (x.buffer_view != VK_NULL_HANDLE)
				{
					allocationhandler->Retire(allocationhandler->destroyer_bufferviews, x.buffer_view);
					allocationhandler->Retire(allocationhandler->destroyer_bindlessUniformTexelBuffers, x.index);
				}
				else
				{
					allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageBuffers, x.index);
				}
			}
			subresources_srv.clear();
			for (auto& x : subresources_uav)
			{
				if (x.buffer_view != VK_NULL_HANDLE)
				{
					allocationhandler->Retire(allocationhandler->destroyer_bufferviews, x.buffer_view);
					allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageTexelBuffers, x.index);
				}
				else
				{
					allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageBuffers, x.index);
				}
			}
			subresources_uav.clear();
		}

		~Buffer_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (resource)
			{
				allocationhandler->Retire(allocationhandler->destroyer_buffers, std::make_pair(resource, allocation));
			}
			else if(allocation)
			{
				allocationhandler->Retire(allocationhandler->destroyer_allocations, allocation);
			}
			destroy_subresources();
			allocationhandler->destroylocker.unlock();
		}
	};
	struct Texture_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VmaAllocation allocation = nullptr;
		VkImage resource = VK_NULL_HANDLE;
		VkImageLayout defaultLayout = VK_IMAGE_LAYOUT_GENERAL;
		VkBuffer staging_resource = VK_NULL_HANDLE;
		struct TextureSubresource
		{
			VkImageView image_view = VK_NULL_HANDLE;
			int index = -1; // bindless
			uint32_t firstMip = 0;
			uint32_t mipCount = 0;
			uint32_t firstSlice = 0;
			uint32_t sliceCount = 0;

			constexpr bool IsValid() const
			{
				return image_view != VK_NULL_HANDLE;
			}
		};
		TextureSubresource srv;
		TextureSubresource uav;
		TextureSubresource rtv;
		TextureSubresource dsv;
		uint32_t framebuffer_layercount = 0;
		std::deque<TextureSubresource> subresources_srv;
		std::deque<TextureSubresource> subresources_uav;
		std::deque<TextureSubresource> subresources_rtv;
		std::deque<TextureSubresource> subresources_dsv;

		SubresourceData* mapped_subresources = nullptr; // stb_ds array: mapped texture subresource layout metadata.
		SparseTextureProperties sparse_texture_properties;

		VkImageView video_decode_view = VK_NULL_HANDLE;

		void destroy_subresources()
		{
			if (srv.IsValid())
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, srv.image_view);
				allocationhandler->Retire(allocationhandler->destroyer_bindlessSampledImages, srv.index);
				srv = {};
			}
			if (uav.IsValid())
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, uav.image_view);
				allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageImages, uav.index);
				uav = {};
			}
			if (rtv.IsValid())
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, rtv.image_view);
				rtv = {};
			}
			if (dsv.IsValid())
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, dsv.image_view);
				dsv = {};
			}
			for (auto x : subresources_srv)
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, x.image_view);
				allocationhandler->Retire(allocationhandler->destroyer_bindlessSampledImages, x.index);
			}
			subresources_srv.clear();
			for (auto x : subresources_uav)
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, x.image_view);
				allocationhandler->Retire(allocationhandler->destroyer_bindlessStorageImages, x.index);
			}
			subresources_uav.clear();
			for (auto x : subresources_rtv)
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, x.image_view);
			}
			subresources_rtv.clear();
			for (auto x : subresources_dsv)
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, x.image_view);
			}
			subresources_dsv.clear();
		}

		~Texture_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (resource)
			{
				allocationhandler->Retire(allocationhandler->destroyer_images, std::make_pair(resource, allocation));
			}
			else if (staging_resource)
			{
				allocationhandler->Retire(allocationhandler->destroyer_buffers, std::make_pair(staging_resource, allocation));
			}
			else if (allocation)
			{
				allocationhandler->Retire(allocationhandler->destroyer_allocations, allocation);
			}
			if (video_decode_view != VK_NULL_HANDLE)
			{
				allocationhandler->Retire(allocationhandler->destroyer_imageviews, video_decode_view);
			}
			arrfree(mapped_subresources);
			mapped_subresources = nullptr;
			destroy_subresources();
			allocationhandler->destroylocker.unlock();
		}
	};
	struct Sampler_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VkSampler resource = VK_NULL_HANDLE;
		int index = -1;

		~Sampler_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (resource) allocationhandler->Retire(allocationhandler->destroyer_samplers, resource);
			if (index >= 0) allocationhandler->Retire(allocationhandler->destroyer_bindlessSamplers, index);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct QueryHeap_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VkQueryPool pool = VK_NULL_HANDLE;

		~QueryHeap_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (pool) allocationhandler->Retire(allocationhandler->destroyer_querypools, pool);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct Shader_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VkShaderModule shaderModule = VK_NULL_HANDLE;
		VkPipeline pipeline_cs = VK_NULL_HANDLE;
		VkPipelineShaderStageCreateInfo stageInfo = {};
		GraphicsDevice_Vulkan::PSOLayout layout;
		std::string entrypoint;

		~Shader_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (shaderModule) allocationhandler->Retire(allocationhandler->destroyer_shadermodules, shaderModule);
			if (pipeline_cs) allocationhandler->Retire(allocationhandler->destroyer_pipelines, pipeline_cs);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct PipelineState_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VkPipeline pipeline = VK_NULL_HANDLE;
		GraphicsDevice_Vulkan::PSOLayout layout;

		~PipelineState_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (pipeline) allocationhandler->Retire(allocationhandler->destroyer_pipelines, pipeline);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct BVH_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VmaAllocation allocation = nullptr;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkAccelerationStructureKHR resource = VK_NULL_HANDLE;
		int index = -1;

		VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
		VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
		VkAccelerationStructureCreateInfoKHR createInfo = {};
		VkAccelerationStructureGeometryKHR* geometries = nullptr;
		uint32_t* primitiveCounts = nullptr;
		VkDeviceAddress scratch_address = 0;
		VkDeviceAddress as_address = 0;

		~BVH_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (buffer) allocationhandler->Retire(allocationhandler->destroyer_buffers, std::make_pair(buffer, allocation));
			if (resource) allocationhandler->Retire(allocationhandler->destroyer_bvhs, resource);
			if (index >= 0) allocationhandler->Retire(allocationhandler->destroyer_bindlessAccelerationStructures, index);
			allocationhandler->destroylocker.unlock();
			arrfree(geometries);
			arrfree(primitiveCounts);
		}
	};
	struct RTPipelineState_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VkPipeline pipeline = VK_NULL_HANDLE;

		~RTPipelineState_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			if (pipeline) allocationhandler->Retire(allocationhandler->destroyer_pipelines, pipeline);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct SwapChain_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VkSwapchainKHR swapChain = VK_NULL_HANDLE;
		VkFormat swapChainImageFormat;
		VkExtent2D swapChainExtent;
		std::deque<wi::allocator::shared_ptr<Texture_Vulkan>> textures; // shared_ptr is used because they can be given out by GetBackBuffer()

		Texture dummyTexture;

		VkSurfaceKHR surface = VK_NULL_HANDLE;

		uint32_t swapChainImageIndex = 0;
		uint32_t swapChainAcquireSemaphoreIndex = 0;
		std::deque<VkSemaphore> swapchainAcquireSemaphores;
		std::deque<VkSemaphore> swapchainReleaseSemaphores;
		bool explicit_acquire_pending = false;

		ColorSpace colorSpace = ColorSpace::SRGB;
		SwapChainDesc desc;
		std::mutex locker;

		~SwapChain_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();

			for (size_t i = 0; i < swapchainAcquireSemaphores.size(); ++i)
			{
				allocationhandler->Retire(allocationhandler->destroyer_semaphores, swapchainAcquireSemaphores[i]);
				allocationhandler->Retire(allocationhandler->destroyer_semaphores, swapchainReleaseSemaphores[i]);
			}

#if defined(SDL3) || defined(SDL2)
			// Checks if the SDL VIDEO System was already destroyed.
			// If so we would delete the swapchain twice, causing a crash on wayland.
			if (SDL_WasInit(SDL_INIT_VIDEO))
#endif
			{
				allocationhandler->Retire(allocationhandler->destroyer_swapchains, swapChain);
				allocationhandler->Retire(allocationhandler->destroyer_surfaces, surface);
			}

			allocationhandler->destroylocker.unlock();

		}
	};
	struct VideoDecoder_Vulkan
	{
		wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler> allocationhandler;
		VkVideoSessionKHR video_session = VK_NULL_HANDLE;
		VkVideoSessionParametersKHR session_parameters = VK_NULL_HANDLE;
		VmaAllocation* allocations = nullptr;

		~VideoDecoder_Vulkan()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_video_sessions, video_session);
			allocationhandler->Retire(allocationhandler->destroyer_video_session_parameters, session_parameters);
			for (uint32_t i = 0; i < arrlenu(allocations); ++i)
			{
				allocationhandler->Retire(allocationhandler->destroyer_allocations, allocations[i]);
			}
			allocationhandler->destroylocker.unlock();
			arrfree(allocations);
		}
	};

	template<typename T> struct VulkanType;
	template<> struct VulkanType<GPUBuffer> { using type = Buffer_Vulkan; };
	template<> struct VulkanType<Texture> { using type = Texture_Vulkan; };
	template<> struct VulkanType<Sampler> { using type = Sampler_Vulkan; };
	template<> struct VulkanType<GPUQueryHeap> { using type = QueryHeap_Vulkan; };
	template<> struct VulkanType<Shader> { using type = Shader_Vulkan; };
	template<> struct VulkanType<RaytracingAccelerationStructure> { using type = BVH_Vulkan; };
	template<> struct VulkanType<PipelineState> { using type = PipelineState_Vulkan; };
	template<> struct VulkanType<RaytracingPipelineState> { using type = RTPipelineState_Vulkan; };
	template<> struct VulkanType<SwapChain> { using type = SwapChain_Vulkan; };
	template<> struct VulkanType<VideoDecoder> { using type = VideoDecoder_Vulkan; };


	template<typename T>
	typename VulkanType<T>::type* to_internal(const T* param)
	{
		return static_cast<typename VulkanType<T>::type*>(param->internal_state.get());
	}

	template<typename T>
	typename VulkanType<T>::type* to_internal(const GPUResource* res)
	{
		return static_cast<typename VulkanType<T>::type*>(res->internal_state.get());
	}

	bool CreateSwapChainInternal(
		SwapChain_Vulkan* internal_state,
		VkPhysicalDevice physicalDevice,
		VkDevice device,
		const wi::allocator::shared_ptr<GraphicsDevice_Vulkan::AllocationHandler>& allocationhandler
	)
	{
		// In vulkan, the swapchain recreate can happen whenever it gets outdated, it's not in application's control
		//	so we have to be extra careful
		std::scoped_lock lock(internal_state->locker);

		VkSurfaceCapabilitiesKHR swapchain_capabilities;
		vulkan_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, internal_state->surface, &swapchain_capabilities));

		uint32_t formatCount;
		vulkan_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, internal_state->surface, &formatCount, nullptr));

		VkSurfaceFormatKHR* swapchain_formats = nullptr;
		arrsetlen(swapchain_formats, formatCount);
		vulkan_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, internal_state->surface, &formatCount, swapchain_formats));

		uint32_t presentModeCount;
		vulkan_check(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, internal_state->surface, &presentModeCount, nullptr));

		VkPresentModeKHR* swapchain_presentModes = nullptr;
		arrsetlen(swapchain_presentModes, presentModeCount);
		vulkan_check(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, internal_state->surface, &presentModeCount, swapchain_presentModes));

		VkSurfaceFormatKHR surfaceFormat = {};
		surfaceFormat.format = _ConvertFormat(internal_state->desc.format);
		surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		bool valid = false;

		for (uint32_t i = 0; i < formatCount; ++i)
		{
			const auto& format = swapchain_formats[i];
			if (!internal_state->desc.allow_hdr && format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				continue;
			if (format.format == surfaceFormat.format)
			{
				surfaceFormat = format;
				valid = true;
				break;
			}
		}
		if (!valid)
		{
			internal_state->desc.format = Format::B8G8R8A8_UNORM;
			surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
			surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		}

		// For now, we only include the color spaces that were tested successfully:
		ColorSpace prev_colorspace = internal_state->colorSpace;
		switch (surfaceFormat.colorSpace)
		{
		default:
		case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
			internal_state->colorSpace = ColorSpace::SRGB;
			break;
		case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
			internal_state->colorSpace = ColorSpace::HDR_LINEAR;
			break;
		case VK_COLOR_SPACE_HDR10_ST2084_EXT:
			internal_state->colorSpace = ColorSpace::HDR10_ST2084;
			break;
		}

		if (prev_colorspace != internal_state->colorSpace)
		{
			if (internal_state->swapChain != VK_NULL_HANDLE)
			{
				// For some reason, if the swapchain gets recreated (via oldSwapChain) with different color space but same image format,
				//	the color space change will not be applied
				vulkan_check(vkDeviceWaitIdle(device));
				vkDestroySwapchainKHR(device, internal_state->swapChain, nullptr);
				internal_state->swapChain = nullptr;
			}
		}

			if (
				swapchain_capabilities.currentExtent.width != 0xFFFFFFFF &&
				swapchain_capabilities.currentExtent.height != 0xFFFFFFFF &&
				swapchain_capabilities.currentExtent.width > 0 &&
				swapchain_capabilities.currentExtent.height > 0)
			{
				internal_state->swapChainExtent = swapchain_capabilities.currentExtent;
			}
			else
			{
				internal_state->swapChainExtent = { internal_state->desc.width, internal_state->desc.height };
				internal_state->swapChainExtent.width = std::max(swapchain_capabilities.minImageExtent.width, std::min(swapchain_capabilities.maxImageExtent.width, internal_state->swapChainExtent.width));
				internal_state->swapChainExtent.height = std::max(swapchain_capabilities.minImageExtent.height, std::min(swapchain_capabilities.maxImageExtent.height, internal_state->swapChainExtent.height));

				// Some portability paths (notably MoltenVK + freshly created windows) can report 0x0 extents transiently.
				// Keep swapchain creation robust by forcing a minimal non-zero extent in that case.
				if (internal_state->swapChainExtent.width == 0 || internal_state->swapChainExtent.height == 0)
				{
					internal_state->swapChainExtent.width = std::max(1u, internal_state->desc.width);
					internal_state->swapChainExtent.height = std::max(1u, internal_state->desc.height);
					SDL_LogWarn(
						SDL_LOG_CATEGORY_APPLICATION,
						"Vulkan surface reported zero extent, falling back to %ux%u",
						internal_state->swapChainExtent.width,
						internal_state->swapChainExtent.height
					);
				}
			}

		uint32_t imageCount = std::max(internal_state->desc.buffer_count, swapchain_capabilities.minImageCount);
		if ((swapchain_capabilities.maxImageCount > 0) && (imageCount > swapchain_capabilities.maxImageCount))
		{
			imageCount = swapchain_capabilities.maxImageCount;
		}

		VkSwapchainCreateInfoKHR createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = internal_state->surface;
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = internal_state->swapChainExtent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.preTransform = swapchain_capabilities.currentTransform;

		createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // The only one that is always supported
		if (!internal_state->desc.vsync)
		{
			// The mailbox/immediate present mode is not necessarily supported:
			for (uint32_t i = 0; i < presentModeCount; ++i)
			{
				auto& presentMode = swapchain_presentModes[i];
				if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
				{
					createInfo.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
					break;
				}
				if (presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
				{
					createInfo.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
				}
			}
		}
		createInfo.clipped = VK_TRUE;
		createInfo.oldSwapchain = internal_state->swapChain;

		vulkan_check(vkCreateSwapchainKHR(device, &createInfo, nullptr, &internal_state->swapChain));

		if (createInfo.oldSwapchain != VK_NULL_HANDLE)
		{
			std::scoped_lock lock(allocationhandler->destroylocker);
			allocationhandler->Retire(allocationhandler->destroyer_swapchains, createInfo.oldSwapchain);
		}

		vulkan_check(vkGetSwapchainImagesKHR(device, internal_state->swapChain, &imageCount, nullptr));
		VkImage swapChainImages[32] = {};
		SDL_assert(SDL_arraysize(swapChainImages) >= imageCount);
		vulkan_check(vkGetSwapchainImagesKHR(device, internal_state->swapChain, &imageCount, swapChainImages));
		internal_state->swapChainImageFormat = surfaceFormat.format;

		// Create swap chain render targets:
		internal_state->textures.resize(imageCount);
		for (uint32_t i = 0; i < imageCount; ++i)
		{
			VkImageViewCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			createInfo.image = swapChainImages[i];
			createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			createInfo.format = internal_state->swapChainImageFormat;
			createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			createInfo.subresourceRange.baseMipLevel = 0;
			createInfo.subresourceRange.levelCount = 1;
			createInfo.subresourceRange.baseArrayLayer = 0;
			createInfo.subresourceRange.layerCount = 1;

			if (internal_state->textures[i].IsValid())
			{
				allocationhandler->destroylocker.lock();
				internal_state->textures[i]->rtv = {};
				internal_state->textures[i]->srv = {};
				allocationhandler->destroylocker.unlock();
			}
			else
			{
				internal_state->textures[i] = wi::allocator::make_shared<Texture_Vulkan>();
			}
			internal_state->textures[i]->resource = swapChainImages[i];
			internal_state->textures[i]->defaultLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vulkan_check(vkCreateImageView(device, &createInfo, nullptr, &internal_state->textures[i]->rtv.image_view));
			vulkan_check(vkCreateImageView(device, &createInfo, nullptr, &internal_state->textures[i]->srv.image_view));
		}

		VkSemaphoreCreateInfo semaphoreInfo = {};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		// safety release of current swapchain semaphores that might still be working, since this could have been called mid-frame:
		allocationhandler->destroylocker.lock();
		for (auto& x : internal_state->swapchainAcquireSemaphores)
		{
			allocationhandler->Retire(allocationhandler->destroyer_semaphores, x);
		}
		internal_state->swapchainAcquireSemaphores.clear();
		for (auto& x : internal_state->swapchainReleaseSemaphores)
		{
			allocationhandler->Retire(allocationhandler->destroyer_semaphores, x);
		}
		internal_state->swapchainReleaseSemaphores.clear();
		allocationhandler->destroylocker.unlock();

		internal_state->swapChainAcquireSemaphoreIndex = 0;
		internal_state->swapChainImageIndex = 0;
		internal_state->explicit_acquire_pending = false;

		if (internal_state->swapchainAcquireSemaphores.empty())
		{
			for (size_t i = 0; i < internal_state->textures.size(); ++i)
			{
				vulkan_check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &internal_state->swapchainAcquireSemaphores.emplace_back()));
			}
		}

		if (internal_state->swapchainReleaseSemaphores.empty())
		{
			for (size_t i = 0; i < internal_state->textures.size(); ++i)
			{
				vulkan_check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &internal_state->swapchainReleaseSemaphores.emplace_back()));
			}
		}

		return true;
	}
}
using namespace vulkan_internal;

	void GraphicsDevice_Vulkan::set_fence_name(VkFence fence, const char* name)
	{
		if (!debugUtils)
			return;
		if (fence == VK_NULL_HANDLE)
			return;

		VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
		info.pObjectName = name;
		info.objectType = VK_OBJECT_TYPE_FENCE;
		info.objectHandle = (uint64_t)fence;

		vulkan_check(vkSetDebugUtilsObjectNameEXT(device, &info));
	}
	void GraphicsDevice_Vulkan::set_semaphore_name(VkSemaphore semaphore, const char* name)
	{
		if (!debugUtils)
			return;
		if (semaphore == VK_NULL_HANDLE)
			return;

		VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
		info.pObjectName = name;
		info.objectType = VK_OBJECT_TYPE_SEMAPHORE;
		info.objectHandle = (uint64_t)semaphore;

		vulkan_check(vkSetDebugUtilsObjectNameEXT(device, &info));
	}

	VkPipelineLayout GraphicsDevice_Vulkan::cache_pso_layout(PSOLayout& layout) const
	{
		PSOLayoutHash layout_hash;
		for (int i = 0; i < SDL_arraysize(layout.table.SRV); ++i)
		{
			layout_hash.SRV[i] = layout.table.SRV[i].descriptorType;
		}
		for (int i = 0; i < SDL_arraysize(layout.table.UAV); ++i)
		{
			layout_hash.UAV[i] = layout.table.UAV[i].descriptorType;
		}
		layout_hash.embed_hash();

		std::scoped_lock lck(layout_locker);
		for (auto& item : pso_layouts)
		{
			if (item.first == layout_hash)
			{
				layout = item.second;
				return layout.pipeline_layout;
			}
		}

		VkDescriptorSetLayout descriptor_set_layouts[DESCRIPTOR_SET_COUNT] = {};

		VkDescriptorSetLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = sizeof(layout.table) / sizeof(VkDescriptorSetLayoutBinding);
		layoutInfo.pBindings = (const VkDescriptorSetLayoutBinding*)&layout.table;

		VkDescriptorBindingFlags bindingFlags[sizeof(layout.table) / sizeof(VkDescriptorSetLayoutBinding)] = {};
		for (auto& x : bindingFlags)
		{
			x =
				VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | // shuts up warning about format mismatches
				VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT; // descriptor sets will be reused after they are no longer used by GPU
		}
		VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo = {};
		bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		bindingFlagsInfo.bindingCount = layoutInfo.bindingCount;
		bindingFlagsInfo.pBindingFlags = bindingFlags;
		layoutInfo.pNext = &bindingFlagsInfo;

		vulkan_check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout.descriptor_set_layout));

		descriptor_set_layouts[DESCRIPTOR_SET_BINDINGS] = layout.descriptor_set_layout;
		descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_SAMPLER] = allocationhandler->bindlessSamplers.descriptorSetLayout;
		descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_STORAGE_BUFFER] = allocationhandler->bindlessStorageBuffers.descriptorSetLayout;
		descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_UNIFORM_TEXEL_BUFFER] = allocationhandler->bindlessUniformTexelBuffers.descriptorSetLayout;
		descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_SAMPLED_IMAGE] = allocationhandler->bindlessSampledImages.descriptorSetLayout;
		descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_STORAGE_IMAGE] = allocationhandler->bindlessStorageImages.descriptorSetLayout;
		descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_STORAGE_TEXEL_BUFFER] = allocationhandler->bindlessStorageTexelBuffers.descriptorSetLayout;
		if (CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		{
			descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_ACCELERATION_STRUCTURE] = allocationhandler->bindlessAccelerationStructures.descriptorSetLayout;
		}
		else
		{
			// Unused, set dummy sampler set:
			descriptor_set_layouts[DESCRIPTOR_SET_BINDLESS_ACCELERATION_STRUCTURE] = allocationhandler->bindlessSamplers.descriptorSetLayout;
		}

		VkPushConstantRange range = {};
		range.size = sizeof(uint32_t) * PUSH_CONSTANT_COUNT;
		range.stageFlags = VK_SHADER_STAGE_ALL;

		VkPipelineLayoutCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		info.pPushConstantRanges = &range;
		info.pushConstantRangeCount = 1;
		info.pSetLayouts = descriptor_set_layouts;
		info.setLayoutCount = SDL_arraysize(descriptor_set_layouts);
		vulkan_check(vkCreatePipelineLayout(device, &info, nullptr, &layout.pipeline_layout));

		static uint32_t next_layout_id = 0;
		layout.id = next_layout_id++;

		pso_layouts.emplace_back(layout_hash, layout);

		return layout.pipeline_layout;
	}

	void GraphicsDevice_Vulkan::CommandQueue::clear()
	{
		submitted_in_current_submit = false;
		swapchain_updates.clear();
		arrfree(submit_waitSemaphoreInfos);
		arrfree(submit_signalSemaphoreInfos);
		arrfree(submit_cmds);
		submit_waitSemaphoreInfos = nullptr;
		submit_signalSemaphoreInfos = nullptr;
		submit_cmds = nullptr;

		arrfree(swapchainWaitSemaphores);
		arrfree(swapchains);
		arrfree(swapchainImageIndices);
		swapchainWaitSemaphores = nullptr;
		swapchains = nullptr;
		swapchainImageIndices = nullptr;
	}
	void GraphicsDevice_Vulkan::CommandQueue::signal(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags2 stageMask)
	{
		if (queue == VK_NULL_HANDLE)
			return;
		VkSemaphoreSubmitInfo signalSemaphore = {};
		signalSemaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		signalSemaphore.semaphore = semaphore;
		signalSemaphore.value = value;
		signalSemaphore.stageMask = stageMask;
		arrput(submit_signalSemaphoreInfos, signalSemaphore);
	}
	void GraphicsDevice_Vulkan::CommandQueue::wait(VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags2 stageMask)
	{
		if (queue == VK_NULL_HANDLE)
			return;
		VkSemaphoreSubmitInfo waitSemaphore = {};
		waitSemaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		waitSemaphore.semaphore = semaphore;
		waitSemaphore.value = value;
		waitSemaphore.stageMask = stageMask;
		arrput(submit_waitSemaphoreInfos, waitSemaphore);
	}
	bool GraphicsDevice_Vulkan::CommandQueue::submit(
		GraphicsDevice_Vulkan* device,
		VkFence fence,
		bool include_frame_sync,
		uint64_t timeline_signal_value,
		uint64_t* out_timeline_signal_value,
		bool process_presents
	)
	{
		if (out_timeline_signal_value != nullptr)
		{
			*out_timeline_signal_value = 0;
		}
		if (queue == VK_NULL_HANDLE)
			return false;
		std::scoped_lock lock(*locker);
		bool did_submit = false;

		// Main submit with command lists and semaphores:
		{
				if (fence != VK_NULL_HANDLE && include_frame_sync)
				{
					// end of frame mark:
					for (int q = 0; q < QUEUE_COUNT; ++q)
					{
					if (frame_semaphores[device->GetBufferIndex()][q] == VK_NULL_HANDLE)
						continue;
						signal(frame_semaphores[device->GetBufferIndex()][q]);
					}
				}
				const bool has_submit_payload =
					fence != VK_NULL_HANDLE ||
					arrlenu(submit_cmds) > 0 ||
					arrlenu(submit_waitSemaphoreInfos) > 0 ||
					arrlenu(submit_signalSemaphoreInfos) > 0;
				if (timeline_signal_value == 0 && out_timeline_signal_value != nullptr && timeline_semaphore != VK_NULL_HANDLE && has_submit_payload)
				{
					timeline_signal_value = timeline_value.fetch_add(1, std::memory_order_relaxed) + 1;
				}
				if (timeline_signal_value != 0 && timeline_semaphore != VK_NULL_HANDLE)
				{
					signal(timeline_semaphore, timeline_signal_value);
				}
				if (out_timeline_signal_value != nullptr)
				{
					*out_timeline_signal_value = timeline_signal_value;
				}

				did_submit =
					fence != VK_NULL_HANDLE ||
					arrlenu(submit_cmds) > 0 ||
				arrlenu(submit_waitSemaphoreInfos) > 0 ||
				arrlenu(submit_signalSemaphoreInfos) > 0;

			if (did_submit)
			{
				VkSubmitInfo2 submitInfo = {};
				submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
				submitInfo.commandBufferInfoCount = (uint32_t)arrlenu(submit_cmds);
				submitInfo.pCommandBufferInfos = submit_cmds;

				submitInfo.waitSemaphoreInfoCount = (uint32_t)arrlenu(submit_waitSemaphoreInfos);
				submitInfo.pWaitSemaphoreInfos = submit_waitSemaphoreInfos;

				submitInfo.signalSemaphoreInfoCount = (uint32_t)arrlenu(submit_signalSemaphoreInfos);
				submitInfo.pSignalSemaphoreInfos = submit_signalSemaphoreInfos;

				vulkan_check(vkQueueSubmit2(queue, 1, &submitInfo, fence));
				submitted_in_current_submit = true;
			}

			arrfree(submit_waitSemaphoreInfos);
			arrfree(submit_signalSemaphoreInfos);
			arrfree(submit_cmds);
			submit_waitSemaphoreInfos = nullptr;
			submit_signalSemaphoreInfos = nullptr;
			submit_cmds = nullptr;
		}

		// Swapchain presents:
		if (process_presents && arrlenu(swapchains) > 0)
		{
			VkPresentInfoKHR presentInfo = {};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = (uint32_t)arrlenu(swapchainWaitSemaphores);
			presentInfo.pWaitSemaphores = swapchainWaitSemaphores;
			presentInfo.swapchainCount = (uint32_t)arrlenu(swapchains);
			presentInfo.pSwapchains = swapchains;
			presentInfo.pImageIndices = swapchainImageIndices;
			VkResult res = vkQueuePresentKHR(queue, &presentInfo);
			if (res != VK_SUCCESS)
			{
				// Handle outdated error in present:
				if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
				{
						for (auto& swapchain : swapchain_updates)
						{
							auto internal_state = to_internal(&swapchain);
							bool success = CreateSwapChainInternal(internal_state, device->physicalDevice, device->device, device->allocationhandler);
							SDL_assert(success);
						}
				}
				else
				{
					vulkan_assert(false, "vkQueuePresentKHR");
				}
			}

			swapchain_updates.clear();
			arrfree(swapchains);
			arrfree(swapchainImageIndices);
			arrfree(swapchainWaitSemaphores);
			swapchains = nullptr;
			swapchainImageIndices = nullptr;
			swapchainWaitSemaphores = nullptr;
		}
		else if (process_presents)
		{
			swapchain_updates.clear();
		}

		return did_submit;
	}

	void GraphicsDevice_Vulkan::CopyAllocator::init(GraphicsDevice_Vulkan* device)
	{
		this->device = device;
	}
	void GraphicsDevice_Vulkan::CopyAllocator::destroy()
	{
		vkQueueWaitIdle(device->queue_init.queue);
		recycle_completed();
		for (auto& x : inflight)
		{
			vkDestroyCommandPool(device->device, x.transferCommandPool, nullptr);
			vkDestroyFence(device->device, x.fence, nullptr);
		}
		for (auto& x : freelist)
		{
			vkDestroyCommandPool(device->device, x.transferCommandPool, nullptr);
			vkDestroyFence(device->device, x.fence, nullptr);
		}
	}
	void GraphicsDevice_Vulkan::CopyAllocator::recycle_completed()
	{
		std::scoped_lock lock(locker);
		for (size_t i = 0; i < inflight.size();)
		{
			if (vkGetFenceStatus(device->device, inflight[i].fence) == VK_SUCCESS)
			{
				if (inflight[i].submitted_queue < QUEUE_COUNT && inflight[i].submitted_value > 0)
				{
					auto& completed_counter = device->queue_timeline_completed_fallback[inflight[i].submitted_queue];
					uint64_t completed = completed_counter.load(std::memory_order_relaxed);
					while (completed < inflight[i].submitted_value &&
						!completed_counter.compare_exchange_weak(completed, inflight[i].submitted_value, std::memory_order_release, std::memory_order_relaxed))
					{
					}
				}
				inflight[i].submitted_queue = QUEUE_COUNT;
				inflight[i].submitted_value = 0;
				freelist.push_back(std::move(inflight[i]));
				std::swap(inflight[i], inflight.back());
				inflight.pop_back();
				continue;
			}
			++i;
		}
	}
	bool GraphicsDevice_Vulkan::CopyAllocator::is_point_complete(QueueSyncPoint point)
	{
		if (!point.IsValid() || point.queue >= QUEUE_COUNT)
			return true;

		recycle_completed();
		const uint64_t completed = device->queue_timeline_completed_fallback[point.queue].load(std::memory_order_acquire);
		if (completed >= point.value)
			return true;

		std::scoped_lock lock(locker);
		for (size_t i = 0; i < inflight.size(); ++i)
		{
			const CopyCMD& cmd = inflight[i];
			if (cmd.submitted_queue != point.queue || cmd.submitted_value != point.value)
				continue;
			return vkGetFenceStatus(device->device, cmd.fence) == VK_SUCCESS;
		}
		return false;
	}
	bool GraphicsDevice_Vulkan::CopyAllocator::wait_point(QueueSyncPoint point)
	{
		if (!point.IsValid() || point.queue >= QUEUE_COUNT)
			return true;

		recycle_completed();
		if (device->queue_timeline_completed_fallback[point.queue].load(std::memory_order_acquire) >= point.value)
			return true;

		VkFence fence = VK_NULL_HANDLE;
		{
			std::scoped_lock lock(locker);
			for (size_t i = 0; i < inflight.size(); ++i)
			{
				const CopyCMD& cmd = inflight[i];
				if (cmd.submitted_queue == point.queue && cmd.submitted_value == point.value)
				{
					fence = cmd.fence;
					break;
				}
			}
		}
		if (fence == VK_NULL_HANDLE)
			return false;

		while (vulkan_check(vkWaitForFences(device->device, 1, &fence, VK_TRUE, timeout_value)) == VK_TIMEOUT)
		{
			VULKAN_LOG_ERROR("[CopyAllocator::wait_point] vkWaitForFences resulted in VK_TIMEOUT");
			std::this_thread::yield();
		}
		recycle_completed();
		return device->queue_timeline_completed_fallback[point.queue].load(std::memory_order_acquire) >= point.value;
	}
	GraphicsDevice_Vulkan::CopyAllocator::CopyCMD GraphicsDevice_Vulkan::CopyAllocator::allocate(uint64_t staging_size, QUEUE_TYPE queue)
	{
		CopyCMD cmd;
		QUEUE_TYPE recording_queue = queue < QUEUE_COUNT ? queue : QUEUE_COPY;
		if (device->queues[recording_queue].queue == VK_NULL_HANDLE)
		{
			recording_queue = device->queues[QUEUE_COPY].queue != VK_NULL_HANDLE ? QUEUE_COPY : QUEUE_GRAPHICS;
		}
		uint32_t recording_family = device->graphicsFamily;
		switch (recording_queue)
		{
		case QUEUE_GRAPHICS:
			recording_family = device->graphicsFamily;
			break;
		case QUEUE_COMPUTE:
			recording_family = device->computeFamily;
			break;
		case QUEUE_COPY:
			recording_family = device->copyFamily;
			break;
		case QUEUE_VIDEO_DECODE:
			recording_family = device->videoFamily;
			break;
		default:
			break;
		}

		recycle_completed();

		locker.lock();
		// Try to search for a staging buffer that can fit the request:
		for (size_t i = 0; i < freelist.size(); ++i)
		{
			if (freelist[i].uploadbuffer.desc.size >= staging_size && freelist[i].recording_queue == recording_queue)
			{
				cmd = std::move(freelist[i]);
				std::swap(freelist[i], freelist.back());
				freelist.pop_back();
				break;
			}
		}
		locker.unlock();

		// If no buffer was found that fits the data, create one:
		if (!cmd.IsValid())
		{
			VkCommandPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
			poolInfo.queueFamilyIndex = recording_family;
			vulkan_check(vkCreateCommandPool(device->device, &poolInfo, nullptr, &cmd.transferCommandPool));

			VkCommandBufferAllocateInfo commandBufferInfo = {};
			commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			commandBufferInfo.commandBufferCount = 1;
			commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			commandBufferInfo.commandPool = cmd.transferCommandPool;
			vulkan_check(vkAllocateCommandBuffers(device->device, &commandBufferInfo, &cmd.transferCommandBuffer));

			VkFenceCreateInfo fenceInfo = {};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			vulkan_check(vkCreateFence(device->device, &fenceInfo, nullptr, &cmd.fence));
			device->set_fence_name(cmd.fence, "CopyAllocator::fence");

			GPUBufferDesc uploaddesc;
			uploaddesc.size = wi::math::GetNextPowerOfTwo(staging_size);
			uploaddesc.size = std::max(uploaddesc.size, uint64_t(65536));
			uploaddesc.usage = Usage::UPLOAD;
			bool upload_success = device->CreateBuffer(&uploaddesc, nullptr, &cmd.uploadbuffer);
			SDL_assert(upload_success);
			device->SetName(&cmd.uploadbuffer, "CopyAllocator::uploadBuffer");
		}
		cmd.recording_queue = recording_queue;
		cmd.submitted_queue = QUEUE_COUNT;
		cmd.submitted_value = 0;

		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		beginInfo.pInheritanceInfo = nullptr;

		vulkan_check(vkResetCommandPool(device->device, cmd.transferCommandPool, 0));
		vulkan_check(vkBeginCommandBuffer(cmd.transferCommandBuffer, &beginInfo));

		vulkan_check(vkResetFences(device->device, 1, &cmd.fence));

		return cmd;
	}
	SubmissionToken GraphicsDevice_Vulkan::CopyAllocator::submit(CopyCMD cmd, QUEUE_TYPE queue)
	{
		SubmissionToken token = {};
		VkSubmitInfo2 submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

		VkCommandBufferSubmitInfo cbSubmitInfo = {};
		cbSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;

		QUEUE_TYPE dst_queue_type = queue < QUEUE_COUNT ? queue : QUEUE_COPY;
		if (device->queues[dst_queue_type].queue == VK_NULL_HANDLE)
		{
			dst_queue_type = device->queues[QUEUE_COPY].queue != VK_NULL_HANDLE ? QUEUE_COPY : QUEUE_GRAPHICS;
		}
		if (cmd.recording_queue < QUEUE_COUNT)
		{
			dst_queue_type = cmd.recording_queue;
		}
		CommandQueue& dst_queue = device->queues[dst_queue_type];

		{
			vulkan_check(vkEndCommandBuffer(cmd.transferCommandBuffer));
			cbSubmitInfo.commandBuffer = cmd.transferCommandBuffer;
			submitInfo.commandBufferInfoCount = 1;
			submitInfo.pCommandBufferInfos = &cbSubmitInfo;

			uint64_t submitted_value = 0;
			VkSemaphoreSubmitInfo signalSemaphoreInfo = {};
			std::scoped_lock lock(*dst_queue.locker);
			if (device->SupportsSubmissionTokens() && dst_queue.timeline_semaphore != VK_NULL_HANDLE)
			{
				signalSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
				signalSemaphoreInfo.semaphore = dst_queue.timeline_semaphore;
				signalSemaphoreInfo.value = dst_queue.timeline_value.fetch_add(1, std::memory_order_relaxed) + 1;
				signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				submitInfo.signalSemaphoreInfoCount = 1;
				submitInfo.pSignalSemaphoreInfos = &signalSemaphoreInfo;
				submitted_value = signalSemaphoreInfo.value;
			}
			else
			{
				submitted_value = device->queue_timeline_submitted_fallback[dst_queue_type].fetch_add(1, std::memory_order_relaxed) + 1;
			}
			token.Merge(QueueSyncPoint{ dst_queue_type, submitted_value });
			{
				std::scoped_lock timeline_lock(device->allocationhandler->destroylocker);
				device->allocationhandler->submitted_queue_values[dst_queue_type] = submitted_value;
			}

			vulkan_check(vkQueueSubmit2(dst_queue.queue, 1, &submitInfo, cmd.fence));
			cmd.submitted_queue = dst_queue_type;
			cmd.submitted_value = submitted_value;
		}

		{
			std::scoped_lock lock(locker);
			inflight.push_back(std::move(cmd));
		}
		return token;
	}

	void GraphicsDevice_Vulkan::DescriptorBinder::flush(bool graphics, CommandList cmd)
	{
		if (dirty == DIRTY_NONE)
			return;

		CommandList_Vulkan& commandlist = device->GetCommandList(cmd);
		SDL_assert(commandlist.layout.pipeline_layout != VK_NULL_HANDLE); // No pipeline was set!

		uint32_t uniform_buffer_dynamic_offsets[DESCRIPTORBINDER_CBV_COUNT] = {};
		for (size_t i = 0; i < std::min((uint32_t)SDL_arraysize(uniform_buffer_dynamic_offsets), device->dynamic_cbv_count); ++i)
		{
			uniform_buffer_dynamic_offsets[i] = wiGraphicsGPUResourceIsValid(&table.CBV[i]) ? (uint32_t)table.CBV_offset[i] : 0;
		}

		if (descriptorSet == VK_NULL_HANDLE || (dirty & DIRTY_DESCRIPTOR))
		{
			const PSOLayout& layout = commandlist.layout;
			auto& binder_pool = commandlist.binder_pools[device->GetBufferIndex()];
			descriptorSet = binder_pool.allocate(layout);

			struct DescriptorTableInfo
			{
				VkDescriptorBufferInfo CBV[DESCRIPTORBINDER_CBV_COUNT];
				VkDescriptorImageInfo SRV[DESCRIPTORBINDER_SRV_COUNT];
				VkDescriptorImageInfo UAV[DESCRIPTORBINDER_UAV_COUNT];
				VkDescriptorImageInfo SAM[DESCRIPTORBINDER_SAMPLER_COUNT];
				VkWriteDescriptorSetAccelerationStructureKHR AccelerationStructure[DESCRIPTORBINDER_SRV_COUNT];
			} infos = {};

			struct DescriptorTableWrites
			{
				VkWriteDescriptorSet CBV[DESCRIPTORBINDER_CBV_COUNT];
				VkWriteDescriptorSet SRV[DESCRIPTORBINDER_SRV_COUNT];
				VkWriteDescriptorSet UAV[DESCRIPTORBINDER_UAV_COUNT];
				VkWriteDescriptorSet SAM[DESCRIPTORBINDER_SAMPLER_COUNT];
			} writes = {};

			VkDescriptorBufferInfo nullBufferInfo = {};
			nullBufferInfo.buffer = device->nullBuffer;
			nullBufferInfo.range = VK_WHOLE_SIZE;

			for (uint32_t i = 0; i < SDL_arraysize(table.CBV); ++i)
			{
				const GPUBuffer& buffer = table.CBV[i];
				auto& info = infos.CBV[i];
				auto& write = writes.CBV[i];
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorType = layout.table.CBV[i].descriptorType;
				write.dstBinding = VULKAN_BINDING_SHIFT_B + i;
				write.descriptorCount = 1;
				write.dstSet = descriptorSet;
				write.pBufferInfo = &info;

				if (wiGraphicsGPUResourceIsValid(&buffer))
				{
					auto internal_state = to_internal(&buffer);
					info.buffer = internal_state->resource;
					info.offset = i < device->dynamic_cbv_count ? 0 : table.CBV_offset[i];
					info.range = std::min(dynamic_cbv_maxsize, buffer.desc.size - table.CBV_offset[i]);
				}
				else
				{
					write.pBufferInfo = &nullBufferInfo;
				}
			}

			for (uint32_t i = 0; i < SDL_arraysize(table.SRV); ++i)
			{
				const GPUResource& res = table.SRV[i];
				auto& write = writes.SRV[i];
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorType = layout.table.SRV[i].descriptorType;
				write.dstBinding = VULKAN_BINDING_SHIFT_T + i;
				write.descriptorCount = 1;
				write.dstSet = descriptorSet;

				if (wiGraphicsGPUResourceIsBuffer(&res) && wiGraphicsGPUResourceIsValid(&res) && (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER))
				{
					auto internal_state = to_internal<GPUBuffer>(&res);
					int subresource = table.SRV_index[i];
					auto& descriptor = subresource >= 0 ? internal_state->subresources_srv[subresource] : internal_state->srv;
					if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
					{
						if (descriptor.buffer_view == VK_NULL_HANDLE)
						{
							write.pTexelBufferView = &device->nullBufferView;
						}
						else
						{
							write.pTexelBufferView = &descriptor.buffer_view;
						}
					}
					else
					{
						if (descriptor.buffer_info.buffer == VK_NULL_HANDLE)
						{
							write.pBufferInfo = &nullBufferInfo;
						}
						else
						{
							write.pBufferInfo = &descriptor.buffer_info;
						}
					}
				}
				else if (wiGraphicsGPUResourceIsTexture(&res) && wiGraphicsGPUResourceIsValid(&res) && write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
				{
					auto internal_state = to_internal<Texture>(&res);
					int subresource = table.SRV_index[i];
					auto& descriptor = subresource >= 0 ? internal_state->subresources_srv[subresource] : internal_state->srv;
					auto& info = infos.SRV[i];
					info.imageView = descriptor.image_view;
					info.imageLayout = internal_state->defaultLayout;
					write.pImageInfo = &info;
				}
				else if (wiGraphicsGPUResourceIsAccelerationStructure(&res) && wiGraphicsGPUResourceIsValid(&res) && write.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
				{
					auto internal_state = to_internal<RaytracingAccelerationStructure>(&res);
					auto& info = infos.AccelerationStructure[i];
					info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
					info.accelerationStructureCount = 1;
					info.pAccelerationStructures = &internal_state->resource;
					write.pNext = &info;
				}
				else
				{
					switch (write.descriptorType)
					{
					default:
					case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
						write.pBufferInfo = &nullBufferInfo;
						break;
					case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
						write.pTexelBufferView = &device->nullBufferView;
						break;
					case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
					{
						auto& info = infos.SRV[i];
						info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
						switch (layout.SRV_image_types[i])
						{
						default:
						case VK_IMAGE_VIEW_TYPE_1D:
							info.imageView = device->nullImageView1D;
							break;
						case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
							info.imageView = device->nullImageView1DArray;
							break;
						case VK_IMAGE_VIEW_TYPE_2D:
							info.imageView = device->nullImageView2D;
							break;
						case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
							info.imageView = device->nullImageView2DArray;
							break;
						case VK_IMAGE_VIEW_TYPE_3D:
							info.imageView = device->nullImageView3D;
							break;
						case VK_IMAGE_VIEW_TYPE_CUBE:
							info.imageView = device->nullImageViewCube;
							break;
						case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
							info.imageView = device->nullImageViewCubeArray;
							break;
						}
						write.pImageInfo = &info;
					}
					break;
					}
				}
			}

			for (uint32_t i = 0; i < SDL_arraysize(table.UAV); ++i)
			{
				const GPUResource& res = table.UAV[i];
				auto& write = writes.UAV[i];
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorType = layout.table.UAV[i].descriptorType;
				write.dstBinding = VULKAN_BINDING_SHIFT_U + i;
				write.descriptorCount = 1;
				write.dstSet = descriptorSet;

				if (wiGraphicsGPUResourceIsBuffer(&res) && wiGraphicsGPUResourceIsValid(&res) && (write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER))
				{
					auto internal_state = to_internal<GPUBuffer>(&res);
					int subresource = table.UAV_index[i];
					auto& descriptor = subresource >= 0 ? internal_state->subresources_uav[subresource] : internal_state->uav;
					if (write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
					{
						if (descriptor.buffer_view == VK_NULL_HANDLE)
						{
							write.pTexelBufferView = &device->nullBufferView;
						}
						else
						{
							write.pTexelBufferView = &descriptor.buffer_view;
						}
					}
					else
					{
						if (descriptor.buffer_info.buffer == VK_NULL_HANDLE)
						{
							write.pBufferInfo = &nullBufferInfo;
						}
						else
						{
							write.pBufferInfo = &descriptor.buffer_info;
						}
					}
				}
				else if (wiGraphicsGPUResourceIsTexture(&res) && wiGraphicsGPUResourceIsValid(&res) && write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
				{
					auto internal_state = to_internal<Texture>(&res);
					int subresource = table.UAV_index[i];
					auto& descriptor = subresource >= 0 ? internal_state->subresources_uav[subresource] : internal_state->uav;
					auto& info = infos.UAV[i];
					info.imageView = descriptor.image_view;
					info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					write.pImageInfo = &info;
				}
				else
				{
					switch (write.descriptorType)
					{
					default:
					case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
						write.pBufferInfo = &nullBufferInfo;
						break;
					case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
						write.pTexelBufferView = &device->nullBufferView;
						break;
					case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
					{
						auto& info = infos.UAV[i];
						info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
						switch (layout.UAV_image_types[i])
						{
						default:
						case VK_IMAGE_VIEW_TYPE_1D:
							info.imageView = device->nullImageView1D;
							break;
						case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
							info.imageView = device->nullImageView1DArray;
							break;
						case VK_IMAGE_VIEW_TYPE_2D:
							info.imageView = device->nullImageView2D;
							break;
						case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
							info.imageView = device->nullImageView2DArray;
							break;
						case VK_IMAGE_VIEW_TYPE_3D:
							info.imageView = device->nullImageView3D;
							break;
						case VK_IMAGE_VIEW_TYPE_CUBE:
							info.imageView = device->nullImageViewCube;
							break;
						case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
							info.imageView = device->nullImageViewCubeArray;
							break;
						}
						write.pImageInfo = &info;
					}
					break;
					}
				}
			}

			for (uint32_t i = 0; i < SDL_arraysize(table.SAM); ++i)
			{
				const Sampler& sam = table.SAM[i];
				auto& write = writes.SAM[i];
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorType = layout.table.SAM[i].descriptorType;
				write.dstBinding = VULKAN_BINDING_SHIFT_S + i;
				write.descriptorCount = 1;
				write.dstSet = descriptorSet;
				auto& info = infos.SAM[i];
				write.pImageInfo = &info;

				if (wiGraphicsSamplerIsValid(&sam))
				{
					auto internal_state = to_internal(&sam);
					info.sampler = internal_state->resource;
				}
				else
				{
					info.sampler = device->nullSampler;
				}
			}

			vkUpdateDescriptorSets(
				device->device,
				sizeof(writes) / sizeof(VkWriteDescriptorSet),
				(const VkWriteDescriptorSet*)&writes,
				0,
				nullptr
			);
		}

		VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		if (!graphics)
		{
			bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;

			if (commandlist.active_cs->stage == ShaderStage::LIB)
			{
				bindPoint = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
			}
		}

		DescriptorSets descriptor_sets = device->descriptor_sets;
		descriptor_sets.sets[DESCRIPTOR_SET_BINDINGS] = descriptorSet;

		vkCmdBindDescriptorSets(
			commandlist.GetCommandBuffer(),
			bindPoint,
			commandlist.layout.pipeline_layout,
			0,
			SDL_arraysize(descriptor_sets.sets),
			descriptor_sets.sets,
			device->dynamic_cbv_count,
			uniform_buffer_dynamic_offsets
		);

		dirty = DIRTY_NONE;
	}

	void GraphicsDevice_Vulkan::pso_validate(CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		if (!commandlist.dirty_pso)
			return;

		const PipelineState* pso = commandlist.active_pso;
		const PipelineHash& pipeline_hash = commandlist.prev_pipeline_hash;
		auto internal_state = to_internal(pso);

		VkPipeline pipeline = VK_NULL_HANDLE;
		bool pipeline_found = false;
		for (auto& item : pipelines_global)
		{
			if (item.first == pipeline_hash)
			{
				auto pipeline_internal = to_internal(&item.second);
				pipeline = pipeline_internal->pipeline;
				pipeline_found = true;
				break;
			}
		}
		if (!pipeline_found)
		{
			for (auto& x : commandlist.pipelines_worker)
			{
				if (pipeline_hash == x.first)
				{
					auto pipeline_internal = to_internal(&x.second);
					pipeline = pipeline_internal->pipeline;
					break;
				}
			}

			if (pipeline == VK_NULL_HANDLE)
			{
				PipelineState just_in_time_pso;
				bool success = CreatePipelineState(&pso->desc, &just_in_time_pso, &commandlist.renderpass_info);
				SDL_assert(success);

				commandlist.pipelines_worker.push_back(std::make_pair(pipeline_hash, just_in_time_pso));

				auto pipeline_internal = to_internal(&just_in_time_pso);
				pipeline = pipeline_internal->pipeline;
			}
		}
		SDL_assert(pipeline != VK_NULL_HANDLE);

		vkCmdBindPipeline(commandlist.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		commandlist.dirty_pso = false;
	}

	void GraphicsDevice_Vulkan::predraw(CommandList cmd)
	{
		pso_validate(cmd);

		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		commandlist.binder.flush(true, cmd);
	}
	void GraphicsDevice_Vulkan::predispatch(CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		commandlist.binder.flush(false, cmd);
	}

	// Engine functions
	GraphicsDevice_Vulkan::GraphicsDevice_Vulkan(wi::platform::window_type window, ValidationMode validationMode_, GPUPreference preference)
	{
		const Uint64 timer_begin = SDL_GetPerformanceCounter();
		capabilities |= GraphicsDeviceCapability::ALIASING_GENERIC;

		// This functionalty is missing from Vulkan but might be added in the future:
		//	Issue: https://github.com/KhronosGroup/Vulkan-Docs/issues/2079
		capabilities |= GraphicsDeviceCapability::COPY_BETWEEN_DIFFERENT_IMAGE_ASPECTS_NOT_SUPPORTED;

		std::deque<std::pair<uint32_t, wi::allocator::shared_ptr<std::mutex>>> queue_lockers;

		TOPLEVEL_ACCELERATION_STRUCTURE_INSTANCE_SIZE = sizeof(VkAccelerationStructureInstanceKHR);

		validationMode = validationMode_;

		VkResult res;

		res = vulkan_check(volkInitialize());
		if (res != VK_SUCCESS)
		{
			ShowVulkanErrorMessage("volkInitialize failed! ERROR: " + std::string(string_VkResult(res)));
			wi::platform::Exit();
		}

		// Fill out application info:
		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "Wicked Engine Application";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "Wicked Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(wi::version::GetMajor(), wi::version::GetMinor(), wi::version::GetRevision());
		appInfo.apiVersion = VK_API_VERSION_1_3;

		// Enumerate available layers and extensions:
		uint32_t instanceLayerCount;
		vulkan_check(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
		VkLayerProperties* availableInstanceLayers = nullptr;
		arrsetlen(availableInstanceLayers, instanceLayerCount);
		vulkan_check(vkEnumerateInstanceLayerProperties(&instanceLayerCount, availableInstanceLayers));

		uint32_t extensionCount = 0;
		vulkan_check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
		VkExtensionProperties* availableInstanceExtensions = nullptr;
		arrsetlen(availableInstanceExtensions, extensionCount);
		vulkan_check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableInstanceExtensions));

		const char** instanceLayers = nullptr;
		const char** instanceExtensions = nullptr;
		bool portabilityEnumerationEnabled = false;

		if (!tryEnableExtension(VK_KHR_SURFACE_EXTENSION_NAME, true, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Required Vulkan instance extension is unavailable: " + std::string(VK_KHR_SURFACE_EXTENSION_NAME));
			wi::platform::Exit();
		}
		if (!tryEnableExtension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, false, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: " + std::string(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME));
			wi::platform::Exit();
		}
		if (!tryEnableExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, false, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: " + std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
			wi::platform::Exit();
		}
		if (!tryEnableExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false, availableInstanceExtensions, extensionCount, &instanceExtensions, &debugUtils))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: " + std::string(VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
			wi::platform::Exit();
		}

#if defined(SDL3) || defined(SDL2)
		if (!WICKED_VULKAN_PLATFORM_WIN32)
		{
			uint32_t sdlExtensionCount = 0;
#if defined(SDL3)
			const char* const* extensionNames_sdl = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
#else
			SDL_Vulkan_GetInstanceExtensions(window, &sdlExtensionCount, nullptr);
			const char** extensionNames_sdl = nullptr;
			arrsetlen(extensionNames_sdl, sdlExtensionCount);
			SDL_Vulkan_GetInstanceExtensions(window, &sdlExtensionCount, extensionNames_sdl);
#endif
			for (uint32_t i = 0; i < sdlExtensionCount; ++i)
			{
				if (!appendUniqueName(extensionNames_sdl[i], &instanceExtensions))
				{
					ShowVulkanErrorMessage("Failed to append SDL Vulkan instance extension: " + std::string(extensionNames_sdl[i]));
					wi::platform::Exit();
				}
			}
#if defined(SDL2)
			arrfree(extensionNames_sdl);
#endif
		}
#endif

#if WICKED_VULKAN_PLATFORM_WIN32
		if (!tryEnableExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Required Vulkan instance extension is unavailable: " + std::string(VK_KHR_WIN32_SURFACE_EXTENSION_NAME));
			wi::platform::Exit();
		}
#elif WICKED_VULKAN_PLATFORM_ANDROID
		if (!tryEnableExtension("VK_KHR_android_surface", true, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Required Vulkan instance extension is unavailable: VK_KHR_android_surface");
			wi::platform::Exit();
		}
#elif WICKED_VULKAN_PLATFORM_APPLE
		if (!tryEnableExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME, true, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Required Vulkan instance extension is unavailable: " + std::string(VK_EXT_METAL_SURFACE_EXTENSION_NAME));
			wi::platform::Exit();
		}
		if (!tryEnableExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, true, availableInstanceExtensions, extensionCount, &instanceExtensions, &portabilityEnumerationEnabled))
		{
			ShowVulkanErrorMessage("Required Vulkan instance extension is unavailable: " + std::string(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
			wi::platform::Exit();
		}
#elif WICKED_VULKAN_PLATFORM_LINUX || WICKED_VULKAN_PLATFORM_BSD
		if (!tryEnableExtension("VK_KHR_xcb_surface", false, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: VK_KHR_xcb_surface");
			wi::platform::Exit();
		}
		if (!tryEnableExtension("VK_KHR_xlib_surface", false, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: VK_KHR_xlib_surface");
			wi::platform::Exit();
		}
		if (!tryEnableExtension("VK_KHR_wayland_surface", false, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: VK_KHR_wayland_surface");
			wi::platform::Exit();
		}
#elif WICKED_VULKAN_PLATFORM_QNX
		if (!tryEnableExtension("VK_QNX_screen_surface", false, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: VK_QNX_screen_surface");
			wi::platform::Exit();
		}
#elif WICKED_VULKAN_PLATFORM_SWITCH || WICKED_VULKAN_PLATFORM_SWITCH2
		if (!tryEnableExtension("VK_NN_vi_surface", false, availableInstanceExtensions, extensionCount, &instanceExtensions))
		{
			ShowVulkanErrorMessage("Failed to append Vulkan instance extension: VK_NN_vi_surface");
			wi::platform::Exit();
		}
#endif

		if (validationMode != ValidationMode::Disabled)
		{
			// Determine the optimal validation layers to enable that are necessary for useful debugging
			static const std::deque<std::deque<const char*>> validationLayerPriorityList =
			{
				// The preferred validation layer is "VK_LAYER_KHRONOS_validation"
				{"VK_LAYER_KHRONOS_validation"},

				// Otherwise we fallback to using the LunarG meta layer
				{"VK_LAYER_LUNARG_standard_validation"},

				// Otherwise we attempt to enable the individual layers that compose the LunarG meta layer since it doesn't exist
				{
					"VK_LAYER_GOOGLE_threading",
					"VK_LAYER_LUNARG_parameter_validation",
					"VK_LAYER_LUNARG_object_tracker",
					"VK_LAYER_LUNARG_core_validation",
					"VK_LAYER_GOOGLE_unique_objects",
				},

				// Otherwise as a last resort we fallback to attempting to enable the LunarG core layer
				{"VK_LAYER_LUNARG_core_validation"}
			};

			for (auto& validationLayers : validationLayerPriorityList)
			{
				if (ValidateLayers(validationLayers, availableInstanceLayers, instanceLayerCount))
				{
					for (auto& x : validationLayers)
					{
						arrput(instanceLayers, x);
					}
					break;
				}
			}
		}

		// Create instance:
		{
			VkInstanceCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			createInfo.pApplicationInfo = &appInfo;
			createInfo.enabledLayerCount = static_cast<uint32_t>(arrlenu(instanceLayers));
			createInfo.ppEnabledLayerNames = instanceLayers;
			createInfo.enabledExtensionCount = static_cast<uint32_t>(arrlenu(instanceExtensions));
			createInfo.ppEnabledExtensionNames = instanceExtensions;
#if WICKED_VULKAN_PLATFORM_APPLE
			if (portabilityEnumerationEnabled)
			{
				createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
			}
#endif

			VkDebugUtilsMessengerCreateInfoEXT debugUtilsCreateInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };

			if (validationMode != ValidationMode::Disabled && debugUtils)
			{
				debugUtilsCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
				debugUtilsCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

				if (validationMode == ValidationMode::Verbose)
				{
					debugUtilsCreateInfo.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
					debugUtilsCreateInfo.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
				}

				debugUtilsCreateInfo.pfnUserCallback = debugUtilsMessengerCallback;
				createInfo.pNext = &debugUtilsCreateInfo;
			}

			res = vulkan_check(vkCreateInstance(&createInfo, nullptr, &instance));
			if (res != VK_SUCCESS)
			{
				ShowVulkanErrorMessage("vkCreateInstance failed! ERROR: " + std::string(string_VkResult(res)));
				wi::platform::Exit();
			}

			volkLoadInstanceOnly(instance);

			if (validationMode != ValidationMode::Disabled && debugUtils)
			{
				vulkan_check(vkCreateDebugUtilsMessengerEXT(instance, &debugUtilsCreateInfo, nullptr, &debugUtilsMessenger));
			}
		}

		// Enumerating and creating devices:
		{
			uint32_t deviceCount = 0;
			vulkan_check(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
			if (deviceCount == 0)
			{
				VULKAN_LOG_ERROR("Failed to find GPU with Vulkan 1.3 support!");
				wi::platform::Exit();
			}

			VkPhysicalDevice* devices = nullptr;
			arrsetlen(devices, deviceCount);
			vulkan_check(vkEnumeratePhysicalDevices(instance, &deviceCount, devices));

			const std::deque<const char*> required_deviceExtensions = {
				VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if WICKED_VULKAN_PLATFORM_APPLE
				VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
#endif
			};
			const char** enabled_deviceExtensions = nullptr;

			bool h264_decode_extension = false;
			bool h265_decode_extension = false;
			bool video_decode_queue_extension = false;
			bool suitable = false;
			bool conservativeRasterization = false;

			auto checkPhysicalDeviceAndFillPropertiesFeatures = [&](VkPhysicalDevice dev) {
				uint32_t extensionCount;
				vulkan_check(vkEnumerateDeviceExtensionProperties(dev, nullptr, &extensionCount, nullptr));
				VkExtensionProperties* available_deviceExtensions = nullptr;
				arrsetlen(available_deviceExtensions, extensionCount);
				vulkan_check(vkEnumerateDeviceExtensionProperties(dev, nullptr, &extensionCount, available_deviceExtensions));

				arrsetlen(enabled_deviceExtensions, 0);
				auto enable_device_extension = [&](const char* extensionName, bool required, bool* out_enabled = nullptr) -> bool
				{
					return tryEnableExtension(extensionName, required, available_deviceExtensions, extensionCount, &enabled_deviceExtensions, out_enabled);
				};
				for (auto& x : required_deviceExtensions)
				{
					if (!enable_device_extension(x, true))
					{
						return false;
					}
				}

				VkPhysicalDeviceProperties api_properties = {};
				vkGetPhysicalDeviceProperties(dev, &api_properties);
				if (VK_API_VERSION_MAJOR(api_properties.apiVersion) < 1 ||
					(VK_API_VERSION_MAJOR(api_properties.apiVersion) == 1 && VK_API_VERSION_MINOR(api_properties.apiVersion) < 3))
				{
					return false;
				}

				h264_decode_extension = false;
				h265_decode_extension = false;
				video_decode_queue_extension = false;

				features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
				features_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
				features_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
				features_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
				features2.pNext = &features_1_1;
				features_1_1.pNext = &features_1_2;
				features_1_2.pNext = &features_1_3;
				void** features_chain = &features_1_3.pNext;
				acceleration_structure_features = {};
				raytracing_features = {};
				raytracing_query_features = {};
				fragment_shading_rate_features = {};
				mesh_shader_features = {};
				conditional_rendering_features = {};
				depth_clip_enable_features = {};

				properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
				properties_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
				properties_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
				properties_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
				properties2.pNext = &properties_1_1;
				properties_1_1.pNext = &properties_1_2;
				properties_1_2.pNext = &properties_1_3;
				void** properties_chain = &properties_1_3.pNext;
				sampler_minmax_properties = {};
				acceleration_structure_properties = {};
				raytracing_properties = {};
				fragment_shading_rate_properties = {};
				mesh_shader_properties = {};
				conservative_raster_properties = {};
				conservativeRasterization = false;

				sampler_minmax_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES;
				*properties_chain = &sampler_minmax_properties;
				properties_chain = &sampler_minmax_properties.pNext;

				depth_stencil_resolve_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
				*properties_chain = &depth_stencil_resolve_properties;
				properties_chain = &depth_stencil_resolve_properties.pNext;

				if (checkExtensionSupport(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME, false);
					image_view_min_lod_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT;
					*features_chain = &image_view_min_lod_features;
					features_chain = &image_view_min_lod_features.pNext;
				}
				if (checkExtensionSupport(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, false);
					depth_clip_enable_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT;
					*features_chain = &depth_clip_enable_features;
					features_chain = &depth_clip_enable_features.pNext;
				}
				if (checkExtensionSupport(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					conservativeRasterization = true;
					enable_device_extension(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, false);
					conservative_raster_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT;
					*properties_chain = &conservative_raster_properties;
					properties_chain = &conservative_raster_properties.pNext;
				}
					if (checkExtensionSupport(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, available_deviceExtensions, extensionCount))
					{
						const bool deferred_host_operations_supported = checkExtensionSupport(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, available_deviceExtensions, extensionCount);
						if (deferred_host_operations_supported)
						{
							enable_device_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false);
							enable_device_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, false);
							acceleration_structure_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
							*features_chain = &acceleration_structure_features;
							features_chain = &acceleration_structure_features.pNext;
							acceleration_structure_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
							*properties_chain = &acceleration_structure_properties;
							properties_chain = &acceleration_structure_properties.pNext;

							if (checkExtensionSupport(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, available_deviceExtensions, extensionCount))
							{
								enable_device_extension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, false);
								enable_device_extension(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, false);
								raytracing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
								*features_chain = &raytracing_features;
								features_chain = &raytracing_features.pNext;
								raytracing_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
								*properties_chain = &raytracing_properties;
								properties_chain = &raytracing_properties.pNext;
							}

							if (checkExtensionSupport(VK_KHR_RAY_QUERY_EXTENSION_NAME, available_deviceExtensions, extensionCount))
							{
								enable_device_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME, false);
								raytracing_query_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
								*features_chain = &raytracing_query_features;
								features_chain = &raytracing_query_features.pNext;
							}
						}
						else
						{
							SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan optional extension pair incomplete, skipping ray tracing path: %s requires %s", VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
						}
					}

				if (checkExtensionSupport(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, false);
					fragment_shading_rate_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
					*features_chain = &fragment_shading_rate_features;
					features_chain = &fragment_shading_rate_features.pNext;
					fragment_shading_rate_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
					*properties_chain = &fragment_shading_rate_properties;
					properties_chain = &fragment_shading_rate_properties.pNext;
				}

				if (checkExtensionSupport(VK_EXT_MESH_SHADER_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME, false);
					mesh_shader_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
					*features_chain = &mesh_shader_features;
					features_chain = &mesh_shader_features.pNext;
					mesh_shader_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
					*properties_chain = &mesh_shader_properties;
					properties_chain = &mesh_shader_properties.pNext;
				}

				if (checkExtensionSupport(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, false);
					conditional_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT;
					*features_chain = &conditional_rendering_features;
					features_chain = &conditional_rendering_features.pNext;
				}

				if (checkExtensionSupport(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, available_deviceExtensions, extensionCount) &&
					checkExtensionSupport(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, false, &video_decode_queue_extension);
					enable_device_extension(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, false);
					if (checkExtensionSupport(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, available_deviceExtensions, extensionCount))
					{
						enable_device_extension(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, false);
						h264_decode_extension = true;
					}
					if (checkExtensionSupport(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, available_deviceExtensions, extensionCount))
					{
						enable_device_extension(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, false);
						h265_decode_extension = true;
					}
				}

#if WICKED_VULKAN_PLATFORM_WIN32
				if (checkExtensionSupport(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, available_deviceExtensions, extensionCount) &&
					checkExtensionSupport(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, false);
					enable_device_extension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, false);
				}
#elif WICKED_VULKAN_PLATFORM_ANDROID || WICKED_VULKAN_PLATFORM_LINUX || WICKED_VULKAN_PLATFORM_BSD
				if (checkExtensionSupport(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, available_deviceExtensions, extensionCount) &&
					checkExtensionSupport(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, available_deviceExtensions, extensionCount))
				{
					enable_device_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, false);
					enable_device_extension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, false);
				}
#endif

					*properties_chain = nullptr;
					*features_chain = nullptr;
					vkGetPhysicalDeviceProperties2(dev, &properties2);
					vkGetPhysicalDeviceFeatures2(dev, &features2);
					if (features_1_3.dynamicRendering != VK_TRUE)
					{
						// Wicked Vulkan relies on dynamic rendering; reject unsuitable adapters during selection.
						return false;
					}
					const bool bindless_features_supported =
						features_1_2.descriptorIndexing == VK_TRUE &&
						features_1_2.runtimeDescriptorArray == VK_TRUE &&
						features_1_2.descriptorBindingVariableDescriptorCount == VK_TRUE &&
						features_1_2.descriptorBindingPartiallyBound == VK_TRUE &&
						features_1_2.descriptorBindingUpdateUnusedWhilePending == VK_TRUE &&
						features_1_2.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
						features_1_2.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE &&
						features_1_2.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
						features_1_2.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_TRUE &&
						features_1_2.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_TRUE &&
						features_1_2.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
						features_1_2.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE &&
						features_1_2.shaderStorageImageArrayNonUniformIndexing == VK_TRUE &&
						features_1_2.shaderUniformTexelBufferArrayNonUniformIndexing == VK_TRUE &&
						features_1_2.shaderStorageTexelBufferArrayNonUniformIndexing == VK_TRUE;
					if (!bindless_features_supported)
					{
						// Descriptor indexing is required by Wicked's bindless descriptor model.
						return false;
					}

					return true;
				};

			for (uint32_t dev_index = 0; dev_index < arrlenu(devices); ++dev_index)
			{
				const VkPhysicalDevice dev = devices[dev_index];
				if (!checkPhysicalDeviceAndFillPropertiesFeatures(dev))
					continue;

				bool priority = properties2.properties.deviceType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
				if (preference == GPUPreference::Integrated)
				{
					priority = properties2.properties.deviceType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
				}
				else if (preference == GPUPreference::AMD)
				{
					priority = ToUpperASCII(std::string(properties2.properties.deviceName)).find("AMD") != std::string::npos;
				}
				else if (preference == GPUPreference::Nvidia)
				{
					priority = ToUpperASCII(std::string(properties2.properties.deviceName)).find("NVIDIA") != std::string::npos;
				}
				else if (preference == GPUPreference::Intel)
				{
					priority = ToUpperASCII(std::string(properties2.properties.deviceName)).find("INTEL") != std::string::npos;
				}
				if (priority || physicalDevice == VK_NULL_HANDLE)
				{
					physicalDevice = dev;
					if (priority)
						break; // if this is prioritized GPU type, look no further
				}
			}

			if (physicalDevice == VK_NULL_HANDLE)
			{
				VULKAN_LOG_ERROR("Failed to find a suitable GPU with Vulkan 1.3 support!");
				wi::platform::Exit();
			}

			// This fills the properties and features again of the finally selected graphics card:
			checkPhysicalDeviceAndFillPropertiesFeatures(physicalDevice);
			timeline_semaphore_supported = features_1_2.timelineSemaphore == VK_TRUE;
			if (!timeline_semaphore_supported)
			{
				ShowVulkanErrorMessage("Selected Vulkan 1.3 device is missing required feature: timelineSemaphore");
				wi::platform::Exit();
			}

				if (properties2.properties.limits.timestampComputeAndGraphics != VK_TRUE)
				{
					SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan timestampComputeAndGraphics is disabled; cross-queue timestamp behavior may be inconsistent.");
				}

				auto log_optional_feature = [](VkBool32 enabled, const char* feature_name)
				{
					if (enabled != VK_TRUE)
					{
						SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan optional feature unavailable on selected adapter: %s", feature_name);
					}
				};

				log_optional_feature(features2.features.imageCubeArray, "imageCubeArray");
				log_optional_feature(features2.features.independentBlend, "independentBlend");
				log_optional_feature(features2.features.geometryShader, "geometryShader");
				log_optional_feature(features2.features.samplerAnisotropy, "samplerAnisotropy");
				log_optional_feature(features2.features.shaderClipDistance, "shaderClipDistance");
				log_optional_feature(features2.features.textureCompressionBC, "textureCompressionBC");
				log_optional_feature(features2.features.occlusionQueryPrecise, "occlusionQueryPrecise");
				if (features_1_3.dynamicRendering != VK_TRUE)
				{
					ShowVulkanErrorMessage("Selected Vulkan 1.3 device is missing required feature: dynamicRendering");
					wi::platform::Exit();
				}

			// Init adapter properties
			vendorId = properties2.properties.vendorID;
			deviceId = properties2.properties.deviceID;
			adapterName = properties2.properties.deviceName;

			driverDescription = properties_1_2.driverName;
			if (properties_1_2.driverInfo[0] != '\0')
			{
				driverDescription += std::string(": ") + properties_1_2.driverInfo;
			}

			switch (properties2.properties.deviceType)
			{
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
				adapterType = AdapterType::IntegratedGpu;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
				adapterType = AdapterType::DiscreteGpu;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
				adapterType = AdapterType::VirtualGpu;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_CPU:
				adapterType = AdapterType::Cpu;
				break;
			default:
				adapterType = AdapterType::Other;
				break;
			}

			if (features2.features.tessellationShader == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::TESSELLATION;
			}
			if (conservativeRasterization)
			{
				capabilities |= GraphicsDeviceCapability::CONSERVATIVE_RASTERIZATION;
			}
			if (features2.features.shaderStorageImageExtendedFormats == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::UAV_LOAD_FORMAT_COMMON;
			}

			if (
				raytracing_features.rayTracingPipeline == VK_TRUE &&
				raytracing_query_features.rayQuery == VK_TRUE &&
				acceleration_structure_features.accelerationStructure == VK_TRUE &&
				features_1_2.bufferDeviceAddress == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::RAYTRACING;
				SHADER_IDENTIFIER_SIZE = raytracing_properties.shaderGroupHandleSize;
			}
			if (mesh_shader_features.meshShader == VK_TRUE && mesh_shader_features.taskShader == VK_TRUE)
			{
				// Mesh shader still has issues on Vulkan: https://github.com/microsoft/DirectXShaderCompiler/issues/6862
				//capabilities |= GraphicsDeviceCapability::MESH_SHADER;
			}
			if (fragment_shading_rate_features.pipelineFragmentShadingRate == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::VARIABLE_RATE_SHADING;
			}
			if (fragment_shading_rate_features.attachmentFragmentShadingRate == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::VARIABLE_RATE_SHADING_TIER2;
				VARIABLE_RATE_SHADING_TILE_SIZE = std::min(fragment_shading_rate_properties.maxFragmentShadingRateAttachmentTexelSize.width, fragment_shading_rate_properties.maxFragmentShadingRateAttachmentTexelSize.height);
			}

			VkFormatProperties formatProperties = {};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, _ConvertFormat(Format::R11G11B10_FLOAT), &formatProperties);
			if (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
			{
				capabilities |= GraphicsDeviceCapability::UAV_LOAD_FORMAT_R11G11B10_FLOAT;
			}

			if (conditional_rendering_features.conditionalRendering == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::GRAPHICS_DEVICE_CAPABILITY_PREDICATION;
			}

			if (features_1_2.samplerFilterMinmax == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::SAMPLER_MINMAX;
			}

			if (features2.features.depthBounds == VK_TRUE)
			{
				capabilities |= GraphicsDeviceCapability::DEPTH_BOUNDS_TEST;
			}

			if (features2.features.sparseBinding == VK_TRUE && features2.features.sparseResidencyAliased == VK_TRUE)
			{
				if (properties2.properties.sparseProperties.residencyNonResidentStrict == VK_TRUE)
				{
					capabilities |= GraphicsDeviceCapability::SPARSE_NULL_MAPPING;
				}
				if (features2.features.sparseResidencyBuffer == VK_TRUE)
				{
					capabilities |= GraphicsDeviceCapability::SPARSE_BUFFER;
				}
				if (features2.features.sparseResidencyImage2D == VK_TRUE)
				{
					capabilities |= GraphicsDeviceCapability::SPARSE_TEXTURE2D;
				}
				if (features2.features.sparseResidencyImage3D == VK_TRUE)
				{
					capabilities |= GraphicsDeviceCapability::SPARSE_TEXTURE3D;
				}
			}

			if (
				(depth_stencil_resolve_properties.supportedDepthResolveModes & VK_RESOLVE_MODE_MIN_BIT) &&
				(depth_stencil_resolve_properties.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT)
				)
			{
				capabilities |= GraphicsDeviceCapability::DEPTH_RESOLVE_MIN_MAX;
			}
			if (
				(depth_stencil_resolve_properties.supportedStencilResolveModes & VK_RESOLVE_MODE_MIN_BIT) &&
				(depth_stencil_resolve_properties.supportedStencilResolveModes & VK_RESOLVE_MODE_MAX_BIT)
				)
			{
				capabilities |= GraphicsDeviceCapability::STENCIL_RESOLVE_MIN_MAX;
			}

			if(h264_decode_extension)
			{
				decode_h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
				decode_h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
				decode_h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_INTERLEAVED_LINES_BIT_KHR;

				decode_h264_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;

				video_capability_h264.profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
				video_capability_h264.profile.pNext = &decode_h264_profile;
				video_capability_h264.profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
				video_capability_h264.profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
				video_capability_h264.profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
				video_capability_h264.profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;

				video_capability_h264.decode_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;

				video_capability_h264.video_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
				video_capability_h264.video_capabilities.pNext = &video_capability_h264.decode_capabilities;
				video_capability_h264.decode_capabilities.pNext = &decode_h264_capabilities;
				vulkan_check(vkGetPhysicalDeviceVideoCapabilitiesKHR(physicalDevice, &video_capability_h264.profile, &video_capability_h264.video_capabilities));

				if (video_capability_h264.decode_capabilities.flags)
				{
					capabilities |= GraphicsDeviceCapability::VIDEO_DECODE_H264;
					VIDEO_DECODE_BITSTREAM_ALIGNMENT = align(VIDEO_DECODE_BITSTREAM_ALIGNMENT, video_capability_h264.video_capabilities.minBitstreamBufferOffsetAlignment);
					VIDEO_DECODE_BITSTREAM_ALIGNMENT = align(VIDEO_DECODE_BITSTREAM_ALIGNMENT, video_capability_h264.video_capabilities.minBitstreamBufferSizeAlignment);
				}
			}

			if (h265_decode_extension)
			{
				decode_h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
				decode_h265_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;

				decode_h265_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;

				video_capability_h265.profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
				video_capability_h265.profile.pNext = &decode_h265_profile;
				video_capability_h265.profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
				video_capability_h265.profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
				video_capability_h265.profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
				video_capability_h265.profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;

				video_capability_h265.decode_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;

				video_capability_h265.video_capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
				video_capability_h265.video_capabilities.pNext = &video_capability_h265.decode_capabilities;
				video_capability_h265.decode_capabilities.pNext = &decode_h265_capabilities;
				vulkan_check(vkGetPhysicalDeviceVideoCapabilitiesKHR(physicalDevice, &video_capability_h265.profile, &video_capability_h265.video_capabilities));

				if (video_capability_h265.decode_capabilities.flags)
				{
					//capabilities |= GraphicsDeviceCapability::VIDEO_DECODE_H265; //TODO
					VIDEO_DECODE_BITSTREAM_ALIGNMENT = align(VIDEO_DECODE_BITSTREAM_ALIGNMENT, video_capability_h265.video_capabilities.minBitstreamBufferOffsetAlignment);
					VIDEO_DECODE_BITSTREAM_ALIGNMENT = align(VIDEO_DECODE_BITSTREAM_ALIGNMENT, video_capability_h265.video_capabilities.minBitstreamBufferSizeAlignment);
				}
			}

			// Find queue families:
			uint32_t queueFamilyCount = 0;
			const bool has_queue_family_properties2 = vkGetPhysicalDeviceQueueFamilyProperties2 != nullptr;
			const bool query_video_queue_properties = video_decode_queue_extension;
			if (has_queue_family_properties2)
			{
				vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &queueFamilyCount, nullptr);

				arrsetlen(queueFamilies, queueFamilyCount);
				arrsetlen(queueFamiliesVideo, queueFamilyCount);
				for (uint32_t i = 0; i < queueFamilyCount; ++i)
				{
					queueFamilies[i] = {};
					queueFamilies[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
					if (query_video_queue_properties)
					{
						queueFamiliesVideo[i] = {};
						queueFamiliesVideo[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
						queueFamilies[i].pNext = &queueFamiliesVideo[i];
					}
					else
					{
						queueFamilies[i].pNext = nullptr;
					}
				}
				vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &queueFamilyCount, queueFamilies);
			}
			else
			{
				// Robust fallback for loaders/drivers where vkGetPhysicalDeviceQueueFamilyProperties2 is unavailable.
				vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

				VkQueueFamilyProperties* queueFamiliesLegacy = nullptr;
				arrsetlen(queueFamiliesLegacy, queueFamilyCount);
				vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamiliesLegacy);

				arrsetlen(queueFamilies, queueFamilyCount);
				arrsetlen(queueFamiliesVideo, queueFamilyCount);
				for (uint32_t i = 0; i < queueFamilyCount; ++i)
				{
					queueFamilies[i] = {};
					queueFamilies[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
					queueFamilies[i].pNext = nullptr;
					queueFamilies[i].queueFamilyProperties = queueFamiliesLegacy[i];

					queueFamiliesVideo[i] = {};
					queueFamiliesVideo[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
				}

				arrfree(queueFamiliesLegacy);
			}

			// Query base queue families:
			for (uint32_t i = 0; i < queueFamilyCount; ++i)
			{
				auto& queueFamily = queueFamilies[i];
				auto& queueFamilyVideo = queueFamiliesVideo[i];

				if (graphicsFamily == VK_QUEUE_FAMILY_IGNORED && queueFamily.queueFamilyProperties.queueCount > 0 && queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT)
				{
					graphicsFamily = i;
					if (queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
					{
						queues[QUEUE_GRAPHICS].sparse_binding_supported = true;
					}
				}

				if (copyFamily == VK_QUEUE_FAMILY_IGNORED && queueFamily.queueFamilyProperties.queueCount > 0 && queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_TRANSFER_BIT)
				{
					copyFamily = i;
					initFamily = i;
				}

				if (computeFamily == VK_QUEUE_FAMILY_IGNORED && queueFamily.queueFamilyProperties.queueCount > 0 && queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT)
				{
					computeFamily = i;
				}

				if (videoFamily == VK_QUEUE_FAMILY_IGNORED &&
					queueFamily.queueFamilyProperties.queueCount > 0 &&
					(queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) &&
					(!h264_decode_extension || (queueFamilyVideo.videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)) &&
					(!h265_decode_extension || (queueFamilyVideo.videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR))
					)
				{
					videoFamily = i;
				}
			}

			// Now try to find dedicated COPY queue:
			for (uint32_t i = 0; i < queueFamilyCount; ++i)
			{
				auto& queueFamily = queueFamilies[i];

				if (queueFamily.queueFamilyProperties.queueCount > 0 &&
					queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_TRANSFER_BIT &&
					!(queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
					!(queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT)
					)
				{
					copyFamily = i;
					initFamily = i;

					if (queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
					{
						queues[QUEUE_COPY].sparse_binding_supported = true;
					}
					break; // found it!
				}
			}

			// Now try to find dedicated COMPUTE queue:
			for (uint32_t i = 0; i < queueFamilyCount; ++i)
			{
				auto& queueFamily = queueFamilies[i];

				if (queueFamily.queueFamilyProperties.queueCount > 0 &&
					queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT &&
					!(queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT)
					)
				{
					computeFamily = i;

					if (queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
					{
						queues[QUEUE_COMPUTE].sparse_binding_supported = true;
					}
					break; // found it!
				}
			}

			// Now try to find dedicated transfer queue with only transfer and sparse flags:
			//	(This is a workaround for a driver bug with sparse updating from transfer queue)
			for (uint32_t i = 0; i < queueFamilyCount; ++i)
			{
				auto& queueFamily = queueFamilies[i];

				if (queueFamily.queueFamilyProperties.queueCount > 0 && queueFamily.queueFamilyProperties.queueFlags == (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT))
				{
					copyFamily = i;
					queues[QUEUE_COPY].sparse_binding_supported = true;
				}
			}

			// Find sparse fallback:
			for (uint32_t i = 0; i < queueFamilyCount; ++i)
			{
				auto& queueFamily = queueFamilies[i];

				if (queueFamily.queueFamilyProperties.queueCount > 0 && (queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT))
				{
					sparseFamily = i;
					break;
				}
			}

			// Try to find separate transfer queue for inits if available, otherwise it will use QUEUE_COPY
			for (uint32_t i = 0; i < queueFamilyCount; ++i)
			{
				auto& queueFamily = queueFamilies[i];

				if (queueFamily.queueFamilyProperties.queueCount > 0 &&
					queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_TRANSFER_BIT &&
					copyFamily != i &&
					!(queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
					!(queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT)
					)
				{
					initFamily = i;
					break;
				}
			}

			VkDeviceQueueCreateInfo* queueCreateInfos = nullptr;
			uint32_t* uniqueQueueFamilies = nullptr;
			auto append_unique_queue_family = [&](uint32_t queueFamily)
			{
				if (queueFamily == VK_QUEUE_FAMILY_IGNORED)
				{
					return;
				}
				for (size_t i = 0; i < arrlenu(uniqueQueueFamilies); ++i)
				{
					if (uniqueQueueFamilies[i] == queueFamily)
					{
						return;
					}
				}
				// stb_ds array: unique queue family IDs are tracked in a plain dynamic array.
				arrput(uniqueQueueFamilies, queueFamily);
			};
			append_unique_queue_family(graphicsFamily);
			append_unique_queue_family(copyFamily);
			append_unique_queue_family(computeFamily);
			append_unique_queue_family(initFamily);
			append_unique_queue_family(videoFamily);
			append_unique_queue_family(sparseFamily);

			auto find_queue_locker = [&](uint32_t queueFamily) -> wi::allocator::shared_ptr<std::mutex>&
			{
				for (auto& item : queue_lockers)
				{
					if (item.first == queueFamily)
					{
						return item.second;
					}
				}
				SDL_assert(0 && "Queue locker missing");
				return queue_lockers.front().second;
			};

			float queuePriority = 1.0f;
			for (size_t queue_index = 0; queue_index < arrlenu(uniqueQueueFamilies); ++queue_index)
			{
				const uint32_t queueFamily = uniqueQueueFamilies[queue_index];
				queue_lockers.emplace_back(queueFamily, wi::allocator::make_shared_single<std::mutex>());
				VkDeviceQueueCreateInfo queueCreateInfo = {};
				queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queueCreateInfo.queueFamilyIndex = queueFamily;
				queueCreateInfo.queueCount = 1;
				queueCreateInfo.pQueuePriorities = &queuePriority;
				arrput(queueCreateInfos, queueCreateInfo);
				arrput(families, queueFamily);
			}
			arrfree(uniqueQueueFamilies);

			VkDeviceCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			createInfo.queueCreateInfoCount = (uint32_t)arrlenu(queueCreateInfos);
			createInfo.pQueueCreateInfos = queueCreateInfos;
			createInfo.pEnabledFeatures = nullptr;
			createInfo.pNext = &features2;
			createInfo.enabledExtensionCount = static_cast<uint32_t>(arrlenu(enabled_deviceExtensions));
			createInfo.ppEnabledExtensionNames = enabled_deviceExtensions;

			res = vulkan_check(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device));
			if (res != VK_SUCCESS)
			{
				ShowVulkanErrorMessage("vkCreateDevice failed! ERROR: " + std::string(string_VkResult(res)));
				wi::platform::Exit();
			}

			volkLoadDevice(device);
		}

		// queues:
		{
			auto find_queue_locker = [&](uint32_t queueFamily) -> wi::allocator::shared_ptr<std::mutex>&
			{
				for (auto& item : queue_lockers)
				{
					if (item.first == queueFamily)
					{
						return item.second;
					}
				}
				SDL_assert(0 && "Queue locker missing");
				return queue_lockers.front().second;
			};

			vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
			vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);
			vkGetDeviceQueue(device, copyFamily, 0, &copyQueue);
			vkGetDeviceQueue(device, initFamily, 0, &initQueue);
			if (videoFamily != VK_QUEUE_FAMILY_IGNORED)
			{
				vkGetDeviceQueue(device, videoFamily, 0, &videoQueue);
			}
			if (sparseFamily != VK_QUEUE_FAMILY_IGNORED)
			{
				vkGetDeviceQueue(device, sparseFamily, 0, &sparseQueue);
			}

			queues[QUEUE_GRAPHICS].queue = graphicsQueue;
			queues[QUEUE_GRAPHICS].locker = find_queue_locker(graphicsFamily);
			queues[QUEUE_COMPUTE].queue = computeQueue;
			queues[QUEUE_COMPUTE].locker = find_queue_locker(computeFamily);
			queues[QUEUE_COPY].queue = copyQueue;
			queues[QUEUE_COPY].locker = find_queue_locker(copyFamily);
			queue_init.queue = initQueue;
			queue_init.locker = find_queue_locker(initFamily);
			if (videoFamily != VK_QUEUE_FAMILY_IGNORED)
			{
				queues[QUEUE_VIDEO_DECODE].queue = videoQueue;
				queues[QUEUE_VIDEO_DECODE].locker = find_queue_locker(videoFamily);
			}
			if (sparseFamily != VK_QUEUE_FAMILY_IGNORED)
			{
				queue_sparse.queue = sparseQueue;
				queue_sparse.locker = find_queue_locker(sparseFamily);
				queue_sparse.sparse_binding_supported = true;
			}

		}

		memory_properties_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
		vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &memory_properties_2);

		if (memory_properties_2.memoryProperties.memoryHeapCount == 1 &&
			memory_properties_2.memoryProperties.memoryHeaps[0].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
		{
			// https://registry.khronos.org/vulkan/specs/1.0-extensions/html/vkspec.html#memory-device
			//	"In a unified memory architecture (UMA) system there is often only a single memory heap which is
			//	considered to be equally “local” to the host and to the device, and such an implementation must advertise the heap as device-local."
			capabilities |= GraphicsDeviceCapability::CACHE_COHERENT_UMA;
		}

		allocationhandler = wi::allocator::make_shared_single<AllocationHandler>();
		allocationhandler->device = device;
		allocationhandler->instance = instance;
		allocationhandler->queue_timeline_supported = timeline_semaphore_supported;
		for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
		{
			allocationhandler->queue_timeline_completed[q] = &queue_timeline_completed_fallback[q];
		}

		// Initialize Vulkan Memory Allocator helper:
		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = physicalDevice;
			allocatorInfo.device = device;
			allocatorInfo.instance = instance;
			allocatorInfo.pAllocationCallbacks = &vma_allocation_callbacks;
			allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;

		// Core in 1.1
		allocatorInfo.flags =
			VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT |
			VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT;

		if (features_1_2.bufferDeviceAddress)
		{
			allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
		}

#if VMA_DYNAMIC_VULKAN_FUNCTIONS
		static VmaVulkanFunctions vulkanFunctions = {};
		vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
		vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
		allocatorInfo.pVulkanFunctions = &vulkanFunctions;
#endif
		res = vulkan_check(vmaCreateAllocator(&allocatorInfo, &allocationhandler->allocator));
		if (res != VK_SUCCESS)
		{
			ShowVulkanErrorMessage("vmaCreateAllocator failed! ERROR: " + std::string(string_VkResult(res)));
			wi::platform::Exit();
		}

		VkExternalMemoryHandleTypeFlags* externalMemoryHandleTypes = nullptr;
#if WICKED_VULKAN_PLATFORM_WIN32
		arrsetlen(externalMemoryHandleTypes, memory_properties_2.memoryProperties.memoryTypeCount);
		for (uint32_t i = 0; i < memory_properties_2.memoryProperties.memoryTypeCount; ++i)
		{
			externalMemoryHandleTypes[i] = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		}
		allocatorInfo.pTypeExternalMemoryHandleTypes = externalMemoryHandleTypes;
#elif WICKED_VULKAN_PLATFORM_ANDROID || WICKED_VULKAN_PLATFORM_LINUX || WICKED_VULKAN_PLATFORM_BSD
		arrsetlen(externalMemoryHandleTypes, memory_properties_2.memoryProperties.memoryTypeCount);
		for (uint32_t i = 0; i < memory_properties_2.memoryProperties.memoryTypeCount; ++i)
		{
			externalMemoryHandleTypes[i] = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		}
		allocatorInfo.pTypeExternalMemoryHandleTypes = externalMemoryHandleTypes;
#endif

		res = vulkan_check(vmaCreateAllocator(&allocatorInfo, &allocationhandler->externalAllocator));
		if (res != VK_SUCCESS)
		{
			ShowVulkanErrorMessage("Failed to create Vulkan external memory allocator, ERROR: " + std::string(string_VkResult(res)));
			wi::platform::Exit();
		}

		copyAllocator.init(this);

		if (timeline_semaphore_supported)
		{
			for (int queue = 0; queue < QUEUE_COUNT; ++queue)
			{
				if (queues[queue].queue == VK_NULL_HANDLE)
					continue;

				VkSemaphoreTypeCreateInfo typeInfo = {};
				typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
				typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
				typeInfo.initialValue = 0;

				VkSemaphoreCreateInfo semaphoreInfo = {};
				semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				semaphoreInfo.pNext = &typeInfo;
				vulkan_check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &queues[queue].timeline_semaphore));
				set_semaphore_name(queues[queue].timeline_semaphore, "CommandQueue::timeline_semaphore");
				allocationhandler->queue_timeline_semaphores[queue] = queues[queue].timeline_semaphore;
			}
		}

		// Create frame resources:
		for (uint32_t fr = 0; fr < BUFFERCOUNT; ++fr)
		{
			for (int queue = 0; queue < QUEUE_COUNT; ++queue)
			{
				if (queues[queue].queue == VK_NULL_HANDLE)
					continue;

				VkFenceCreateInfo fenceInfo = {};
				fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
				//fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
				res = vulkan_check(vkCreateFence(device, &fenceInfo, nullptr, &frame_fence[fr][queue]));
				if (res != VK_SUCCESS)
				{
					ShowVulkanErrorMessage("vkCreateFence[FRAME] failed! ERROR: " + std::string(string_VkResult(res)));
					wi::platform::Exit();
				}
				switch (queue)
				{
				case QUEUE_GRAPHICS:
					set_fence_name(frame_fence[fr][queue], "frame_fence[QUEUE_GRAPHICS]");
					break;
				case QUEUE_COMPUTE:
					set_fence_name(frame_fence[fr][queue], "frame_fence[QUEUE_COMPUTE]");
					break;
				case QUEUE_COPY:
					set_fence_name(frame_fence[fr][queue], "frame_fence[QUEUE_COPY]");
					break;
				case QUEUE_VIDEO_DECODE:
					set_fence_name(frame_fence[fr][queue], "frame_fence[QUEUE_VIDEO_DECODE]");
					break;
				};
			}

			// Frame end semaphores:
			for (int queue1 = 0; queue1 < QUEUE_COUNT; ++queue1)
			{
				if (queues[queue1].queue == nullptr)
					continue;
				for (int queue2 = 0; queue2 < QUEUE_COUNT; ++queue2)
				{
					if (queue1 == queue2)
						continue;
					if (queues[queue2].queue == nullptr)
						continue;

					VkSemaphoreCreateInfo info = {};
					info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
					vulkan_check(vkCreateSemaphore(device, &info, nullptr, &queues[queue1].frame_semaphores[fr][queue2]));
					set_semaphore_name(queues[queue1].frame_semaphores[fr][queue2], "CommandQueue::frame_semaphores");
				}
			}
		}

		TIMESTAMP_FREQUENCY = uint64_t(1.0 / double(properties2.properties.limits.timestampPeriod) * 1000 * 1000 * 1000);

		dynamic_cbv_count = std::min(ROOT_CBV_COUNT, properties2.properties.limits.maxDescriptorSetUniformBuffersDynamic);

		// Dynamic PSO states:
		arrput(pso_dynamicStates, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
		arrput(pso_dynamicStates, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
		arrput(pso_dynamicStates, VK_DYNAMIC_STATE_STENCIL_REFERENCE);
		arrput(pso_dynamicStates, VK_DYNAMIC_STATE_BLEND_CONSTANTS);
		if (CheckCapability(GraphicsDeviceCapability::DEPTH_BOUNDS_TEST))
		{
			arrput(pso_dynamicStates, VK_DYNAMIC_STATE_DEPTH_BOUNDS);
		}
		if (CheckCapability(GraphicsDeviceCapability::VARIABLE_RATE_SHADING))
		{
			arrput(pso_dynamicStates, VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR);
		}
		arrput(pso_dynamicStates, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);

		dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicStateInfo.pDynamicStates = pso_dynamicStates;
		dynamicStateInfo.dynamicStateCount = (uint32_t)arrlenu(pso_dynamicStates);

		dynamicStateInfo_MeshShader.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicStateInfo_MeshShader.pDynamicStates = pso_dynamicStates;
		dynamicStateInfo_MeshShader.dynamicStateCount = (uint32_t)arrlenu(pso_dynamicStates) - 1; // don't include VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE for mesh shader

		// Static samplers:
		{
			VkSamplerCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			createInfo.pNext = nullptr;
			createInfo.flags = 0;
			createInfo.compareEnable = false;
			createInfo.compareOp = VK_COMPARE_OP_NEVER;
			createInfo.minLod = 0;
			createInfo.maxLod = FLT_MAX;
			createInfo.mipLodBias = 0;
			createInfo.anisotropyEnable = false;
			createInfo.maxAnisotropy = 0;

			// sampler_linear_clamp:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[0]));

			// sampler_linear_wrap:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[1]));

			//sampler_linear_mirror:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[2]));

			// sampler_point_clamp:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[3]));

			// sampler_point_wrap:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[4]));

			// sampler_point_mirror:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[5]));

			// sampler_aniso_clamp:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.anisotropyEnable = true;
			createInfo.maxAnisotropy = 16;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[6]));

			// sampler_aniso_wrap:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.anisotropyEnable = true;
			createInfo.maxAnisotropy = 16;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[7]));

			// sampler_aniso_mirror:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			createInfo.anisotropyEnable = true;
			createInfo.maxAnisotropy = 16;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[8]));

			// sampler_cmp_depth:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.anisotropyEnable = false;
			createInfo.maxAnisotropy = 0;
			createInfo.compareEnable = true;
			createInfo.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
			createInfo.minLod = 0;
			createInfo.maxLod = 0;
			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &immutable_samplers[9]));
		}

		// Create null descriptors for uninitialized bind slots and bindless safety fallback:
		{
			VkBufferCreateInfo bufferInfo = {};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
				// Keep this at least one RGBA32 texel so null texel-buffer views are valid on MoltenVK/Metal.
				bufferInfo.size = 16;
			bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			bufferInfo.flags = 0;

			VmaAllocationCreateInfo allocInfo = {};
			allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			vulkan_check(vmaCreateBuffer(allocationhandler->allocator, &bufferInfo, &allocInfo, &nullBuffer, &nullBufferAllocation, nullptr));

			VkBufferViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
			viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
			viewInfo.range = VK_WHOLE_SIZE;
			viewInfo.buffer = nullBuffer;
			vulkan_check(vkCreateBufferView(device, &viewInfo, nullptr, &nullBufferView));
		}
		{
			VkImageCreateInfo imageInfo = {};
			imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageInfo.extent.width = 1;
			imageInfo.extent.height = 1;
			imageInfo.extent.depth = 1;
			imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			imageInfo.arrayLayers = 1;
			imageInfo.mipLevels = 1;
			imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			imageInfo.flags = 0;

			VmaAllocationCreateInfo allocInfo = {};
			allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

			imageInfo.imageType = VK_IMAGE_TYPE_1D;
			vulkan_check(vmaCreateImage(allocationhandler->allocator, &imageInfo, &allocInfo, &nullImage1D, &nullImageAllocation1D, nullptr));

			imageInfo.imageType = VK_IMAGE_TYPE_2D;
			imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			imageInfo.arrayLayers = 6;
			vulkan_check(vmaCreateImage(allocationhandler->allocator, &imageInfo, &allocInfo, &nullImage2D, &nullImageAllocation2D, nullptr));

			imageInfo.imageType = VK_IMAGE_TYPE_3D;
			imageInfo.flags = 0;
			imageInfo.arrayLayers = 1;
			vulkan_check(vmaCreateImage(allocationhandler->allocator, &imageInfo, &allocInfo, &nullImage3D, &nullImageAllocation3D, nullptr));

			// Transitions:
			{
				VkImageMemoryBarrier2 barrier = {};
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrier.oldLayout = imageInfo.initialLayout;
				barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
				barrier.srcAccessMask = 0;
				barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
				barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				barrier.subresourceRange.baseArrayLayer = 0;
				barrier.subresourceRange.baseMipLevel = 0;
				barrier.subresourceRange.levelCount = 1;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

				barrier.image = nullImage1D;
				barrier.subresourceRange.layerCount = 1;
				arrput(init_transitions, barrier);

				barrier.image = nullImage2D;
				barrier.subresourceRange.layerCount = 6;
				arrput(init_transitions, barrier);

				barrier.image = nullImage3D;
				barrier.subresourceRange.layerCount = 1;
				arrput(init_transitions, barrier);
			}

			VkImageViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			viewInfo.subresourceRange.baseArrayLayer = 0;
			viewInfo.subresourceRange.layerCount = 1;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;

			viewInfo.image = nullImage1D;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_1D;
			vulkan_check(vkCreateImageView(device, &viewInfo, nullptr, &nullImageView1D));

			viewInfo.image = nullImage1D;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			vulkan_check(vkCreateImageView(device, &viewInfo, nullptr, &nullImageView1DArray));

			viewInfo.image = nullImage2D;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vulkan_check(vkCreateImageView(device, &viewInfo, nullptr, &nullImageView2D));

			viewInfo.image = nullImage2D;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			vulkan_check(vkCreateImageView(device, &viewInfo, nullptr, &nullImageView2DArray));

			viewInfo.image = nullImage2D;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
			viewInfo.subresourceRange.layerCount = 6;
			vulkan_check(vkCreateImageView(device, &viewInfo, nullptr, &nullImageViewCube));

			viewInfo.image = nullImage2D;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
			viewInfo.subresourceRange.layerCount = 6;
			vulkan_check(vkCreateImageView(device, &viewInfo, nullptr, &nullImageViewCubeArray));

			viewInfo.image = nullImage3D;
			viewInfo.subresourceRange.layerCount = 1;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
			vulkan_check(vkCreateImageView(device, &viewInfo, nullptr, &nullImageView3D));
		}
		{
			VkSamplerCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

			vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &nullSampler));
		}

		// Bindings:
		{
			for (uint32_t i = 0; i < SDL_arraysize(layout_template.table.CBV); ++i)
			{
				auto& binding = layout_template.table.CBV[i];
				binding.descriptorType = i < dynamic_cbv_count ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				binding.binding = VULKAN_BINDING_SHIFT_B + i;
				binding.descriptorCount = 1;
				binding.stageFlags = VK_SHADER_STAGE_ALL;
			}
			for (uint32_t i = 0; i < SDL_arraysize(layout_template.table.SRV); ++i)
			{
				auto& binding = layout_template.table.SRV[i];
				binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				binding.binding = VULKAN_BINDING_SHIFT_T + i;
				binding.descriptorCount = 1;
				binding.stageFlags = VK_SHADER_STAGE_ALL;
			}
			for (uint32_t i = 0; i < SDL_arraysize(layout_template.table.UAV); ++i)
			{
				auto& binding = layout_template.table.UAV[i];
				binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				binding.binding = VULKAN_BINDING_SHIFT_U + i;
				binding.descriptorCount = 1;
				binding.stageFlags = VK_SHADER_STAGE_ALL;
			}
			for (uint32_t i = 0; i < SDL_arraysize(layout_template.table.SAM); ++i)
			{
				auto& binding = layout_template.table.SAM[i];
				binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				binding.binding = VULKAN_BINDING_SHIFT_S + i;
				binding.descriptorCount = 1;
				binding.stageFlags = VK_SHADER_STAGE_ALL;
			}
			for (uint32_t i = 0; i < SDL_arraysize(layout_template.table.IMMUTABLE_SAM); ++i)
			{
				auto& binding = layout_template.table.IMMUTABLE_SAM[i];
				binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				binding.binding = VULKAN_BINDING_SHIFT_S + STATIC_SAMPLER_SLOT_BEGIN + i;
				binding.descriptorCount = 1;
				binding.pImmutableSamplers = &immutable_samplers[i];
				binding.stageFlags = VK_SHADER_STAGE_ALL;
			}
		}

			// Bindless:
			{
				struct VulkanRequiredFeature
				{
					VkBool32 value;
					const char* name;
				};
				const VulkanRequiredFeature bindless_required_features[] =
				{
					{features_1_2.descriptorIndexing, "descriptorIndexing"},
					{features_1_2.runtimeDescriptorArray, "runtimeDescriptorArray"},
					{features_1_2.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount"},
					{features_1_2.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
					{features_1_2.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending"},
					{features_1_2.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind"},
					{features_1_2.descriptorBindingStorageImageUpdateAfterBind, "descriptorBindingStorageImageUpdateAfterBind"},
					{features_1_2.descriptorBindingStorageBufferUpdateAfterBind, "descriptorBindingStorageBufferUpdateAfterBind"},
					{features_1_2.descriptorBindingUniformTexelBufferUpdateAfterBind, "descriptorBindingUniformTexelBufferUpdateAfterBind"},
					{features_1_2.descriptorBindingStorageTexelBufferUpdateAfterBind, "descriptorBindingStorageTexelBufferUpdateAfterBind"},
					{features_1_2.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing"},
					{features_1_2.shaderStorageBufferArrayNonUniformIndexing, "shaderStorageBufferArrayNonUniformIndexing"},
					{features_1_2.shaderStorageImageArrayNonUniformIndexing, "shaderStorageImageArrayNonUniformIndexing"},
					{features_1_2.shaderUniformTexelBufferArrayNonUniformIndexing, "shaderUniformTexelBufferArrayNonUniformIndexing"},
					{features_1_2.shaderStorageTexelBufferArrayNonUniformIndexing, "shaderStorageTexelBufferArrayNonUniformIndexing"},
				};
				std::string missing_bindless_features;
				for (const auto& feature : bindless_required_features)
				{
					if (feature.value != VK_TRUE)
					{
						if (!missing_bindless_features.empty())
						{
							missing_bindless_features += ", ";
						}
						missing_bindless_features += feature.name;
					}
				}
				if (!missing_bindless_features.empty())
				{
					SDL_LogError(
						SDL_LOG_CATEGORY_APPLICATION,
						"Selected Vulkan adapter is missing required descriptor indexing features: %s",
						missing_bindless_features.c_str()
					);
					ShowVulkanErrorMessage(
						"Selected Vulkan 1.3 device is missing required descriptor indexing features for Wicked bindless descriptors: " + missing_bindless_features
					);
					wi::platform::Exit();
				}

				const uint32_t bindless_portable_cap = 8192u;
				uint32_t bindless_sampler_count = std::max(1u, std::min(BINDLESS_SAMPLER_CAPACITY, properties_1_2.maxDescriptorSetUpdateAfterBindSamplers));
				uint32_t bindless_storage_buffer_count = std::max(1u, std::min(BINDLESS_RESOURCE_CAPACITY, properties_1_2.maxDescriptorSetUpdateAfterBindStorageBuffers));
				uint32_t bindless_uniform_texel_count = std::max(1u, std::min(BINDLESS_RESOURCE_CAPACITY, properties_1_2.maxDescriptorSetUpdateAfterBindSampledImages / 2u));
				uint32_t bindless_sampled_image_count = std::max(1u, std::min(BINDLESS_RESOURCE_CAPACITY, properties_1_2.maxDescriptorSetUpdateAfterBindSampledImages / 2u));
				uint32_t bindless_storage_image_count = std::max(1u, std::min(BINDLESS_RESOURCE_CAPACITY, properties_1_2.maxDescriptorSetUpdateAfterBindStorageImages / 2u));
				uint32_t bindless_storage_texel_count = std::max(1u, std::min(BINDLESS_RESOURCE_CAPACITY, properties_1_2.maxDescriptorSetUpdateAfterBindStorageImages / 2u));

				bindless_sampler_count = std::min(bindless_sampler_count, bindless_portable_cap);
				bindless_storage_buffer_count = std::min(bindless_storage_buffer_count, bindless_portable_cap);
				bindless_uniform_texel_count = std::min(bindless_uniform_texel_count, bindless_portable_cap);
				bindless_sampled_image_count = std::min(bindless_sampled_image_count, bindless_portable_cap);
				bindless_storage_image_count = std::min(bindless_storage_image_count, bindless_portable_cap);
				bindless_storage_texel_count = std::min(bindless_storage_texel_count, bindless_portable_cap);

				const uint32_t bindless_counts_original[] =
				{
					bindless_sampler_count,
					bindless_storage_buffer_count,
					bindless_uniform_texel_count,
					bindless_sampled_image_count,
					bindless_storage_image_count,
					bindless_storage_texel_count,
				};

				const uint64_t bindless_budget = properties_1_2.maxUpdateAfterBindDescriptorsInAllPools;
				uint64_t bindless_total =
					uint64_t(bindless_sampler_count) +
					uint64_t(bindless_storage_buffer_count) +
					uint64_t(bindless_uniform_texel_count) +
					uint64_t(bindless_sampled_image_count) +
					uint64_t(bindless_storage_image_count) +
					uint64_t(bindless_storage_texel_count);

				if (bindless_budget > 0 && bindless_total > bindless_budget)
				{
					const auto scale_count = [&](uint32_t count) -> uint32_t
					{
						const uint64_t scaled = (uint64_t(count) * bindless_budget) / bindless_total;
						return uint32_t(std::max<uint64_t>(1u, scaled));
					};

					bindless_sampler_count = scale_count(bindless_sampler_count);
					bindless_storage_buffer_count = scale_count(bindless_storage_buffer_count);
					bindless_uniform_texel_count = scale_count(bindless_uniform_texel_count);
					bindless_sampled_image_count = scale_count(bindless_sampled_image_count);
					bindless_storage_image_count = scale_count(bindless_storage_image_count);
					bindless_storage_texel_count = scale_count(bindless_storage_texel_count);

					auto reduce_largest = [&](uint32_t* counts, size_t count)
					{
						size_t largest_index = 0;
						for (size_t i = 1; i < count; ++i)
						{
							if (counts[i] > counts[largest_index])
							{
								largest_index = i;
							}
						}
						if (counts[largest_index] > 1)
						{
							--counts[largest_index];
							return true;
						}
						return false;
					};

					uint32_t reduced_counts[] =
					{
						bindless_sampler_count,
						bindless_storage_buffer_count,
						bindless_uniform_texel_count,
						bindless_sampled_image_count,
						bindless_storage_image_count,
						bindless_storage_texel_count,
					};

					bindless_total =
						uint64_t(reduced_counts[0]) +
						uint64_t(reduced_counts[1]) +
						uint64_t(reduced_counts[2]) +
						uint64_t(reduced_counts[3]) +
						uint64_t(reduced_counts[4]) +
						uint64_t(reduced_counts[5]);

					while (bindless_total > bindless_budget && reduce_largest(reduced_counts, SDL_arraysize(reduced_counts)))
					{
						bindless_total =
							uint64_t(reduced_counts[0]) +
							uint64_t(reduced_counts[1]) +
							uint64_t(reduced_counts[2]) +
							uint64_t(reduced_counts[3]) +
							uint64_t(reduced_counts[4]) +
							uint64_t(reduced_counts[5]);
					}

					bindless_sampler_count = reduced_counts[0];
					bindless_storage_buffer_count = reduced_counts[1];
					bindless_uniform_texel_count = reduced_counts[2];
					bindless_sampled_image_count = reduced_counts[3];
					bindless_storage_image_count = reduced_counts[4];
					bindless_storage_texel_count = reduced_counts[5];

					SDL_LogWarn(
						SDL_LOG_CATEGORY_APPLICATION,
						"Vulkan bindless capacities reduced to respect maxUpdateAfterBindDescriptorsInAllPools=%llu "
						"(sampler %u->%u, storage_buffer %u->%u, uniform_texel %u->%u, sampled_image %u->%u, storage_image %u->%u, storage_texel %u->%u)",
						(unsigned long long)bindless_budget,
						bindless_counts_original[0], bindless_sampler_count,
						bindless_counts_original[1], bindless_storage_buffer_count,
						bindless_counts_original[2], bindless_uniform_texel_count,
						bindless_counts_original[3], bindless_sampled_image_count,
						bindless_counts_original[4], bindless_storage_image_count,
						bindless_counts_original[5], bindless_storage_texel_count
					);
				}

				allocationhandler->bindlessSamplers.init(this, VK_DESCRIPTOR_TYPE_SAMPLER, bindless_sampler_count);
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_SAMPLER] = allocationhandler->bindlessSamplers.descriptorSet;

				allocationhandler->bindlessStorageBuffers.init(this, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindless_storage_buffer_count);
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_STORAGE_BUFFER] = allocationhandler->bindlessStorageBuffers.descriptorSet;

				allocationhandler->bindlessUniformTexelBuffers.init(this, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, bindless_uniform_texel_count);
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_UNIFORM_TEXEL_BUFFER] = allocationhandler->bindlessUniformTexelBuffers.descriptorSet;

				allocationhandler->bindlessSampledImages.init(this, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bindless_sampled_image_count);
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_SAMPLED_IMAGE] = allocationhandler->bindlessSampledImages.descriptorSet;

				allocationhandler->bindlessStorageImages.init(this, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bindless_storage_image_count);
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_STORAGE_IMAGE] = allocationhandler->bindlessStorageImages.descriptorSet;

				allocationhandler->bindlessStorageTexelBuffers.init(this, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, bindless_storage_texel_count);
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_STORAGE_TEXEL_BUFFER] = allocationhandler->bindlessStorageTexelBuffers.descriptorSet;

			if (CheckCapability(GraphicsDeviceCapability::RAYTRACING))
			{
				allocationhandler->bindlessAccelerationStructures.init(this, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 32);
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_ACCELERATION_STRUCTURE] = allocationhandler->bindlessAccelerationStructures.descriptorSet;
			}
			else
			{
				// Unused, set dummy sampler set:
				descriptor_sets.sets[DESCRIPTOR_SET_BINDLESS_ACCELERATION_STRUCTURE] = allocationhandler->bindlessSamplers.descriptorSet;
			}
		}

#ifdef VULKAN_PIPELINE_CACHE_ENABLED
		// Pipeline Cache
		{
			// Try to read pipeline cache file if exists.
			uint8_t* pipelineData = nullptr;
			size_t pipelineDataSize = 0;
			{
				const std::string cachePath = get_shader_cache_path();
				if (FILE* file = std::fopen(cachePath.c_str(), "rb"))
				{
					if (std::fseek(file, 0, SEEK_END) == 0)
					{
						const long fileSize = std::ftell(file);
						if (fileSize > 0)
						{
							std::rewind(file);
							// Use unqualified CRT allocators so MMGR macro overrides can intercept when enabled.
							pipelineData = (uint8_t*)malloc((size_t)fileSize);
							if (pipelineData != nullptr)
							{
								pipelineDataSize = std::fread(pipelineData, 1, (size_t)fileSize, file);
								if (pipelineDataSize != (size_t)fileSize)
								{
									free(pipelineData);
									pipelineData = nullptr;
									pipelineDataSize = 0;
								}
							}
						}
					}
					std::fclose(file);
				}
			}

			// Verify cache validation.
			if (pipelineData != nullptr)
			{
				if (pipelineDataSize >= 16 + VK_UUID_SIZE)
				{
					uint32_t headerLength = 0;
					uint32_t cacheHeaderVersion = 0;
					uint32_t vendorID = 0;
					uint32_t deviceID = 0;
					uint8_t pipelineCacheUUID[VK_UUID_SIZE] = {};

					std::memcpy(&headerLength, pipelineData + 0, 4);
					std::memcpy(&cacheHeaderVersion, pipelineData + 4, 4);
					std::memcpy(&vendorID, pipelineData + 8, 4);
					std::memcpy(&deviceID, pipelineData + 12, 4);
					std::memcpy(pipelineCacheUUID, pipelineData + 16, VK_UUID_SIZE);

					bool badCache = false;

					if (headerLength <= 0)
					{
						badCache = true;
					}

					if (cacheHeaderVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
					{
						badCache = true;
					}

					if (vendorID != properties2.properties.vendorID)
					{
						badCache = true;
					}

					if (deviceID != properties2.properties.deviceID)
					{
						badCache = true;
					}

					if (std::memcmp(pipelineCacheUUID, properties2.properties.pipelineCacheUUID, sizeof(pipelineCacheUUID)) != 0)
					{
						badCache = true;
					}

					if (badCache)
					{
						// Don't submit initial cache data if any version info is incorrect
						free(pipelineData);
						pipelineData = nullptr;
						pipelineDataSize = 0;
					}
				}
				else
				{
					free(pipelineData);
					pipelineData = nullptr;
					pipelineDataSize = 0;
				}
			}

			VkPipelineCacheCreateInfo createInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
			createInfo.initialDataSize = pipelineDataSize;
			createInfo.pInitialData = pipelineData;

			// Create Vulkan pipeline cache
			vulkan_check(vkCreatePipelineCache(device, &createInfo, nullptr, &pipelineCache));
			free(pipelineData);
		}
#endif

		const Uint64 timer_end = SDL_GetPerformanceCounter();
		const Uint64 timer_freq = SDL_GetPerformanceFrequency();
		const double timer_ms = timer_freq > 0 ? (double)(timer_end - timer_begin) * 1000.0 / (double)timer_freq : 0.0;
		VULKAN_LOG("Created GraphicsDevice_Vulkan (%d ms)\nAdapter: %s", (int)std::round(timer_ms), adapterName.c_str());
	}
	GraphicsDevice_Vulkan::~GraphicsDevice_Vulkan()
	{
		vulkan_check(vkDeviceWaitIdle(device));

		for (uint32_t fr = 0; fr < BUFFERCOUNT; ++fr)
		{
			for (int queue = 0; queue < QUEUE_COUNT; ++queue)
			{
				VkFence fence = frame_fence[fr][queue];
				if (fence == VK_NULL_HANDLE)
					continue;
				vkDestroyFence(device, fence, nullptr);
				for (VkSemaphore semaphore : queues[queue].frame_semaphores[fr])
				{
					if (semaphore != VK_NULL_HANDLE)
					{
						vkDestroySemaphore(device, semaphore, nullptr);
					}
				}
			}
		}
		for (auto& queue : queues)
		{
			if (queue.timeline_semaphore != VK_NULL_HANDLE)
			{
				vkDestroySemaphore(device, queue.timeline_semaphore, nullptr);
			}
		}

		copyAllocator.destroy();

		for (auto& x : transition_handlers)
		{
			vkDestroyCommandPool(device, x.commandPool, nullptr);
			for (auto& y : x.semaphores)
			{
				vkDestroySemaphore(device, y, nullptr);
			}
		}

		for (auto& commandlist : commandlists)
		{
			for (int buffer = 0; buffer < BUFFERCOUNT; ++buffer)
			{
				for (int queue = 0; queue < QUEUE_COUNT; ++queue)
				{
					vkDestroyCommandPool(device, commandlist->commandPools[buffer][queue], nullptr);
				}
			}
			for (auto& x : commandlist->binder_pools)
			{
				x.destroy();
			}
		}

		for (auto& x : semaphore_pool)
		{
			vkDestroySemaphore(device, x, nullptr);
		}

		vmaDestroyBuffer(allocationhandler->allocator, nullBuffer, nullBufferAllocation);
		vkDestroySampler(device, nullSampler, nullptr);

		for (VkSampler sam : immutable_samplers)
		{
			vkDestroySampler(device, sam, nullptr);
		}

		if (pipelineCache != VK_NULL_HANDLE)
		{
			// Get size of pipeline cache
			size_t size{};
			vulkan_check(vkGetPipelineCacheData(device, pipelineCache, &size, nullptr));

			// Get data of pipeline cache
			uint8_t* data = nullptr;
			arrsetlen(data, size);
			vulkan_check(vkGetPipelineCacheData(device, pipelineCache, &size, data));

			// Write pipeline cache data to a file in binary format
			WriteFileBytesSDL(get_shader_cache_path(), data, size);
			arrfree(data);

			// Destroy Vulkan pipeline cache
			vkDestroyPipelineCache(device, pipelineCache, nullptr);
			pipelineCache = VK_NULL_HANDLE;
		}

		if (debugUtilsMessenger != VK_NULL_HANDLE)
		{
			vkDestroyDebugUtilsMessengerEXT(instance, debugUtilsMessenger, nullptr);
		}
	}

	bool GraphicsDevice_Vulkan::CreateSwapChain(const SwapChainDesc* desc, wi::platform::window_type window, SwapChain* swapchain) const
	{
		auto internal_state = wiGraphicsSwapChainIsValid(swapchain) ? wi::allocator::shared_ptr<SwapChain_Vulkan>(swapchain->internal_state) : wi::allocator::make_shared<SwapChain_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		internal_state->desc = *desc;
		swapchain->internal_state = internal_state;
		swapchain->desc = *desc;

		// Surface creation:
		if(internal_state->surface == VK_NULL_HANDLE)
		{
#ifdef _WIN32
			VkWin32SurfaceCreateInfoKHR createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
			createInfo.hwnd = window;
			createInfo.hinstance = GetModuleHandle(nullptr);

			vulkan_check(vkCreateWin32SurfaceKHR(instance, &createInfo, nullptr, &internal_state->surface));
#else
#if defined(SDL3) || defined(SDL2)
#if defined(SDL3)
			if (!SDL_Vulkan_CreateSurface((SDL_Window*)window, instance, nullptr, &internal_state->surface))
#else
			if (!SDL_Vulkan_CreateSurface(window, instance, &internal_state->surface))
#endif
			{
				VULKAN_LOG_ERROR("Error creating a vulkan surface with SDL_Vulkan_CreateSurface!");
				wi::platform::Exit();
			}
#else
#error WICKEDENGINE VULKAN DEVICE ERROR: PLATFORM NOT SUPPORTED
#endif
#endif // _WIN32
		}

		uint32_t presentFamily = VK_QUEUE_FAMILY_IGNORED;
		for (uint32_t familyIndex = 0; familyIndex < arrlenu(queueFamilies); ++familyIndex)
		{
			const auto& queueFamily = queueFamilies[familyIndex];
			VkBool32 presentSupport = false;
			vulkan_check(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, familyIndex, internal_state->surface, &presentSupport));

			if (presentFamily == VK_QUEUE_FAMILY_IGNORED && queueFamily.queueFamilyProperties.queueCount > 0 && presentSupport)
			{
				presentFamily = familyIndex;
				break;
			}
		}

		// Present family not found, we cannot create SwapChain
		if (presentFamily == VK_QUEUE_FAMILY_IGNORED)
		{
			return false;
		}

		bool success = CreateSwapChainInternal(internal_state.get(), physicalDevice, device, allocationhandler);
		SDL_assert(success);

		// The requested swapchain format is overridden if it wasn't supported:
		swapchain->desc.format = internal_state->desc.format;

		return success;
	}
	bool GraphicsDevice_Vulkan::CreateBuffer2(const GPUBufferDesc* desc, const std::function<void(void*)>& init_callback, GPUBuffer* buffer, const GPUResource* alias, uint64_t alias_offset) const
	{
		auto internal_state = wi::allocator::make_shared<Buffer_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		buffer->internal_state = internal_state;
		buffer->type = GPUResource::Type::BUFFER;
		buffer->mapped_data = nullptr;
		buffer->mapped_size = 0;
		buffer->desc = *desc;

		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = buffer->desc.size;
		bufferInfo.usage = 0;
		if (has_flag(buffer->desc.bind_flags, BindFlag::BIND_VERTEX_BUFFER))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		}
		if (has_flag(buffer->desc.bind_flags, BindFlag::BIND_INDEX_BUFFER))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		}
		if (has_flag(buffer->desc.bind_flags, BindFlag::BIND_CONSTANT_BUFFER))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			bufferInfo.size = align(bufferInfo.size + dynamic_cbv_maxsize, dynamic_cbv_maxsize); // hack: padding for descriptor writes!
		}
		if (has_flag(buffer->desc.bind_flags, BindFlag::BIND_SHADER_RESOURCE))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; // read only ByteAddressBuffer is also storage buffer
			bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
		}
		if (has_flag(buffer->desc.bind_flags, BindFlag::BIND_UNORDERED_ACCESS))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
		}
		if (has_flag(buffer->desc.misc_flags, ResourceMiscFlag::BUFFER_RAW))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}
		if (has_flag(buffer->desc.misc_flags, ResourceMiscFlag::BUFFER_STRUCTURED))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}
		if (has_flag(buffer->desc.misc_flags, ResourceMiscFlag::INDIRECT_ARGS))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		}
		if (has_flag(buffer->desc.misc_flags, ResourceMiscFlag::RAY_TRACING))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
			bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
			buffer->desc.alignment = std::max(buffer->desc.alignment, 16u);
			buffer->desc.alignment = std::max(buffer->desc.alignment, raytracing_properties.shaderGroupBaseAlignment);
		}
		if (has_flag(buffer->desc.misc_flags, ResourceMiscFlag::RESOURCE_MISC_PREDICATION))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
		}
		if (features_1_2.bufferDeviceAddress == VK_TRUE)
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		}
		if (has_flag(buffer->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE))
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
		}
		bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		VkVideoProfileListInfoKHR profile_list_info = {};
		profile_list_info.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
		if (has_flag(desc->misc_flags, ResourceMiscFlag::VIDEO_COMPATIBILITY_H264))
		{
			profile_list_info.pProfiles = &video_capability_h264.profile;
			profile_list_info.profileCount = 1;
			bufferInfo.pNext = &profile_list_info;
		}
		else if (has_flag(desc->misc_flags, ResourceMiscFlag::VIDEO_COMPATIBILITY_H265))
		{
			profile_list_info.pProfiles = &video_capability_h265.profile;
			profile_list_info.profileCount = 1;
			bufferInfo.pNext = &profile_list_info;
		}

		bufferInfo.flags = 0;

		if (arrlenu(families) > 1)
		{
			bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
			bufferInfo.queueFamilyIndexCount = (uint32_t)arrlenu(families);
			bufferInfo.pQueueFamilyIndices = families;
		}
		else
		{
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		if (desc->usage == Usage::READBACK)
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		}
		else if (desc->usage == Usage::UPLOAD)
		{
			bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		}

		VkResult res;

		if (has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_BUFFER) ||
			has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS) ||
			has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_RT_DS))
		{
			VkMemoryRequirements memory_requirements = {};
			memory_requirements.alignment = buffer->desc.alignment;
			if (memory_requirements.alignment == 0)
			{
				memory_requirements.alignment = GetMinOffsetAlignment(desc);
			}
			memory_requirements.size = align(desc->size, memory_requirements.alignment);
			memory_requirements.memoryTypeBits = ~0u;

			VmaAllocationCreateInfo create_info = {};
			if (desc->usage == Usage::READBACK)
			{
				create_info.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
				create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}
			else if (desc->usage == Usage::UPLOAD)
			{
				create_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
				create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}
			else
			{
				create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			}

			create_info.flags |= VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT;

			if (has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS) ||
				has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_RT_DS))
			{
				create_info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // workaround for potential image offset issue (varying formats, varying offset requirements)
			}

			VmaAllocationInfo allocation_info = {};

			res = vulkan_check(vmaAllocateMemory(
				allocationhandler->allocator,
				&memory_requirements,
				&create_info,
				&internal_state->allocation,
				&allocation_info
			));

			res = vulkan_check(vkCreateBuffer(
				device,
				&bufferInfo,
				nullptr,
				&internal_state->resource
			));

			res = vulkan_check(vkBindBufferMemory(
				device,
				internal_state->resource,
				internal_state->allocation->GetMemory(),
				internal_state->allocation->GetOffset()
			));
		}
		else if (has_flag(desc->misc_flags, ResourceMiscFlag::SPARSE))
		{
			SDL_assert(CheckCapability(GraphicsDeviceCapability::SPARSE_BUFFER));
			bufferInfo.flags |= VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
			bufferInfo.flags |= VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
			bufferInfo.flags |= VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;

			res = vulkan_check(vkCreateBuffer(device, &bufferInfo, nullptr, &internal_state->resource));

			VkMemoryRequirements memory_requirements = {};
			vkGetBufferMemoryRequirements(
				device,
				internal_state->resource,
				&memory_requirements
			);
			buffer->sparse_page_size = (uint32_t)memory_requirements.alignment;
		}
		else
		{
			VmaAllocationCreateInfo create_info = {};
			if (desc->usage == Usage::READBACK)
			{
				create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
				create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}
			else if (desc->usage == Usage::UPLOAD)
			{
				create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
				create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}
			else
			{
				create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			}

			if (desc->usage == Usage::DEFAULT && (bufferInfo.usage & VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR))
			{
				// NVidia video bitstream buffer workaround: DEFAULT usage resource which is not mapped producing incorrect result
				create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}

			if (alias == nullptr)
			{
				if (buffer->desc.alignment > 0)
				{
					res = vulkan_check(vmaCreateBufferWithAlignment(
						allocationhandler->allocator,
						&bufferInfo,
						&create_info,
						buffer->desc.alignment,
						&internal_state->resource,
						&internal_state->allocation,
						nullptr
					));
				}
				else
				{
					res = vulkan_check(vmaCreateBuffer(
						allocationhandler->allocator,
						&bufferInfo,
						&create_info,
						&internal_state->resource,
						&internal_state->allocation,
						nullptr
					));
				}
			}
			else
			{
				// Aliasing: https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/resource_aliasing.html
				if (wiGraphicsGPUResourceIsTexture(alias))
				{
					auto alias_internal = to_internal<Texture>(alias);
					res = vulkan_check(vmaCreateAliasingBuffer2(
						allocationhandler->allocator,
						alias_internal->allocation,
						alias_offset,
						&bufferInfo,
						&internal_state->resource
					));
				}
				else
				{
					auto alias_internal = to_internal<GPUBuffer>(alias);
					res = vulkan_check(vmaCreateAliasingBuffer2(
						allocationhandler->allocator,
						alias_internal->allocation,
						alias_offset,
						&bufferInfo,
						&internal_state->resource
					));
				}
			}
		}

		if (desc->usage == Usage::READBACK || desc->usage == Usage::UPLOAD)
		{
			if (alias == nullptr)
			{
				buffer->mapped_data = internal_state->allocation->GetMappedData();
				buffer->mapped_size = internal_state->allocation->GetSize();
			}
			else
			{
				VULKAN_ASSERT_MSG(alias->mapped_data != nullptr, "Aliased buffer created with mapping request, but the aliasing storage resource was not mapped!");
				buffer->mapped_data = (uint8_t*)alias->mapped_data + alias_offset;
				buffer->mapped_size = desc->size;
			}
		}

		if (bufferInfo.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		{
			VkBufferDeviceAddressInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
			info.buffer = internal_state->resource;
			internal_state->address = vkGetBufferDeviceAddress(device, &info);
		}

		// Issue data copy on request:
		if (init_callback != nullptr)
		{
			CopyAllocator::CopyCMD cmd;
			void* mapped_data = nullptr;
			if (desc->usage == Usage::UPLOAD)
			{
				mapped_data = buffer->mapped_data;
			}
			else
			{
				cmd = copyAllocator.allocate(desc->size, QUEUE_COPY);
				mapped_data = cmd.uploadbuffer.mapped_data;
			}

			init_callback(mapped_data);

			if(cmd.IsValid())
			{
				VkBufferCopy copyRegion = {};
				copyRegion.size = buffer->desc.size;
				copyRegion.srcOffset = 0;
				copyRegion.dstOffset = 0;

				vkCmdCopyBuffer(
					cmd.transferCommandBuffer,
					to_internal(&cmd.uploadbuffer)->resource,
					internal_state->resource,
					1,
					&copyRegion
				);

				SubmissionToken token = copyAllocator.submit(std::move(cmd), QUEUE_COPY);
				if (token.IsValid())
				{
					std::scoped_lock lock(upload_token_locker);
					pending_implicit_uploads.Merge(token);
				}
			}
		}

		// Create resource views if needed
		if (!has_flag(desc->misc_flags, ResourceMiscFlag::NO_DEFAULT_DESCRIPTORS))
		{
			if (has_flag(desc->bind_flags, BindFlag::BIND_SHADER_RESOURCE))
			{
				CreateSubresource(buffer, SubresourceType::SRV, 0);
			}
			if (has_flag(desc->bind_flags, BindFlag::BIND_UNORDERED_ACCESS))
			{
				CreateSubresource(buffer, SubresourceType::UAV, 0);
			}
		}

		return res == VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreateTexture(const TextureDesc* desc, const SubresourceData* initial_data, Texture* texture, const GPUResource* alias, uint64_t alias_offset) const
	{
		auto internal_state = wi::allocator::make_shared<Texture_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		internal_state->defaultLayout = _ConvertImageLayout(desc->layout);
		texture->internal_state = internal_state;
		texture->type = GPUResource::Type::TEXTURE;
		texture->mapped_data = nullptr;
		texture->mapped_size = 0;
		texture->mapped_subresources = nullptr;
		texture->mapped_subresource_count = 0;
		texture->sparse_properties = nullptr;
		texture->desc = *desc;

		texture->desc.mip_levels = GetMipCount(texture->desc);

		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.extent.width = texture->desc.width;
		imageInfo.extent.height = texture->desc.height;
		imageInfo.extent.depth = texture->desc.depth;
		imageInfo.format = _ConvertFormat(texture->desc.format);
		imageInfo.arrayLayers = texture->desc.array_size;
		imageInfo.mipLevels = texture->desc.mip_levels;
		imageInfo.samples = (VkSampleCountFlagBits)texture->desc.sample_count;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = 0;
		if (has_flag(texture->desc.bind_flags, BindFlag::BIND_SHADER_RESOURCE))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if (has_flag(texture->desc.bind_flags, BindFlag::BIND_UNORDERED_ACCESS))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

			if (IsFormatSRGB(texture->desc.format))
			{
				imageInfo.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
			}
		}
		if (has_flag(texture->desc.bind_flags, BindFlag::RENDER_TARGET))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}
		if (has_flag(texture->desc.bind_flags, BindFlag::DEPTH_STENCIL))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		if (has_flag(texture->desc.bind_flags, BindFlag::SHADING_RATE))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
		}
		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::TRANSIENT_ATTACHMENT))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
		}
		else
		{
			imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}

		imageInfo.flags = 0;
		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::TEXTURECUBE))
		{
			imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		}
		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::TYPED_FORMAT_CASTING) || has_flag(texture->desc.misc_flags, ResourceMiscFlag::TYPELESS_FORMAT_CASTING))
		{
			imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		}

		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
			imageInfo.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
			imageInfo.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
		}
		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE_OUTPUT_ONLY))
		{
			imageInfo.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
		}
		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE_DPB_ONLY))
		{
			imageInfo.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR; // Note: this is not a combination of flags, but complete assignment!
		}

		if (desc->format == Format::NV12 && has_flag(texture->desc.bind_flags, BindFlag::BIND_SHADER_RESOURCE))
		{
			imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		}

		VkVideoProfileListInfoKHR profile_list_info = {};
		profile_list_info.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
		if (has_flag(desc->misc_flags, ResourceMiscFlag::VIDEO_COMPATIBILITY_H264))
		{
			profile_list_info.pProfiles = &video_capability_h264.profile;
			profile_list_info.profileCount = 1;
			imageInfo.pNext = &profile_list_info;
		}
		else if (has_flag(desc->misc_flags, ResourceMiscFlag::VIDEO_COMPATIBILITY_H265))
		{
			profile_list_info.pProfiles = &video_capability_h265.profile;
			profile_list_info.profileCount = 1;
			imageInfo.pNext = &profile_list_info;
		}

#if 0
		// Doesn't seem to be needed right now:
		if (profile_list_info.profileCount > 0)
		{
			VkPhysicalDeviceVideoFormatInfoKHR video_format_info = {};
			video_format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
			video_format_info.imageUsage = imageInfo.usage;
			video_format_info.pNext = &profile_list_info;
			uint32_t format_property_count = 0;
			vulkan_check(vkGetPhysicalDeviceVideoFormatPropertiesKHR(physicalDevice, &video_format_info, &format_property_count, nullptr));

			VkVideoFormatPropertiesKHR* video_format_properties = nullptr;
			arrsetlen(video_format_properties, format_property_count);
			for (uint32_t i = 0; i < format_property_count; ++i)
			{
				video_format_properties[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
			}
			vulkan_check(vkGetPhysicalDeviceVideoFormatPropertiesKHR(physicalDevice, &video_format_info, &format_property_count, video_format_properties));

			SDL_assert(imageInfo.flags == 0 || (arrlenu(video_format_properties) > 0 && video_format_properties[0].imageCreateFlags & imageInfo.flags));
			SDL_assert(arrlenu(video_format_properties) > 0 && video_format_properties[0].imageUsageFlags & imageInfo.usage);
		}
#endif

		if (arrlenu(families) > 1)
		{
			imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
			imageInfo.queueFamilyIndexCount = (uint32_t)arrlenu(families);
			imageInfo.pQueueFamilyIndices = families;
		}
		else
		{
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		switch (texture->desc.type)
		{
		case TextureDesc::Type::TEXTURE_1D:
			imageInfo.imageType = VK_IMAGE_TYPE_1D;
			break;
		case TextureDesc::Type::TEXTURE_2D:
			imageInfo.imageType = VK_IMAGE_TYPE_2D;
			break;
		case TextureDesc::Type::TEXTURE_3D:
			imageInfo.imageType = VK_IMAGE_TYPE_3D;
			break;
		default:
			SDL_assert(0);
			break;
		}

		VkResult res = VK_SUCCESS;

		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::SPARSE))
		{
			SDL_assert(CheckCapability(GraphicsDeviceCapability::SPARSE_TEXTURE2D) || imageInfo.imageType != VK_IMAGE_TYPE_2D);
			SDL_assert(CheckCapability(GraphicsDeviceCapability::SPARSE_TEXTURE3D) || imageInfo.imageType != VK_IMAGE_TYPE_3D);
			imageInfo.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
			imageInfo.flags |= VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
			imageInfo.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;

			vulkan_check(vkCreateImage(device, &imageInfo, nullptr, &internal_state->resource));

			VkMemoryRequirements memory_requirements = {};
			// no check needed, returns void
			vkGetImageMemoryRequirements(
				device,
				internal_state->resource,
				&memory_requirements
			);
			texture->sparse_page_size = (uint32_t)memory_requirements.alignment;

			uint32_t sparse_requirement_count = 0;

			vkGetImageSparseMemoryRequirements(
				device,
				internal_state->resource,
				&sparse_requirement_count,
				nullptr
			);

			VkSparseImageMemoryRequirements* sparse_requirements = nullptr;
			arrsetlen(sparse_requirements, sparse_requirement_count);
			texture->sparse_properties = &internal_state->sparse_texture_properties;

			vkGetImageSparseMemoryRequirements(
				device,
				internal_state->resource,
				&sparse_requirement_count,
				sparse_requirements
			);

			SparseTextureProperties& out_sparse = internal_state->sparse_texture_properties;
			out_sparse.total_tile_count = uint32_t(memory_requirements.size / memory_requirements.alignment);

			for (size_t i = 0; i < arrlenu(sparse_requirements); ++i)
			{
				const VkSparseImageMemoryRequirements& in_sparse = sparse_requirements[i];
				if (i == 0)
				{
					// These should be common for all subresources right? Like in DX12?
					out_sparse.tile_width = in_sparse.formatProperties.imageGranularity.width;
					out_sparse.tile_height = in_sparse.formatProperties.imageGranularity.height;
					out_sparse.tile_depth = in_sparse.formatProperties.imageGranularity.depth;
					out_sparse.packed_mip_start = in_sparse.imageMipTailFirstLod;
					out_sparse.packed_mip_count = texture->desc.mip_levels - in_sparse.imageMipTailFirstLod;
					out_sparse.packed_mip_tile_offset = uint32_t(in_sparse.imageMipTailOffset / memory_requirements.alignment);
					out_sparse.packed_mip_tile_count = uint32_t(in_sparse.imageMipTailSize / memory_requirements.alignment);
				}
			}

		}
		else
		{
			VmaAllocationCreateInfo allocInfo = {};
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

			if (has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_BUFFER) ||
				has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS) ||
				has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_RT_DS))
			{
				allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // workaround for potential image offset issue (varying formats, varying offset requirements)
				allocInfo.flags |= VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT;
			}

			if (texture->desc.usage == Usage::READBACK || texture->desc.usage == Usage::UPLOAD)
			{
				// Note: we are creating a buffer instead of linear image because linear image cannot have mips
				//	With a buffer, we can tightly pack mips linearly into a buffer so it won't have that limitation
				VkBufferCreateInfo bufferInfo = {};
				bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
				bufferInfo.size = ComputeTextureMemorySizeInBytes(*desc);

				allocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
				if (texture->desc.usage == Usage::READBACK)
				{
					allocInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
					bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
				}
				else if (texture->desc.usage == Usage::UPLOAD)
				{
					allocInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
					bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				}

				res = vulkan_check(vmaCreateBuffer(allocationhandler->allocator, &bufferInfo, &allocInfo, &internal_state->staging_resource, &internal_state->allocation, nullptr));

				if (texture->desc.usage == Usage::READBACK || texture->desc.usage == Usage::UPLOAD)
				{
					texture->mapped_data = internal_state->allocation->GetMappedData();
					texture->mapped_size = internal_state->allocation->GetSize();

					CreateTextureSubresourceDatasRaw(*desc, texture->mapped_data, internal_state->mapped_subresources, (uint32_t)properties2.properties.limits.optimalBufferCopyRowPitchAlignment);
					texture->mapped_subresources = internal_state->mapped_subresources;
					texture->mapped_subresource_count = (uint32_t)arrlenu(internal_state->mapped_subresources);
				}
			}
			else
			{
				VmaAllocator allocator = allocationhandler->allocator;
				VkExternalMemoryImageCreateInfo externalMemImageCreateInfo = {};

				if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::SHARED))
				{
					externalMemImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
#if WICKED_VULKAN_PLATFORM_WIN32
					// TODO: Expose this? should we use VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT?
					externalMemImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
					externalMemImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
					imageInfo.pNext = &externalMemImageCreateInfo;

					// We have to use a dedicated allocator for external handles that has been created with VkExportMemoryAllocateInfo
					allocator = allocationhandler->externalAllocator;

					allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
				}

				if (alias == nullptr)
				{
					res = vulkan_check(vmaCreateImage(
						allocator,
						&imageInfo,
						&allocInfo,
						&internal_state->resource,
						&internal_state->allocation,
						nullptr
					));
				}
				else
				{
					// Aliasing: https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/resource_aliasing.html
					if (wiGraphicsGPUResourceIsTexture(alias))
					{
						auto alias_internal = to_internal<Texture>(alias);
						res = vulkan_check(vmaCreateAliasingImage2(
							allocator,
							alias_internal->allocation,
							alias_offset,
							&imageInfo,
							&internal_state->resource
						));
					}
					else
					{
						auto alias_internal = to_internal<GPUBuffer>(alias);
						res = vulkan_check(vmaCreateAliasingImage2(
							allocator,
							alias_internal->allocation,
							alias_offset,
							&imageInfo,
							&internal_state->resource
						));
					}
				}

				if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::SHARED))
				{
					VmaAllocationInfo allocationInfo;
					vmaGetAllocationInfo(allocator, internal_state->allocation, &allocationInfo);

#if WICKED_VULKAN_PLATFORM_WIN32
					VkMemoryGetWin32HandleInfoKHR getWin32HandleInfoKHR = {};
					getWin32HandleInfoKHR.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
					getWin32HandleInfoKHR.pNext = nullptr;
					getWin32HandleInfoKHR.memory = allocationInfo.deviceMemory;
					getWin32HandleInfoKHR.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR;
					res = vulkan_check(vkGetMemoryWin32HandleKHR(allocationhandler->device, &getWin32HandleInfoKHR, &texture->shared_handle));
#elif WICKED_VULKAN_PLATFORM_ANDROID || WICKED_VULKAN_PLATFORM_LINUX || WICKED_VULKAN_PLATFORM_BSD
					VkMemoryGetFdInfoKHR memoryGetFdInfoKHR = {};
					memoryGetFdInfoKHR.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
					memoryGetFdInfoKHR.pNext = nullptr;
					memoryGetFdInfoKHR.memory = allocationInfo.deviceMemory;
					memoryGetFdInfoKHR.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
					res = vulkan_check(vkGetMemoryFdKHR(allocationhandler->device, &memoryGetFdInfoKHR, &texture->shared_handle));
#endif
				}
			}
		}

		// Issue data copy on request:
		if (initial_data != nullptr)
		{
			CopyAllocator::CopyCMD cmd;
			void* mapped_data = nullptr;
			if (desc->usage == Usage::UPLOAD)
			{
				mapped_data = texture->mapped_data;
			}
			else
			{
				cmd = copyAllocator.allocate(internal_state->allocation->GetSize(), QUEUE_COPY);
				mapped_data = cmd.uploadbuffer.mapped_data;
			}

			VkBufferImageCopy* copyRegions = nullptr;

			const uint32_t data_stride = GetFormatStride(desc->format);
			const uint32_t block_size = GetFormatBlockSize(desc->format);

			VkDeviceSize copyOffset = 0;
			uint32_t initDataIdx = 0;
			for (uint32_t layer = 0; layer < desc->array_size; ++layer)
			{
				uint32_t width = imageInfo.extent.width;
				uint32_t height = imageInfo.extent.height;
				uint32_t depth = imageInfo.extent.depth;
				for (uint32_t mip = 0; mip < texture->desc.mip_levels; ++mip)
				{
					const SubresourceData& subresourceData = initial_data[initDataIdx++];
					const uint32_t num_blocks_x = std::max(1u, width / block_size);
					const uint32_t num_blocks_y = std::max(1u, height / block_size);
					const uint32_t dst_rowpitch = num_blocks_x * data_stride;
					const uint32_t dst_slicepitch = dst_rowpitch * num_blocks_y;
					const uint32_t src_rowpitch = subresourceData.row_pitch;
					const uint32_t src_slicepitch = subresourceData.slice_pitch;
					for (uint32_t z = 0; z < depth; ++z)
					{
						uint8_t* dst_slice = (uint8_t*)mapped_data + copyOffset + dst_slicepitch * z;
						uint8_t* src_slice = (uint8_t*)subresourceData.data_ptr + src_slicepitch * z;
						for (uint32_t y = 0; y < num_blocks_y; ++y)
						{
							std::memcpy(
								dst_slice + dst_rowpitch * y,
								src_slice + src_rowpitch * y,
								dst_rowpitch
							);
						}
					}

					if (cmd.IsValid())
					{
						VkBufferImageCopy copyRegion = {};
						copyRegion.bufferOffset = copyOffset;
						copyRegion.bufferRowLength = 0;
						copyRegion.bufferImageHeight = 0;

						copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
						copyRegion.imageSubresource.mipLevel = mip;
						copyRegion.imageSubresource.baseArrayLayer = layer;
						copyRegion.imageSubresource.layerCount = 1;

						copyRegion.imageOffset = { 0, 0, 0 };
						copyRegion.imageExtent = {
							width,
							height,
							depth
						};

						arrput(copyRegions, copyRegion);
					}

					copyOffset += dst_slicepitch * depth;
					copyOffset = align(copyOffset, VkDeviceSize(4)); // fix for validation: on transfer queue the srcOffset must be 4-byte aligned

					width = std::max(1u, width / 2);
					height = std::max(1u, height / 2);
					depth = std::max(1u, depth / 2);
				}
			}

			if(cmd.IsValid())
			{
				VkImageMemoryBarrier2 barrier = {};
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrier.image = internal_state->resource;
				barrier.oldLayout = imageInfo.initialLayout;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				barrier.srcAccessMask = 0;
				barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
				barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				barrier.subresourceRange.baseArrayLayer = 0;
				barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
				barrier.subresourceRange.baseMipLevel = 0;
				barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

				VkDependencyInfo dependencyInfo = {};
				dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
				dependencyInfo.imageMemoryBarrierCount = 1;
				dependencyInfo.pImageMemoryBarriers = &barrier;

				vkCmdPipelineBarrier2(cmd.transferCommandBuffer, &dependencyInfo);

				vkCmdCopyBufferToImage(
					cmd.transferCommandBuffer,
					to_internal(&cmd.uploadbuffer)->resource,
					internal_state->resource,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					(uint32_t)arrlenu(copyRegions),
					copyRegions
				);

				// Keep layout transition in the upload command stream for both timeline and no-timeline paths.
				std::swap(barrier.srcStageMask, barrier.dstStageMask);
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.newLayout = _ConvertImageLayout(texture->desc.layout);
				barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = _ParseResourceState(texture->desc.layout);
				VkDependencyInfo final_dependency = {};
				final_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
				final_dependency.imageMemoryBarrierCount = 1;
				final_dependency.pImageMemoryBarriers = &barrier;
				vkCmdPipelineBarrier2(cmd.transferCommandBuffer, &final_dependency);

				SubmissionToken token = copyAllocator.submit(std::move(cmd), QUEUE_COPY);
				if (token.IsValid())
				{
					std::scoped_lock lock(upload_token_locker);
					pending_implicit_uploads.Merge(token);
				}
			}
		}
		else if(texture->desc.layout != ResourceState::UNDEFINED && internal_state->resource != VK_NULL_HANDLE)
		{
			VkImageMemoryBarrier2 barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.image = internal_state->resource;
			barrier.oldLayout = imageInfo.initialLayout;
			barrier.newLayout = _ConvertImageLayout(texture->desc.layout);
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask = 0;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			barrier.dstAccessMask = _ParseResourceState(texture->desc.layout);
			if (IsFormatDepthSupport(texture->desc.format))
			{
				barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
				if (IsFormatStencilSupport(texture->desc.format))
				{
					barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
				}
			}
			else
			{
				barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			}
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = imageInfo.arrayLayers;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = imageInfo.mipLevels;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

			std::scoped_lock lck(transitionLocker);
			arrput(init_transitions, barrier);
		}

		if (!has_flag(desc->misc_flags, ResourceMiscFlag::NO_DEFAULT_DESCRIPTORS))
		{
			if (has_flag(texture->desc.bind_flags, BindFlag::RENDER_TARGET))
			{
				CreateSubresource(texture, SubresourceType::RTV, 0, -1, 0, -1);
			}
			if (has_flag(texture->desc.bind_flags, BindFlag::DEPTH_STENCIL))
			{
				CreateSubresource(texture, SubresourceType::DSV, 0, -1, 0, -1);
			}
			if (has_flag(texture->desc.bind_flags, BindFlag::BIND_SHADER_RESOURCE))
			{
				CreateSubresource(texture, SubresourceType::SRV, 0, -1, 0, -1);
			}
			if (has_flag(texture->desc.bind_flags, BindFlag::BIND_UNORDERED_ACCESS))
			{
				CreateSubresource(texture, SubresourceType::UAV, 0, -1, 0, -1);
			}
		}

		if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE) || has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE_OUTPUT_ONLY) || has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE_DPB_ONLY))
		{
			VkImageViewCreateInfo view_desc = {};
			view_desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			view_desc.flags = 0;
			view_desc.image = internal_state->resource;
			view_desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			view_desc.subresourceRange.baseArrayLayer = 0;
			view_desc.subresourceRange.layerCount = imageInfo.arrayLayers;
			view_desc.subresourceRange.baseMipLevel = 0;
			view_desc.subresourceRange.levelCount = imageInfo.mipLevels;
			view_desc.format = imageInfo.format;
			view_desc.viewType = imageInfo.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

			VkImageViewUsageCreateInfo viewUsageInfo = {};
			viewUsageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
			if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE_DPB_ONLY))
			{
				viewUsageInfo.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
			}
			else if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::VIDEO_DECODE_OUTPUT_ONLY))
			{
				viewUsageInfo.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
			}
			else
			{
				viewUsageInfo.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
			}
			view_desc.pNext = &viewUsageInfo;

			res = vulkan_check(vkCreateImageView(device, &view_desc, nullptr, &internal_state->video_decode_view));
		}

		return res >= VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreateShader(ShaderStage stage, const void* shadercode, size_t shadercode_size, Shader* shader, const char* entrypoint) const
	{
		auto internal_state = wi::allocator::make_shared<Shader_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		internal_state->entrypoint = entrypoint;
		shader->internal_state = internal_state;
		shader->stage = stage;

		VkResult res = VK_SUCCESS;

		VkShaderModuleCreateInfo moduleInfo = {};
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleInfo.codeSize = shadercode_size;
		moduleInfo.pCode = (const uint32_t*)shadercode;
		res = vulkan_check(vkCreateShaderModule(device, &moduleInfo, nullptr, &internal_state->shaderModule));

		internal_state->stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		internal_state->stageInfo.module = internal_state->shaderModule;
		internal_state->stageInfo.pName = internal_state->entrypoint.c_str();
		switch (stage)
		{
		case ShaderStage::MS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
			break;
		case ShaderStage::AS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
			break;
		case ShaderStage::VS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
			break;
		case ShaderStage::HS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
			break;
		case ShaderStage::DS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
			break;
		case ShaderStage::GS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
			break;
		case ShaderStage::PS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			break;
		case ShaderStage::CS:
			internal_state->stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			break;
		default:
			// also means library shader (ray tracing)
			internal_state->stageInfo.stage = VK_SHADER_STAGE_ALL;
			break;
		}

		internal_state->layout = layout_template;

		// Right now reflection must be used to find exact binding types for SRV and UAV:
		{
			SpvReflectShaderModule module;
			SpvReflectResult result = spvReflectCreateShaderModule(moduleInfo.codeSize, moduleInfo.pCode, &module);
			SDL_assert(result == SPV_REFLECT_RESULT_SUCCESS);

			uint32_t binding_count = 0;
			result = spvReflectEnumerateDescriptorBindings(&module, &binding_count, nullptr);
			SDL_assert(result == SPV_REFLECT_RESULT_SUCCESS);

			SpvReflectDescriptorBinding* bindings[DESCRIPTORBINDER_ALL_COUNT] = {};
			SDL_assert(binding_count < SDL_arraysize(bindings));
			result = spvReflectEnumerateDescriptorBindings(&module, &binding_count, bindings);
			SDL_assert(result == SPV_REFLECT_RESULT_SUCCESS);

			for (uint32_t i = 0; i < binding_count; ++i)
			{
				if (bindings[i] == nullptr)
					continue;
				const auto& refl = *bindings[i];
				if (refl.accessed == false)
					continue;
				if (refl.set > 0)
					continue; // don't care about bindless here
				if (refl.resource_type != SPV_REFLECT_RESOURCE_FLAG_SRV && refl.resource_type != SPV_REFLECT_RESOURCE_FLAG_UAV)
					continue; // don't care about anything other than SRV and UAV here, we reduce descriptor sets by hashing only based on SRV and UAV types

				VkDescriptorSetLayoutBinding binding = {};
				binding.binding = refl.binding;
				binding.descriptorCount = refl.count;
				binding.descriptorType = (VkDescriptorType)refl.descriptor_type;
				binding.stageFlags = VK_SHADER_STAGE_ALL;

				VkImageViewType view_type = {};
				switch (refl.descriptor_type)
				{
				default:
					break;
				case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
				case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
					switch (refl.image.dim)
					{
					default:
					case SpvDim1D:
						if (refl.image.arrayed == 0)
						{
							view_type = VK_IMAGE_VIEW_TYPE_1D;
						}
						else
						{
							view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
						}
						break;
					case SpvDim2D:
						if (refl.image.arrayed == 0)
						{
							view_type = VK_IMAGE_VIEW_TYPE_2D;
						}
						else
						{
							view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
						}
						break;
					case SpvDim3D:
						view_type = VK_IMAGE_VIEW_TYPE_3D;
						break;
					case SpvDimCube:
						if (refl.image.arrayed == 0)
						{
							view_type = VK_IMAGE_VIEW_TYPE_CUBE;
						}
						else
						{
							view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
						}
						break;
					}
					break;
				}

				switch (refl.resource_type)
				{
				case SPV_REFLECT_RESOURCE_FLAG_SRV:
				{
					const uint32_t srv_slot = binding.binding - VULKAN_BINDING_SHIFT_T;
					internal_state->layout.table.SRV[srv_slot] = binding;
					internal_state->layout.SRV_image_types[srv_slot] = view_type;
				}
				break;
				case SPV_REFLECT_RESOURCE_FLAG_UAV:
				{
					const uint32_t uav_slot = binding.binding - VULKAN_BINDING_SHIFT_U;
					internal_state->layout.table.UAV[uav_slot] = binding;
					internal_state->layout.UAV_image_types[uav_slot] = view_type;
				}
				break;
				default:
					SDL_assert(0);
					break;
				}
			}

			spvReflectDestroyShaderModule(&module);
		}

		if (stage == ShaderStage::CS)
		{
			VkComputePipelineCreateInfo pipelineInfo = {};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			pipelineInfo.layout = cache_pso_layout(internal_state->layout);
			pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

			// Create compute pipeline state in place:
			pipelineInfo.stage = internal_state->stageInfo;

			res = vulkan_check(vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &internal_state->pipeline_cs));
		}
		else if (stage == ShaderStage::LIB)
		{
			cache_pso_layout(internal_state->layout);
		}

		return res == VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreateSampler(const SamplerDesc* desc, Sampler* sampler) const
	{
		auto internal_state = wi::allocator::make_shared<Sampler_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		sampler->internal_state = internal_state;
		sampler->desc = *desc;

		VkSamplerCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		createInfo.flags = 0;
		createInfo.pNext = nullptr;

		switch (desc->filter)
		{
		case Filter::MIN_MAG_MIP_POINT:
		case Filter::MINIMUM_MIN_MAG_MIP_POINT:
		case Filter::MAXIMUM_MIN_MAG_MIP_POINT:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::MIN_MAG_POINT_MIP_LINEAR:
		case Filter::MINIMUM_MIN_MAG_POINT_MIP_LINEAR:
		case Filter::MAXIMUM_MIN_MAG_POINT_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::MIN_POINT_MAG_LINEAR_MIP_POINT:
		case Filter::MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
		case Filter::MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::MIN_POINT_MAG_MIP_LINEAR:
		case Filter::MINIMUM_MIN_POINT_MAG_MIP_LINEAR:
		case Filter::MAXIMUM_MIN_POINT_MAG_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::MIN_LINEAR_MAG_MIP_POINT:
		case Filter::MINIMUM_MIN_LINEAR_MAG_MIP_POINT:
		case Filter::MAXIMUM_MIN_LINEAR_MAG_MIP_POINT:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::MIN_LINEAR_MAG_POINT_MIP_LINEAR:
		case Filter::MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
		case Filter::MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::MIN_MAG_LINEAR_MIP_POINT:
		case Filter::MINIMUM_MIN_MAG_LINEAR_MIP_POINT:
		case Filter::MAXIMUM_MIN_MAG_LINEAR_MIP_POINT:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::MIN_MAG_MIP_LINEAR:
		case Filter::MINIMUM_MIN_MAG_MIP_LINEAR:
		case Filter::MAXIMUM_MIN_MAG_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		case Filter::ANISOTROPIC:
		case Filter::MINIMUM_ANISOTROPIC:
		case Filter::MAXIMUM_ANISOTROPIC:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = true;
			createInfo.maxAnisotropy = std::min(16.0f, std::max(1.0f, static_cast<float>(desc->max_anisotropy)));
			createInfo.compareEnable = false;
			break;
		case Filter::COMPARISON_MIN_MAG_MIP_POINT:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_MIN_MAG_POINT_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_MIN_POINT_MAG_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_MIN_LINEAR_MAG_MIP_POINT:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_MIN_MAG_LINEAR_MIP_POINT:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_MIN_MAG_MIP_LINEAR:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = true;
			break;
		case Filter::COMPARISON_ANISOTROPIC:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			createInfo.anisotropyEnable = true;
			createInfo.maxAnisotropy = std::min(16.0f, std::max(1.0f, static_cast<float>(desc->max_anisotropy)));
			createInfo.compareEnable = true;
			break;
		default:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			createInfo.anisotropyEnable = false;
			createInfo.compareEnable = false;
			break;
		}

		VkSamplerReductionModeCreateInfo reductionmode = {};
		reductionmode.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
		if (CheckCapability(GraphicsDeviceCapability::SAMPLER_MINMAX))
		{
			switch (desc->filter)
			{
			case Filter::MINIMUM_MIN_MAG_MIP_POINT:
			case Filter::MINIMUM_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MINIMUM_MIN_POINT_MAG_MIP_LINEAR:
			case Filter::MINIMUM_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
			case Filter::MINIMUM_MIN_MAG_LINEAR_MIP_POINT:
			case Filter::MINIMUM_MIN_MAG_MIP_LINEAR:
			case Filter::MINIMUM_ANISOTROPIC:
				reductionmode.reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN;
				createInfo.pNext = &reductionmode;
				break;
			case Filter::MAXIMUM_MIN_MAG_MIP_POINT:
			case Filter::MAXIMUM_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MAXIMUM_MIN_POINT_MAG_MIP_LINEAR:
			case Filter::MAXIMUM_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
			case Filter::MAXIMUM_MIN_MAG_LINEAR_MIP_POINT:
			case Filter::MAXIMUM_MIN_MAG_MIP_LINEAR:
			case Filter::MAXIMUM_ANISOTROPIC:
				reductionmode.reductionMode = VK_SAMPLER_REDUCTION_MODE_MAX;
				createInfo.pNext = &reductionmode;
				break;
			default:
				break;
			}
		}

		createInfo.addressModeU = _ConvertTextureAddressMode(desc->address_u, features_1_2);
		createInfo.addressModeV = _ConvertTextureAddressMode(desc->address_v, features_1_2);
		createInfo.addressModeW = _ConvertTextureAddressMode(desc->address_w, features_1_2);
		createInfo.compareOp = _ConvertComparisonFunc(desc->comparison_func);
		createInfo.minLod = desc->min_lod;
		createInfo.maxLod = desc->max_lod;
		createInfo.mipLodBias = desc->mip_lod_bias;
		createInfo.borderColor = _ConvertSamplerBorderColor(desc->border_color);
		createInfo.unnormalizedCoordinates = VK_FALSE;

		VkResult res = vulkan_check(vkCreateSampler(device, &createInfo, nullptr, &internal_state->resource));

		internal_state->index = allocationhandler->bindlessSamplers.allocate();
		if (internal_state->index >= 0)
		{
			VkDescriptorImageInfo imageInfo = {};
			imageInfo.sampler = internal_state->resource;
			VkWriteDescriptorSet write = {};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			write.dstBinding = 0;
			write.dstArrayElement = internal_state->index;
			write.descriptorCount = 1;
			write.dstSet = allocationhandler->bindlessSamplers.descriptorSet;
			write.pImageInfo = &imageInfo;
			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		}
		else
		{
			SDL_assert(0);
		}

		return res == VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreateQueryHeap(const GPUQueryHeapDesc* desc, GPUQueryHeap* queryheap) const
	{
		auto internal_state = wi::allocator::make_shared<QueryHeap_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		queryheap->internal_state = internal_state;
		queryheap->desc = *desc;

		VkQueryPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		poolInfo.queryCount = desc->query_count;

		switch (desc->type)
		{
		case GpuQueryType::TIMESTAMP:
			poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			break;
		case GpuQueryType::OCCLUSION:
		case GpuQueryType::OCCLUSION_BINARY:
			poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
			break;
		}

		VkResult res = vulkan_check(vkCreateQueryPool(device, &poolInfo, nullptr, &internal_state->pool));

		return res == VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreatePipelineState(const PipelineStateDesc* desc, PipelineState* pso, const RenderPassInfo* renderpass_info) const
	{
		auto internal_state = wi::allocator::make_shared<PipelineState_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		pso->internal_state = internal_state;
		pso->desc = *desc;

		RasterizerState rs_default = {};
		DepthStencilState dss_default = {};
		BlendState bs_default = {};
		InitRasterizerState(rs_default);
		InitDepthStencilState(dss_default);
		InitBlendState(bs_default);
		const RasterizerState& rs_desc = pso->desc.rs != nullptr ? *pso->desc.rs : rs_default;
		const DepthStencilState& dss_desc = pso->desc.dss != nullptr ? *pso->desc.dss : dss_default;
		const BlendState& bs_desc = pso->desc.bs != nullptr ? *pso->desc.bs : bs_default;

		internal_state->layout = layout_template;

		VkGraphicsPipelineCreateInfo pipelineInfo = {};
		VkPipelineShaderStageCreateInfo shaderStages[size_t(ShaderStage::Count)] = {};
		VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
		VkPipelineRasterizationStateCreateInfo rasterizer = {};
		VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClipStateInfo = {};
		VkPipelineRasterizationConservativeStateCreateInfoEXT rasterizationConservativeState = {};
		VkPipelineViewportStateCreateInfo viewportState = {};
		VkPipelineDepthStencilStateCreateInfo depthstencil = {};
		VkSampleMask samplemask = {};
		VkPipelineTessellationStateCreateInfo tessellationInfo = {};

		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		if (CheckCapability(GraphicsDeviceCapability::VARIABLE_RATE_SHADING_TIER2))
		{
			pipelineInfo.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
		}
		pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

		auto insert_shader_layout = [&](Shader_Vulkan* shader_interal) {
			for (uint32_t i = 0; i < SDL_arraysize(shader_interal->layout.table.SRV); ++i)
			{
				if (internal_state->layout.table.SRV[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
					continue; // it was already assigned to non-default, can't assign somethig else
				internal_state->layout.table.SRV[i] = shader_interal->layout.table.SRV[i];
				internal_state->layout.SRV_image_types[i] = shader_interal->layout.SRV_image_types[i];
			}
			for (uint32_t i = 0; i < SDL_arraysize(shader_interal->layout.table.UAV); ++i)
			{
				if (internal_state->layout.table.UAV[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
					continue; // it was already assigned to non-default, can't assign somethig else
				internal_state->layout.table.UAV[i] = shader_interal->layout.table.UAV[i];
				internal_state->layout.UAV_image_types[i] = shader_interal->layout.UAV_image_types[i];
			}
		};

		uint32_t shaderStageCount = 0;
		if (wiGraphicsShaderIsValid(pso->desc.ms))
		{
			auto shader_internal = to_internal(pso->desc.ms);
			shaderStages[shaderStageCount++] = shader_internal->stageInfo;
			insert_shader_layout(shader_internal);
		}
		if (wiGraphicsShaderIsValid(pso->desc.as))
		{
			auto shader_internal = to_internal(pso->desc.as);
			shaderStages[shaderStageCount++] = shader_internal->stageInfo;
			insert_shader_layout(shader_internal);
		}
		if (wiGraphicsShaderIsValid(pso->desc.vs))
		{
			auto shader_internal = to_internal(pso->desc.vs);
			shaderStages[shaderStageCount++] = shader_internal->stageInfo;
			insert_shader_layout(shader_internal);
		}
		if (wiGraphicsShaderIsValid(pso->desc.hs))
		{
			auto shader_internal = to_internal(pso->desc.hs);
			shaderStages[shaderStageCount++] = shader_internal->stageInfo;
			insert_shader_layout(shader_internal);
		}
		if (wiGraphicsShaderIsValid(pso->desc.ds))
		{
			auto shader_internal = to_internal(pso->desc.ds);
			shaderStages[shaderStageCount++] = shader_internal->stageInfo;
			insert_shader_layout(shader_internal);
		}
		if (wiGraphicsShaderIsValid(pso->desc.gs))
		{
			auto shader_internal = to_internal(pso->desc.gs);
			shaderStages[shaderStageCount++] = shader_internal->stageInfo;
			insert_shader_layout(shader_internal);
		}
		if (wiGraphicsShaderIsValid(pso->desc.ps))
		{
			auto shader_internal = to_internal(pso->desc.ps);
			shaderStages[shaderStageCount++] = shader_internal->stageInfo;
			insert_shader_layout(shader_internal);
		}
		pipelineInfo.stageCount = shaderStageCount;
		pipelineInfo.pStages = shaderStages;

		pipelineInfo.layout = cache_pso_layout(internal_state->layout);

		// Fixed function states:

		// Primitive type:
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		switch (pso->desc.pt)
		{
		case PrimitiveTopology::POINTLIST:
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			break;
		case PrimitiveTopology::LINELIST:
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			break;
		case PrimitiveTopology::LINESTRIP:
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			break;
		case PrimitiveTopology::TRIANGLESTRIP:
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			break;
		case PrimitiveTopology::TRIANGLELIST:
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			break;
		case PrimitiveTopology::PATCHLIST:
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
			break;
		default:
			break;
		}
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		pipelineInfo.pInputAssemblyState = &inputAssembly;


		// Rasterizer:
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.depthClampEnable = VK_TRUE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.lineWidth = 1.0f;
		rasterizer.cullMode = VK_CULL_MODE_NONE;
		rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
		rasterizer.depthBiasEnable = VK_FALSE;
		rasterizer.depthBiasConstantFactor = 0.0f;
		rasterizer.depthBiasClamp = 0.0f;
		rasterizer.depthBiasSlopeFactor = 0.0f;

		const void** tail = &rasterizer.pNext;

		// depth clip will be enabled via Vulkan 1.1 extension VK_EXT_depth_clip_enable:
		depthClipStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
		depthClipStateInfo.depthClipEnable = VK_TRUE;
		if (depth_clip_enable_features.depthClipEnable == VK_TRUE)
		{
			*tail = &depthClipStateInfo;
			tail = &depthClipStateInfo.pNext;
		}

		switch (rs_desc.fill_mode)
		{
		case FillMode::WIREFRAME:
			rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
			break;
		case FillMode::SOLID:
		default:
			rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
			break;
		}

		switch (rs_desc.cull_mode)
		{
		case CullMode::BACK:
			rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
			break;
		case CullMode::FRONT:
			rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
			break;
		case CullMode::CULL_NONE:
		default:
			rasterizer.cullMode = VK_CULL_MODE_NONE;
			break;
		}

		rasterizer.frontFace = rs_desc.front_counter_clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
		rasterizer.depthBiasEnable = rs_desc.depth_bias != 0 || rs_desc.slope_scaled_depth_bias != 0;
		rasterizer.depthBiasConstantFactor = static_cast<float>(rs_desc.depth_bias);
		rasterizer.depthBiasClamp = rs_desc.depth_bias_clamp;
		rasterizer.depthBiasSlopeFactor = rs_desc.slope_scaled_depth_bias;

		// Depth clip will be enabled via Vulkan 1.1 extension VK_EXT_depth_clip_enable:
		depthClipStateInfo.depthClipEnable = rs_desc.depth_clip_enable ? VK_TRUE : VK_FALSE;

		if (CheckCapability(GraphicsDeviceCapability::CONSERVATIVE_RASTERIZATION) && rs_desc.conservative_rasterization_enable)
		{
			rasterizationConservativeState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT;
			rasterizationConservativeState.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
			rasterizationConservativeState.extraPrimitiveOverestimationSize = 0.0f;
			*tail = &rasterizationConservativeState;
			tail = &rasterizationConservativeState.pNext;
		}

		pipelineInfo.pRasterizationState = &rasterizer;

		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 0;
		viewportState.pViewports = nullptr;
		viewportState.scissorCount = 0;
		viewportState.pScissors = nullptr;

		pipelineInfo.pViewportState = &viewportState;


		// Depth-Stencil:
		depthstencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthstencil.depthTestEnable = dss_desc.depth_enable ? VK_TRUE : VK_FALSE;
		depthstencil.depthWriteEnable = dss_desc.depth_write_mask == DepthWriteMask::DEPTH_WRITE_ZERO ? VK_FALSE : VK_TRUE;
		depthstencil.depthCompareOp = _ConvertComparisonFunc(dss_desc.depth_func);

		if (dss_desc.stencil_enable)
		{
			depthstencil.stencilTestEnable = VK_TRUE;

			depthstencil.front.compareMask = dss_desc.stencil_read_mask;
			depthstencil.front.writeMask = dss_desc.stencil_write_mask;
			depthstencil.front.reference = 0; // runtime supplied
			depthstencil.front.compareOp = _ConvertComparisonFunc(dss_desc.front_face.stencil_func);
			depthstencil.front.passOp = _ConvertStencilOp(dss_desc.front_face.stencil_pass_op);
			depthstencil.front.failOp = _ConvertStencilOp(dss_desc.front_face.stencil_fail_op);
			depthstencil.front.depthFailOp = _ConvertStencilOp(dss_desc.front_face.stencil_depth_fail_op);

			depthstencil.back.compareMask = dss_desc.stencil_read_mask;
			depthstencil.back.writeMask = dss_desc.stencil_write_mask;
			depthstencil.back.reference = 0; // runtime supplied
			depthstencil.back.compareOp = _ConvertComparisonFunc(dss_desc.back_face.stencil_func);
			depthstencil.back.passOp = _ConvertStencilOp(dss_desc.back_face.stencil_pass_op);
			depthstencil.back.failOp = _ConvertStencilOp(dss_desc.back_face.stencil_fail_op);
			depthstencil.back.depthFailOp = _ConvertStencilOp(dss_desc.back_face.stencil_depth_fail_op);
		}
		else
		{
			depthstencil.stencilTestEnable = VK_FALSE;

			depthstencil.front.compareMask = 0;
			depthstencil.front.writeMask = 0;
			depthstencil.front.reference = 0;
			depthstencil.front.compareOp = VK_COMPARE_OP_NEVER;
			depthstencil.front.passOp = VK_STENCIL_OP_KEEP;
			depthstencil.front.failOp = VK_STENCIL_OP_KEEP;
			depthstencil.front.depthFailOp = VK_STENCIL_OP_KEEP;

			depthstencil.back.compareMask = 0;
			depthstencil.back.writeMask = 0;
			depthstencil.back.reference = 0; // runtime supplied
			depthstencil.back.compareOp = VK_COMPARE_OP_NEVER;
			depthstencil.back.passOp = VK_STENCIL_OP_KEEP;
			depthstencil.back.failOp = VK_STENCIL_OP_KEEP;
			depthstencil.back.depthFailOp = VK_STENCIL_OP_KEEP;
		}

		if (CheckCapability(GraphicsDeviceCapability::DEPTH_BOUNDS_TEST))
		{
			depthstencil.depthBoundsTestEnable = dss_desc.depth_bounds_test_enable ? VK_TRUE : VK_FALSE;
		}
		else
		{
			depthstencil.depthBoundsTestEnable = VK_FALSE;
		}

		pipelineInfo.pDepthStencilState = &depthstencil;


		// Tessellation:
		tessellationInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
		tessellationInfo.patchControlPoints = desc->patch_control_points;

		pipelineInfo.pTessellationState = &tessellationInfo;

		if (pso->desc.ms == nullptr)
		{
			pipelineInfo.pDynamicState = &dynamicStateInfo;
		}
		else
		{
			pipelineInfo.pDynamicState = &dynamicStateInfo_MeshShader;
		}

		VkResult res = VK_SUCCESS;

		if (renderpass_info != nullptr)
		{
			// MSAA:
			VkPipelineMultisampleStateCreateInfo multisampling = {};
			multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			multisampling.sampleShadingEnable = VK_FALSE;
			multisampling.rasterizationSamples = (VkSampleCountFlagBits)renderpass_info->sample_count;
			if (rs_desc.forced_sample_count > 1)
			{
				multisampling.rasterizationSamples = (VkSampleCountFlagBits)rs_desc.forced_sample_count;
			}
			multisampling.minSampleShading = 1.0f;
			samplemask = pso->desc.sample_mask;
			multisampling.pSampleMask = &samplemask;
			multisampling.alphaToCoverageEnable = bs_desc.alpha_to_coverage_enable ? VK_TRUE : VK_FALSE;
			multisampling.alphaToOneEnable = VK_FALSE;

			pipelineInfo.pMultisampleState = &multisampling;


			// Blending:
			uint32_t numBlendAttachments = 0;
			VkPipelineColorBlendAttachmentState colorBlendAttachments[8] = {};
			for (size_t i = 0; i < renderpass_info->rt_count; ++i)
			{
				size_t attachmentIndex = 0;
				if (bs_desc.independent_blend_enable)
					attachmentIndex = i;

				const auto& desc = bs_desc.render_target[attachmentIndex];
				VkPipelineColorBlendAttachmentState& attachment = colorBlendAttachments[numBlendAttachments];
				numBlendAttachments++;

				attachment.blendEnable = desc.blend_enable ? VK_TRUE : VK_FALSE;

				attachment.colorWriteMask = 0;
				if (has_flag(desc.render_target_write_mask, ColorWrite::ENABLE_RED))
				{
					attachment.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
				}
				if (has_flag(desc.render_target_write_mask, ColorWrite::ENABLE_GREEN))
				{
					attachment.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
				}
				if (has_flag(desc.render_target_write_mask, ColorWrite::ENABLE_BLUE))
				{
					attachment.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
				}
				if (has_flag(desc.render_target_write_mask, ColorWrite::ENABLE_ALPHA))
				{
					attachment.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
				}

				attachment.srcColorBlendFactor = _ConvertBlend(desc.src_blend);
				attachment.dstColorBlendFactor = _ConvertBlend(desc.dest_blend);
				attachment.colorBlendOp = _ConvertBlendOp(desc.blend_op);
				attachment.srcAlphaBlendFactor = _ConvertBlend(desc.src_blend_alpha);
				attachment.dstAlphaBlendFactor = _ConvertBlend(desc.dest_blend_alpha);
				attachment.alphaBlendOp = _ConvertBlendOp(desc.blend_op_alpha);
			}

			VkPipelineColorBlendStateCreateInfo colorBlending = {};
			colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			colorBlending.logicOpEnable = VK_FALSE;
			colorBlending.logicOp = VK_LOGIC_OP_COPY;
			colorBlending.attachmentCount = numBlendAttachments;
			colorBlending.pAttachments = colorBlendAttachments;
			colorBlending.blendConstants[0] = 1.0f;
			colorBlending.blendConstants[1] = 1.0f;
			colorBlending.blendConstants[2] = 1.0f;
			colorBlending.blendConstants[3] = 1.0f;

			pipelineInfo.pColorBlendState = &colorBlending;

			// Input layout:
			VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
			vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			static constexpr uint32_t max_il_elemet_count = 32;
			StackVector<VkVertexInputBindingDescription, max_il_elemet_count> bindings;
			StackVector<VkVertexInputAttributeDescription, max_il_elemet_count> attributes;
			if (pso->desc.il != nullptr)
			{
				uint32_t lastBinding = 0xFFFFFFFF;
				const uint32_t element_count = std::min((uint32_t)arrlenu(pso->desc.il->elements), max_il_elemet_count);
				for (uint32_t i = 0; i < element_count; ++i)
				{
					auto& element = pso->desc.il->elements[i];
					if (element.input_slot == lastBinding)
						continue;
					lastBinding = element.input_slot;
					VkVertexInputBindingDescription& bind = bindings.emplace_back();
					bind.binding = element.input_slot;
					bind.inputRate = element.input_slot_class == InputClassification::PER_VERTEX_DATA ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE;
					bind.stride = GetFormatStride(element.format);
				}

				uint32_t offset = 0;
				lastBinding = 0xFFFFFFFF;
				for (uint32_t i = 0; i < element_count; ++i)
				{
					auto& element = pso->desc.il->elements[i];
					VkVertexInputAttributeDescription attr = {};
					attr.binding = element.input_slot;
					if (attr.binding != lastBinding)
					{
						lastBinding = attr.binding;
						offset = 0;
					}
					attr.format = _ConvertFormat(element.format);
					attr.location = i;
					attr.offset = element.aligned_byte_offset;
					if (attr.offset == InputLayout::APPEND_ALIGNED_ELEMENT)
					{
						// need to manually resolve this from the format spec.
						attr.offset = offset;
						offset += GetFormatStride(element.format);
					}

					attributes.push_back(attr);
				}

				vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
				vertexInputInfo.pVertexBindingDescriptions = bindings.data();
				vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
				vertexInputInfo.pVertexAttributeDescriptions = attributes.data();
			}
			pipelineInfo.pVertexInputState = &vertexInputInfo;

			pipelineInfo.renderPass = VK_NULL_HANDLE; // instead we use VkPipelineRenderingCreateInfo

			VkPipelineRenderingCreateInfo renderingInfo = {};
			renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
			renderingInfo.viewMask = 0;
			renderingInfo.colorAttachmentCount = renderpass_info->rt_count;
			VkFormat formats[8] = {};
			for (uint32_t i = 0; i < renderpass_info->rt_count; ++i)
			{
				formats[i] = _ConvertFormat(renderpass_info->rt_formats[i]);
			}
			renderingInfo.pColorAttachmentFormats = formats;
			renderingInfo.depthAttachmentFormat = _ConvertFormat(renderpass_info->ds_format);
			if (IsFormatStencilSupport(renderpass_info->ds_format))
			{
				renderingInfo.stencilAttachmentFormat = renderingInfo.depthAttachmentFormat;
			}
			pipelineInfo.pNext = &renderingInfo;

			res = vulkan_check(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &internal_state->pipeline));
		}

		return res == VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreateRaytracingAccelerationStructure(const RaytracingAccelerationStructureDesc* desc, RaytracingAccelerationStructure* bvh) const
	{
		auto internal_state = wi::allocator::make_shared<BVH_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		bvh->internal_state = internal_state;
		bvh->type = GPUResource::Type::RAYTRACING_ACCELERATION_STRUCTURE;
		::wi::CloneRaytracingAccelerationStructureDesc(bvh->desc, *desc);
		bvh->size = 0;

		internal_state->buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		internal_state->buildInfo.flags = 0;
		if (bvh->desc.flags & RaytracingAccelerationStructureDesc::FLAG_ALLOW_UPDATE)
		{
			internal_state->buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
		}
		if (bvh->desc.flags & RaytracingAccelerationStructureDesc::FLAG_ALLOW_COMPACTION)
		{
			internal_state->buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
		}
		if (bvh->desc.flags & RaytracingAccelerationStructureDesc::FLAG_PREFER_FAST_TRACE)
		{
			internal_state->buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		}
		if (bvh->desc.flags & RaytracingAccelerationStructureDesc::FLAG_PREFER_FAST_BUILD)
		{
			internal_state->buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
		}
		if (bvh->desc.flags & RaytracingAccelerationStructureDesc::FLAG_MINIMIZE_MEMORY)
		{
			internal_state->buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
		}

		switch (desc->type)
		{
		case RaytracingAccelerationStructureDesc::Type::BOTTOMLEVEL:
		{
			internal_state->buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

			for (size_t geometry_index = 0; geometry_index < arrlenu(desc->bottom_level.geometries); ++geometry_index)
			{
				auto& x = desc->bottom_level.geometries[geometry_index];
				arrput(internal_state->geometries, {});
				auto& geometry = internal_state->geometries[arrlenu(internal_state->geometries) - 1];
				geometry = {};
				geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;

				arrput(internal_state->primitiveCounts, 0);
				uint32_t& primitiveCount = internal_state->primitiveCounts[arrlenu(internal_state->primitiveCounts) - 1];

				if (x.type == RaytracingAccelerationStructureDesc::BottomLevel::Geometry::Type::TRIANGLES)
				{
					geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
					geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
					geometry.geometry.triangles.indexType = x.triangles.index_format == IndexBufferFormat::UINT16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
					geometry.geometry.triangles.maxVertex = x.triangles.vertex_count;
					geometry.geometry.triangles.vertexStride = x.triangles.vertex_stride;
					geometry.geometry.triangles.vertexFormat = _ConvertFormat(x.triangles.vertex_format);

					primitiveCount = x.triangles.index_count / 3;
				}
				else if (x.type == RaytracingAccelerationStructureDesc::BottomLevel::Geometry::Type::PROCEDURAL_AABBS)
				{
					geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
					geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
					geometry.geometry.aabbs.stride = sizeof(float) * 6; // min - max corners

					primitiveCount = x.aabbs.count;
				}
			}


		}
		break;
		case RaytracingAccelerationStructureDesc::Type::TOPLEVEL:
		{
			internal_state->buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

			arrput(internal_state->geometries, {});
			auto& geometry = internal_state->geometries[arrlenu(internal_state->geometries) - 1];
			geometry = {};
			geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
			geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
			geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
			geometry.geometry.instances.arrayOfPointers = VK_FALSE;

			arrput(internal_state->primitiveCounts, 0);
			uint32_t& primitiveCount = internal_state->primitiveCounts[arrlenu(internal_state->primitiveCounts) - 1];
			primitiveCount = desc->top_level.count;
		}
		break;
		}

		internal_state->buildInfo.geometryCount = (uint32_t)arrlenu(internal_state->geometries);
		internal_state->buildInfo.pGeometries = internal_state->geometries;

		internal_state->sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

		// Compute memory requirements:
		vkGetAccelerationStructureBuildSizesKHR(
			device,
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
			&internal_state->buildInfo,
			internal_state->primitiveCounts,
			&internal_state->sizeInfo
		);

		// Backing memory as buffer:
		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = align(internal_state->sizeInfo.accelerationStructureSize, (VkDeviceSize)acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment) + std::max(internal_state->sizeInfo.buildScratchSize, internal_state->sizeInfo.updateScratchSize);
		bufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
		bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; // scratch
		bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		bufferInfo.flags = 0;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

		vulkan_check(vmaCreateBuffer(
			allocationhandler->allocator,
			&bufferInfo,
			&allocInfo,
			&internal_state->buffer,
			&internal_state->allocation,
			nullptr
		));

		// Create the acceleration structure:
		internal_state->createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		internal_state->createInfo.type = internal_state->buildInfo.type;
		internal_state->createInfo.buffer = internal_state->buffer;
		internal_state->createInfo.size = internal_state->sizeInfo.accelerationStructureSize;

		VkResult res = vulkan_check(vkCreateAccelerationStructureKHR(
			device,
			&internal_state->createInfo,
			nullptr,
			&internal_state->resource
		));

		// Get the device address for the acceleration structure:
		VkAccelerationStructureDeviceAddressInfoKHR addrinfo = {};
		addrinfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		addrinfo.accelerationStructure = internal_state->resource;
		internal_state->as_address = vkGetAccelerationStructureDeviceAddressKHR(device, &addrinfo);

		// Get scratch address:
		VkBufferDeviceAddressInfo addressinfo = {};
		addressinfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		addressinfo.buffer = internal_state->buffer;
		internal_state->scratch_address = align(vkGetBufferDeviceAddress(device, &addressinfo) + internal_state->sizeInfo.accelerationStructureSize, (VkDeviceAddress)acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment);

		if (desc->type == RaytracingAccelerationStructureDesc::Type::TOPLEVEL)
		{
			internal_state->index = allocationhandler->bindlessAccelerationStructures.allocate();
			if (internal_state->index >= 0)
			{
				VkWriteDescriptorSetAccelerationStructureKHR acc = {};
				acc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
				acc.accelerationStructureCount = 1;
				acc.pAccelerationStructures = &internal_state->resource;

				VkWriteDescriptorSet write = {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
				write.dstBinding = 0;
				write.dstArrayElement = internal_state->index;
				write.descriptorCount = 1;
				write.dstSet = allocationhandler->bindlessAccelerationStructures.descriptorSet;
				write.pNext = &acc;
				vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
			}
		}

		bvh->size = bufferInfo.size;

		return res == VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreateRaytracingPipelineState(const RaytracingPipelineStateDesc* desc, RaytracingPipelineState* rtpso) const
	{
		auto internal_state = wi::allocator::make_shared<RTPipelineState_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		rtpso->internal_state = internal_state;
		::wi::CloneRaytracingPipelineStateDesc(rtpso->desc, *desc);

		VkRayTracingPipelineCreateInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
		info.flags = 0;

		VkPipelineShaderStageCreateInfo* stages = nullptr; // stb_ds array: transient raytracing shader stage list.
		for (size_t shader_library_index = 0; shader_library_index < arrlenu(desc->shader_libraries); ++shader_library_index)
		{
			auto& x = desc->shader_libraries[shader_library_index];
			auto shader_internal = to_internal(x.shader);
			info.layout = shader_internal->layout.pipeline_layout;
			arrput(stages, {});
			auto& stage = stages[arrlenu(stages) - 1];
			stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stage.module = shader_internal->shaderModule;
			switch (x.type)
			{
			default:
			case ShaderLibrary::Type::RAYGENERATION:
				stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
				break;
			case ShaderLibrary::Type::MISS:
				stage.stage = VK_SHADER_STAGE_MISS_BIT_KHR;
				break;
			case ShaderLibrary::Type::CLOSESTHIT:
				stage.stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
				break;
			case ShaderLibrary::Type::ANYHIT:
				stage.stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
				break;
			case ShaderLibrary::Type::INTERSECTION:
				stage.stage = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
				break;
			}
			stage.pName = (x.function_name != nullptr && x.function_name[0] != '\0') ? x.function_name : "main";
		}
		info.stageCount = (uint32_t)arrlenu(stages);
		info.pStages = stages;

		VkRayTracingShaderGroupCreateInfoKHR* groups = nullptr; // stb_ds array: transient raytracing group list.
		for (size_t hit_group_index = 0; hit_group_index < arrlenu(desc->hit_groups); ++hit_group_index)
		{
			auto& x = desc->hit_groups[hit_group_index];
			arrput(groups, {});
			auto& group = groups[arrlenu(groups) - 1];
			group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
			switch (x.type)
			{
			default:
			case ShaderHitGroup::Type::GENERAL:
				group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
				break;
			case ShaderHitGroup::Type::TRIANGLES:
				group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
				break;
			case ShaderHitGroup::Type::PROCEDURAL:
				group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
				break;
			}
			group.generalShader = x.general_shader;
			group.closestHitShader = x.closest_hit_shader;
			group.anyHitShader = x.any_hit_shader;
			group.intersectionShader = x.intersection_shader;
		}
		info.groupCount = (uint32_t)arrlenu(groups);
		info.pGroups = groups;

		info.maxPipelineRayRecursionDepth = desc->max_trace_recursion_depth;

		//VkRayTracingPipelineInterfaceCreateInfoKHR library_interface = {};
		//library_interface.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR;
		//library_interface.maxPipelineRayPayloadSize = pDesc->max_payload_size_in_bytes;
		//library_interface.maxPipelineRayHitAttributeSize = pDesc->max_attribute_size_in_bytes;
		//info.pLibraryInterface = &library_interface;

		info.basePipelineHandle = VK_NULL_HANDLE;
		info.basePipelineIndex = 0;

		VkResult res = vulkan_check(vkCreateRayTracingPipelinesKHR(
			device,
			VK_NULL_HANDLE,
			pipelineCache,
			1,
			&info,
			nullptr,
			&internal_state->pipeline
		));

		return res == VK_SUCCESS;
	}
	bool GraphicsDevice_Vulkan::CreateVideoDecoder(const VideoDesc* desc, VideoDecoder* video_decoder) const
	{
		auto internal_state = wi::allocator::make_shared<VideoDecoder_Vulkan>();
		internal_state->allocationhandler = allocationhandler;
		video_decoder->internal_state = internal_state;
		video_decoder->desc = *desc;

		if (video_decoder->desc.profile == VideoProfile::H264)
		{
			StdVideoH264PictureParameterSet* pps_array_h264 = nullptr;
			StdVideoH264ScalingLists* scalinglist_array_h264 = nullptr;
			arrsetlen(pps_array_h264, desc->pps_count);
			arrsetlen(scalinglist_array_h264, desc->pps_count);
			for (uint32_t i = 0; i < desc->pps_count; ++i)
			{
				const h264::PPS* pps = (const h264::PPS*)desc->pps_datas + i;
				StdVideoH264PictureParameterSet& vk_pps = pps_array_h264[i];
				StdVideoH264ScalingLists& vk_scalinglist = scalinglist_array_h264[i];

				vk_pps.flags.transform_8x8_mode_flag = pps->transform_8x8_mode_flag;
				vk_pps.flags.redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present_flag;
				vk_pps.flags.constrained_intra_pred_flag = pps->constrained_intra_pred_flag;
				vk_pps.flags.deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag;
				vk_pps.flags.weighted_pred_flag = pps->weighted_pred_flag;
				vk_pps.flags.bottom_field_pic_order_in_frame_present_flag = pps->pic_order_present_flag;
				vk_pps.flags.entropy_coding_mode_flag = pps->entropy_coding_mode_flag;
				vk_pps.flags.pic_scaling_matrix_present_flag = pps->pic_scaling_matrix_present_flag;

				vk_pps.seq_parameter_set_id = pps->seq_parameter_set_id;
				vk_pps.pic_parameter_set_id = pps->pic_parameter_set_id;
				vk_pps.num_ref_idx_l0_default_active_minus1 = pps->num_ref_idx_l0_active_minus1;
				vk_pps.num_ref_idx_l1_default_active_minus1 = pps->num_ref_idx_l1_active_minus1;
				vk_pps.weighted_bipred_idc = (StdVideoH264WeightedBipredIdc)pps->weighted_bipred_idc;
				vk_pps.pic_init_qp_minus26 = pps->pic_init_qp_minus26;
				vk_pps.pic_init_qs_minus26 = pps->pic_init_qs_minus26;
				vk_pps.chroma_qp_index_offset = pps->chroma_qp_index_offset;
				vk_pps.second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;

				vk_pps.pScalingLists = &vk_scalinglist;
				for (int j = 0; j < SDL_arraysize(pps->pic_scaling_list_present_flag); ++j)
				{
					vk_scalinglist.scaling_list_present_mask |= pps->pic_scaling_list_present_flag[j] << j;
				}
				for (int j = 0; j < SDL_arraysize(pps->UseDefaultScalingMatrix4x4Flag); ++j)
				{
					vk_scalinglist.use_default_scaling_matrix_mask |= pps->UseDefaultScalingMatrix4x4Flag[j] << j;
				}
				for (int j = 0; j < SDL_arraysize(pps->ScalingList4x4); ++j)
				{
					for (int k = 0; k < SDL_arraysize(pps->ScalingList4x4[j]); ++k)
					{
						vk_scalinglist.ScalingList4x4[j][k] = (uint8_t)pps->ScalingList4x4[j][k];
					}
				}
				for (int j = 0; j < SDL_arraysize(pps->ScalingList8x8); ++j)
				{
					for (int k = 0; k < SDL_arraysize(pps->ScalingList8x8[j]); ++k)
					{
						vk_scalinglist.ScalingList8x8[j][k] = (uint8_t)pps->ScalingList8x8[j][k];
					}
				}
			}

			uint32_t num_reference_frames = 0;
			StdVideoH264SequenceParameterSet* sps_array_h264 = nullptr;
			StdVideoH264SequenceParameterSetVui* vui_array_h264 = nullptr;
			StdVideoH264HrdParameters* hrd_array_h264 = nullptr;
			arrsetlen(sps_array_h264, desc->sps_count);
			arrsetlen(vui_array_h264, desc->sps_count);
			arrsetlen(hrd_array_h264, desc->sps_count);
			for (uint32_t i = 0; i < desc->sps_count; ++i)
			{
				const h264::SPS* sps = (const h264::SPS*)desc->sps_datas + i;
				StdVideoH264SequenceParameterSet& vk_sps = sps_array_h264[i];

				vk_sps.flags.constraint_set0_flag = sps->constraint_set0_flag;
				vk_sps.flags.constraint_set1_flag = sps->constraint_set1_flag;
				vk_sps.flags.constraint_set2_flag = sps->constraint_set2_flag;
				vk_sps.flags.constraint_set3_flag = sps->constraint_set3_flag;
				vk_sps.flags.constraint_set4_flag = sps->constraint_set4_flag;
				vk_sps.flags.constraint_set5_flag = sps->constraint_set5_flag;
				vk_sps.flags.direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
				vk_sps.flags.mb_adaptive_frame_field_flag = sps->mb_adaptive_frame_field_flag;
				vk_sps.flags.frame_mbs_only_flag = sps->frame_mbs_only_flag;
				vk_sps.flags.delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag;
				vk_sps.flags.separate_colour_plane_flag = sps->separate_colour_plane_flag;
				vk_sps.flags.gaps_in_frame_num_value_allowed_flag = sps->gaps_in_frame_num_value_allowed_flag;
				vk_sps.flags.qpprime_y_zero_transform_bypass_flag = sps->qpprime_y_zero_transform_bypass_flag;
				vk_sps.flags.frame_cropping_flag = sps->frame_cropping_flag;
				vk_sps.flags.seq_scaling_matrix_present_flag = sps->seq_scaling_matrix_present_flag;
				vk_sps.flags.vui_parameters_present_flag = sps->vui_parameters_present_flag;

				if (vk_sps.flags.vui_parameters_present_flag)
				{
					StdVideoH264SequenceParameterSetVui& vk_vui = vui_array_h264[i];
					vk_sps.pSequenceParameterSetVui = &vk_vui;
					vk_vui.flags.aspect_ratio_info_present_flag = sps->vui.aspect_ratio_info_present_flag;
					vk_vui.flags.overscan_info_present_flag = sps->vui.overscan_info_present_flag;
					vk_vui.flags.overscan_appropriate_flag = sps->vui.overscan_appropriate_flag;
					vk_vui.flags.video_signal_type_present_flag = sps->vui.video_signal_type_present_flag;
					vk_vui.flags.video_full_range_flag = sps->vui.video_full_range_flag;
					vk_vui.flags.color_description_present_flag = sps->vui.colour_description_present_flag;
					vk_vui.flags.chroma_loc_info_present_flag = sps->vui.chroma_loc_info_present_flag;
					vk_vui.flags.timing_info_present_flag = sps->vui.timing_info_present_flag;
					vk_vui.flags.fixed_frame_rate_flag = sps->vui.fixed_frame_rate_flag;
					vk_vui.flags.bitstream_restriction_flag = sps->vui.bitstream_restriction_flag;
					vk_vui.flags.nal_hrd_parameters_present_flag = sps->vui.nal_hrd_parameters_present_flag;
					vk_vui.flags.vcl_hrd_parameters_present_flag = sps->vui.vcl_hrd_parameters_present_flag;

					vk_vui.aspect_ratio_idc = (StdVideoH264AspectRatioIdc)sps->vui.aspect_ratio_idc;
					vk_vui.sar_width = sps->vui.sar_width;
					vk_vui.sar_height = sps->vui.sar_height;
					vk_vui.video_format = sps->vui.video_format;
					vk_vui.colour_primaries = sps->vui.colour_primaries;
					vk_vui.transfer_characteristics = sps->vui.transfer_characteristics;
					vk_vui.matrix_coefficients = sps->vui.matrix_coefficients;
					vk_vui.num_units_in_tick = sps->vui.num_units_in_tick;
					vk_vui.time_scale = sps->vui.time_scale;
					vk_vui.max_num_reorder_frames = sps->vui.num_reorder_frames;
					vk_vui.max_dec_frame_buffering = sps->vui.max_dec_frame_buffering;
					vk_vui.chroma_sample_loc_type_top_field = sps->vui.chroma_sample_loc_type_top_field;
					vk_vui.chroma_sample_loc_type_bottom_field = sps->vui.chroma_sample_loc_type_bottom_field;

					StdVideoH264HrdParameters& vk_hrd = hrd_array_h264[i];
					vk_vui.pHrdParameters = &vk_hrd;
					vk_hrd.cpb_cnt_minus1 = sps->hrd.cpb_cnt_minus1;
					vk_hrd.bit_rate_scale = sps->hrd.bit_rate_scale;
					vk_hrd.cpb_size_scale = sps->hrd.cpb_size_scale;
					for (int j = 0; j < SDL_arraysize(sps->hrd.bit_rate_value_minus1); ++j)
					{
						vk_hrd.bit_rate_value_minus1[j] = sps->hrd.bit_rate_value_minus1[j];
						vk_hrd.cpb_size_value_minus1[j] = sps->hrd.cpb_size_value_minus1[j];
						vk_hrd.cbr_flag[j] = sps->hrd.cbr_flag[j];
					}
					vk_hrd.initial_cpb_removal_delay_length_minus1 = sps->hrd.initial_cpb_removal_delay_length_minus1;
					vk_hrd.cpb_removal_delay_length_minus1 = sps->hrd.cpb_removal_delay_length_minus1;
					vk_hrd.dpb_output_delay_length_minus1 = sps->hrd.dpb_output_delay_length_minus1;
					vk_hrd.time_offset_length = sps->hrd.time_offset_length;
				}

				vk_sps.profile_idc = (StdVideoH264ProfileIdc)sps->profile_idc;
				switch (sps->level_idc)
				{
				case 0:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_1_0;
					break;
				case 11:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_1_1;
					break;
				case 12:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_1_2;
					break;
				case 13:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_1_3;
					break;
				case 20:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_2_0;
					break;
				case 21:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_2_1;
					break;
				case 22:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_2_2;
					break;
				case 30:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_3_0;
					break;
				case 31:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_3_1;
					break;
				case 32:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_3_2;
					break;
				case 40:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_0;
					break;
				case 41:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
					break;
				case 42:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_2;
					break;
				case 50:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_5_0;
					break;
				case 51:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_5_1;
					break;
				case 52:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_5_2;
					break;
				case 60:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_6_0;
					break;
				case 61:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_6_1;
					break;
				case 62:
					vk_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_6_2;
					break;
				default:
					SDL_assert(0);
					break;
				}
				SDL_assert(vk_sps.level_idc <= decode_h264_capabilities.maxLevelIdc);
				//vk_sps.chroma_format_idc = (StdVideoH264ChromaFormatIdc)sps->chroma_format_idc;
				vk_sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420; // only one we support currently
				vk_sps.seq_parameter_set_id = sps->seq_parameter_set_id;
				vk_sps.bit_depth_luma_minus8 = sps->bit_depth_luma_minus8;
				vk_sps.bit_depth_chroma_minus8 = sps->bit_depth_chroma_minus8;
				vk_sps.log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
				vk_sps.pic_order_cnt_type = (StdVideoH264PocType)sps->pic_order_cnt_type;
				vk_sps.offset_for_non_ref_pic = sps->offset_for_non_ref_pic;
				vk_sps.offset_for_top_to_bottom_field = sps->offset_for_top_to_bottom_field;
				vk_sps.log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
				vk_sps.num_ref_frames_in_pic_order_cnt_cycle = sps->num_ref_frames_in_pic_order_cnt_cycle;
				vk_sps.max_num_ref_frames = sps->num_ref_frames;
				vk_sps.pic_width_in_mbs_minus1 = sps->pic_width_in_mbs_minus1;
				vk_sps.pic_height_in_map_units_minus1 = sps->pic_height_in_map_units_minus1;
				vk_sps.frame_crop_left_offset = sps->frame_crop_left_offset;
				vk_sps.frame_crop_right_offset = sps->frame_crop_right_offset;
				vk_sps.frame_crop_top_offset = sps->frame_crop_top_offset;
				vk_sps.frame_crop_bottom_offset = sps->frame_crop_bottom_offset;
				vk_sps.pOffsetForRefFrame = sps->offset_for_ref_frame;

				num_reference_frames = std::max(num_reference_frames, (uint32_t)sps->num_ref_frames);
			}

			num_reference_frames = std::min(num_reference_frames, video_capability_h264.video_capabilities.maxActiveReferencePictures);

			VkVideoDecodeH264SessionParametersAddInfoKHR session_parameters_add_info_h264 = {};
			session_parameters_add_info_h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
			session_parameters_add_info_h264.stdPPSCount = (uint32_t)arrlenu(pps_array_h264);
			session_parameters_add_info_h264.pStdPPSs = pps_array_h264;
			session_parameters_add_info_h264.stdSPSCount = (uint32_t)arrlenu(sps_array_h264);
			session_parameters_add_info_h264.pStdSPSs = sps_array_h264;

			VkVideoSessionCreateInfoKHR info = {};
			info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
			info.queueFamilyIndex = videoFamily;
			info.maxActiveReferencePictures = num_reference_frames * 2; // *2: top and bottom field counts as two I think: https://vulkan.lunarg.com/doc/view/1.3.239.0/windows/1.3-extensions/vkspec.html#_video_decode_commands
			info.maxDpbSlots = std::min(desc->num_dpb_slots, video_capability_h264.video_capabilities.maxDpbSlots);
			info.maxCodedExtent.width = std::min(desc->width, video_capability_h264.video_capabilities.maxCodedExtent.width);
			info.maxCodedExtent.height = std::min(desc->height, video_capability_h264.video_capabilities.maxCodedExtent.height);
			info.pictureFormat = _ConvertFormat(desc->format);
			info.referencePictureFormat = info.pictureFormat;
			info.pVideoProfile = &video_capability_h264.profile;
			info.pStdHeaderVersion = &video_capability_h264.video_capabilities.stdHeaderVersion;

			vulkan_check(vkCreateVideoSessionKHR(device, &info, nullptr, &internal_state->video_session));

			uint32_t requirement_count = 0;
			vulkan_check(vkGetVideoSessionMemoryRequirementsKHR(device, internal_state->video_session, &requirement_count, nullptr));

			VkVideoSessionMemoryRequirementsKHR* requirements = nullptr;
			arrsetlen(requirements, requirement_count);
			for (uint32_t i = 0; i < requirement_count; ++i)
			{
				requirements[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
			}
			vulkan_check(vkGetVideoSessionMemoryRequirementsKHR(device, internal_state->video_session, &requirement_count, requirements));

			arrsetlen(internal_state->allocations, requirement_count);
			VkBindVideoSessionMemoryInfoKHR* bind_session_memory_infos = nullptr;
			arrsetlen(bind_session_memory_infos, requirement_count);
			for (uint32_t i = 0; i < requirement_count; ++i)
			{
				const VkVideoSessionMemoryRequirementsKHR& video_req = requirements[i];
				VmaAllocationCreateInfo alloc_create_info = {};
				alloc_create_info.memoryTypeBits = video_req.memoryRequirements.memoryTypeBits;
				VmaAllocationInfo alloc_info = {};

				vulkan_check(vmaAllocateMemory(
					allocationhandler->allocator,
					&video_req.memoryRequirements,
					&alloc_create_info,
					&internal_state->allocations[i],
					&alloc_info
				));

				VkBindVideoSessionMemoryInfoKHR& bind_info = bind_session_memory_infos[i];
				bind_info.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
				bind_info.memory = alloc_info.deviceMemory;
				bind_info.memoryBindIndex = video_req.memoryBindIndex;
				bind_info.memoryOffset = alloc_info.offset;
				bind_info.memorySize = alloc_info.size;
			}
			vulkan_check(vkBindVideoSessionMemoryKHR(device, internal_state->video_session, requirement_count, bind_session_memory_infos));

			VkVideoDecodeH264SessionParametersCreateInfoKHR session_parameters_info_h264 = {};
			session_parameters_info_h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
			session_parameters_info_h264.maxStdPPSCount = (uint32_t)arrlenu(pps_array_h264);
			session_parameters_info_h264.maxStdSPSCount = (uint32_t)arrlenu(sps_array_h264);
			session_parameters_info_h264.pParametersAddInfo = &session_parameters_add_info_h264;

			VkVideoSessionParametersCreateInfoKHR session_parameters_info = {};
			session_parameters_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
			session_parameters_info.videoSession = internal_state->video_session;
			session_parameters_info.videoSessionParametersTemplate = VK_NULL_HANDLE;
			session_parameters_info.pNext = &session_parameters_info_h264;
			vulkan_check(vkCreateVideoSessionParametersKHR(device, &session_parameters_info, nullptr, &internal_state->session_parameters));

			video_decoder->support = {};
			if (video_capability_h264.decode_capabilities.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR)
			{
				video_decoder->support |= VideoDecoderSupportFlags::DPB_AND_OUTPUT_COINCIDE;
			}
			if (video_capability_h264.decode_capabilities.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR)
			{
				video_decoder->support |= VideoDecoderSupportFlags::DPB_AND_OUTPUT_DISTINCT;
			}
			video_decoder->support |= VideoDecoderSupportFlags::DPB_INDIVIDUAL_TEXTURES_SUPPORTED;
		}
		else if (video_decoder->desc.profile == VideoProfile::H265)
		{
			SDL_assert(0); // TODO
			return false;
		}

		return true;
	}

	int GraphicsDevice_Vulkan::CreateSubresource(Texture* texture, SubresourceType type, uint32_t firstSlice, uint32_t sliceCount, uint32_t firstMip, uint32_t mipCount, const Format* format_change, const ImageAspect* aspect, const Swizzle* swizzle, float min_lod_clamp) const
	{
		auto internal_state = to_internal(texture);

		Format format = wiGraphicsTextureGetDesc(texture)->format;
		if (format_change != nullptr)
		{
			format = *format_change;
		}

		Texture_Vulkan::TextureSubresource subresource;
		subresource.firstMip = firstMip;
		subresource.mipCount = mipCount;
		subresource.firstSlice = firstSlice;
		subresource.sliceCount = sliceCount;

		VkImageViewCreateInfo view_desc = {};
		view_desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_desc.flags = 0;
		view_desc.image = internal_state->resource;
		view_desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		if (aspect != nullptr)
		{
			view_desc.subresourceRange.aspectMask = _ConvertImageAspect(*aspect);
		}
		view_desc.subresourceRange.baseArrayLayer = firstSlice;
		view_desc.subresourceRange.layerCount = sliceCount;
		view_desc.subresourceRange.baseMipLevel = firstMip;
		view_desc.subresourceRange.levelCount = mipCount;
		if (type == SubresourceType::SRV)
		{
			view_desc.components = _ConvertSwizzle(swizzle == nullptr ? texture->desc.swizzle : *swizzle);
		}
		switch (format)
		{
		case Format::NV12:
			if (view_desc.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT)
			{
				view_desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
				view_desc.format = VK_FORMAT_R8_UNORM;
			}
			else if (view_desc.subresourceRange.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT)
			{
				view_desc.format = VK_FORMAT_R8_UNORM;
			}
			else if (view_desc.subresourceRange.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT)
			{
				view_desc.format = VK_FORMAT_R8G8_UNORM;
			}
			break;
		default:
			view_desc.format = _ConvertFormat(format);
			break;
		}

		if (texture->desc.type == TextureDesc::Type::TEXTURE_1D)
		{
			if (texture->desc.array_size > 1)
			{
				view_desc.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			}
			else
			{
				view_desc.viewType = VK_IMAGE_VIEW_TYPE_1D;
			}
		}
		else if (texture->desc.type == TextureDesc::Type::TEXTURE_2D)
		{
			if (texture->desc.array_size > 1)
			{
				if (has_flag(texture->desc.misc_flags, ResourceMiscFlag::TEXTURECUBE))
				{
					if (texture->desc.array_size > 6 && sliceCount > 6)
					{
						view_desc.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
					}
					else
					{
						view_desc.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
					}
				}
				else
				{
					view_desc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
				}
			}
			else
			{
				view_desc.viewType = VK_IMAGE_VIEW_TYPE_2D;
			}
		}
		else if (texture->desc.type == TextureDesc::Type::TEXTURE_3D)
		{
			view_desc.viewType = VK_IMAGE_VIEW_TYPE_3D;
		}

		switch (type)
		{
		case SubresourceType::SRV:
		{
			if (IsFormatDepthSupport(format))
			{
				view_desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			}

			// This is required in cases where image was created with eg. USAGE_STORAGE, but
			//	the view format that we are creating doesn't support USAGE_STORAGE (for examplle: SRGB formats)
			VkImageViewUsageCreateInfo viewUsageInfo = {};
			viewUsageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
			viewUsageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
			view_desc.pNext = &viewUsageInfo;

			VkImageViewMinLodCreateInfoEXT min_lod_info = {};
			min_lod_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT;
			min_lod_info.minLod = min_lod_clamp;
			if (min_lod_clamp > 0 && image_view_min_lod_features.minLod == VK_TRUE)
			{
				viewUsageInfo.pNext = &min_lod_info;
			}

			VkResult res = vkCreateImageView(device, &view_desc, nullptr, &subresource.image_view);

			subresource.index = allocationhandler->bindlessSampledImages.allocate();
			if (subresource.index >= 0)
			{
				VkDescriptorImageInfo imageInfo = {};
				imageInfo.imageView = subresource.image_view;
				imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				VkWriteDescriptorSet write = {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				write.dstBinding = 0;
				write.dstArrayElement = subresource.index;
				write.descriptorCount = 1;
				write.dstSet = allocationhandler->bindlessSampledImages.descriptorSet;
				write.pImageInfo = &imageInfo;
				vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
			}

			if (res == VK_SUCCESS)
			{
				if (!internal_state->srv.IsValid())
				{
					internal_state->srv = subresource;
					return -1;
				}
				internal_state->subresources_srv.push_back(subresource);
				return int(internal_state->subresources_srv.size() - 1);
			}
			else
			{
				SDL_assert(0);
			}
		}
		break;
		case SubresourceType::UAV:
		{
			if (view_desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE || view_desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
			{
				view_desc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			}

			VkResult res = vkCreateImageView(device, &view_desc, nullptr, &subresource.image_view);

			subresource.index = allocationhandler->bindlessStorageImages.allocate();
			if (subresource.index >= 0)
			{
				VkDescriptorImageInfo imageInfo = {};
				imageInfo.imageView = subresource.image_view;
				imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				VkWriteDescriptorSet write = {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				write.dstBinding = 0;
				write.dstArrayElement = subresource.index;
				write.descriptorCount = 1;
				write.dstSet = allocationhandler->bindlessStorageImages.descriptorSet;
				write.pImageInfo = &imageInfo;
				vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
			}

			if (res == VK_SUCCESS)
			{
				if (!internal_state->uav.IsValid())
				{
					internal_state->uav = subresource;
					return -1;
				}
				internal_state->subresources_uav.push_back(subresource);
				return int(internal_state->subresources_uav.size() - 1);
			}
			else
			{
				SDL_assert(0);
			}
		}
		break;
		case SubresourceType::RTV:
		{
			view_desc.subresourceRange.levelCount = 1;
			VkResult res = vkCreateImageView(device, &view_desc, nullptr, &subresource.image_view);

			if (res == VK_SUCCESS)
			{
				if (!internal_state->rtv.IsValid())
				{
					internal_state->rtv = subresource;
					internal_state->framebuffer_layercount = view_desc.subresourceRange.layerCount;
					return -1;
				}
				internal_state->subresources_rtv.push_back(subresource);
				return int(internal_state->subresources_rtv.size() - 1);
			}
			else
			{
				SDL_assert(0);
			}
		}
		break;
		case SubresourceType::DSV:
		{
			view_desc.subresourceRange.levelCount = 1;
			view_desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			VkResult res = vkCreateImageView(device, &view_desc, nullptr, &subresource.image_view);

			if (res == VK_SUCCESS)
			{
				if (!internal_state->dsv.IsValid())
				{
					internal_state->dsv = subresource;
					internal_state->framebuffer_layercount = view_desc.subresourceRange.layerCount;
					return -1;
				}
				internal_state->subresources_dsv.push_back(subresource);
				return int(internal_state->subresources_dsv.size() - 1);
			}
			else
			{
				SDL_assert(0);
			}
		}
		break;
		default:
			break;
		}
		return -1;
	}
	int GraphicsDevice_Vulkan::CreateSubresource(GPUBuffer* buffer, SubresourceType type, uint64_t offset, uint64_t size, const Format* format_change, const uint32_t* structuredbuffer_stride_change) const
	{
		auto internal_state = to_internal(buffer);
		const GPUBufferDesc& desc = *wiGraphicsGPUBufferGetDesc(buffer);
		VkResult res;

		Buffer_Vulkan::BufferSubresource subresource;

		Format format = desc.format;
		if (format_change != nullptr)
		{
			format = *format_change;
		}
		if (type == SubresourceType::UAV)
		{
			// RW resource can't be SRGB
			format = GetFormatNonSRGB(format);
		}

		switch (type)
		{

		case SubresourceType::SRV:
		case SubresourceType::UAV:
		{
			if (format == Format::UNKNOWN)
			{
				// Raw buffer
				subresource.index = allocationhandler->bindlessStorageBuffers.allocate();
				if (subresource.IsValid())
				{
					subresource.buffer_info.buffer = internal_state->resource;
					subresource.buffer_info.offset = offset;
					subresource.buffer_info.range = size;

					VkWriteDescriptorSet write = {};
					write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					write.dstBinding = 0;
					write.dstArrayElement = subresource.index;
					write.descriptorCount = 1;
					write.dstSet = allocationhandler->bindlessStorageBuffers.descriptorSet;
					write.pBufferInfo = &subresource.buffer_info;
					vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
				}

				if (type == SubresourceType::SRV)
				{
					if (!internal_state->srv.IsValid())
					{
						internal_state->srv = subresource;
						return -1;
					}
					internal_state->subresources_srv.push_back(subresource);
					return int(internal_state->subresources_srv.size() - 1);
				}
				else
				{
					if (!internal_state->uav.IsValid())
					{
						internal_state->uav = subresource;
						return -1;
					}
					internal_state->subresources_uav.push_back(subresource);
					return int(internal_state->subresources_uav.size() - 1);
				}
			}
			else
			{
				// Typed buffer
				VkBufferViewCreateInfo srv_desc = {};
				srv_desc.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
				srv_desc.buffer = internal_state->resource;
				srv_desc.flags = 0;
				srv_desc.format = _ConvertFormat(format);
				srv_desc.offset = offset;
				srv_desc.range = std::min(size, (uint64_t)desc.size - srv_desc.offset);

				res = vkCreateBufferView(device, &srv_desc, nullptr, &subresource.buffer_view);

				if (res == VK_SUCCESS)
				{
					if (type == SubresourceType::SRV)
					{
						subresource.index = allocationhandler->bindlessUniformTexelBuffers.allocate();
						if (subresource.IsValid())
						{
							VkWriteDescriptorSet write = {};
							write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
							write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
							write.dstBinding = 0;
							write.dstArrayElement = subresource.index;
							write.descriptorCount = 1;
							write.dstSet = allocationhandler->bindlessUniformTexelBuffers.descriptorSet;
							write.pTexelBufferView = &subresource.buffer_view;
							vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
						}

						if (!internal_state->srv.IsValid())
						{
							internal_state->srv = subresource;
							return -1;
						}
						internal_state->subresources_srv.push_back(subresource);
						return int(internal_state->subresources_srv.size() - 1);
					}
					else
					{
						subresource.index = allocationhandler->bindlessStorageTexelBuffers.allocate();
						if (subresource.IsValid())
						{
							VkWriteDescriptorSet write = {};
							write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
							write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
							write.dstBinding = 0;
							write.dstArrayElement = subresource.index;
							write.descriptorCount = 1;
							write.dstSet = allocationhandler->bindlessStorageTexelBuffers.descriptorSet;
							write.pTexelBufferView = &subresource.buffer_view;
							vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
						}

						if (!internal_state->uav.IsValid())
						{
							internal_state->uav = subresource;
							return -1;
						}
						internal_state->subresources_uav.push_back(subresource);
						return int(internal_state->subresources_uav.size() - 1);
					}
				}
				else
				{
					SDL_assert(0);
				}
			}
		}
		break;
		default:
			SDL_assert(0);
			break;
		}
		return -1;
	}

	void GraphicsDevice_Vulkan::DeleteSubresources(GPUResource* resource)
	{
		if (wiGraphicsGPUResourceIsTexture(resource))
		{
			auto internal_state = to_internal((Texture*)resource);
			internal_state->allocationhandler->destroylocker.lock();
			internal_state->destroy_subresources();
			internal_state->allocationhandler->destroylocker.unlock();
		}
		else if (wiGraphicsGPUResourceIsBuffer(resource))
		{
			auto internal_state = to_internal((GPUBuffer*)resource);
			internal_state->allocationhandler->destroylocker.lock();
			internal_state->destroy_subresources();
			internal_state->allocationhandler->destroylocker.unlock();
		}
	}

	int GraphicsDevice_Vulkan::GetDescriptorIndex(const GPUResource* resource, SubresourceType type, int subresource) const
	{
		if (!wiGraphicsGPUResourceIsValid(resource))
			return -1;

		switch (type)
		{
		default:
		case SubresourceType::SRV:
			if (wiGraphicsGPUResourceIsBuffer(resource))
			{
				const auto internal_state = to_internal<GPUBuffer>(resource);
				if (subresource < 0)
				{
					return internal_state->srv.index;
				}
				else
				{
					if (subresource >= (int)internal_state->subresources_srv.size())
						return -1;
					return internal_state->subresources_srv[subresource].index;
				}
			}
			else if (wiGraphicsGPUResourceIsTexture(resource))
			{
				const auto internal_state = to_internal<Texture>(resource);
				if (subresource < 0)
				{
					return internal_state->srv.index;
				}
				else
				{
					if (subresource >= (int)internal_state->subresources_srv.size())
						return -1;
					return internal_state->subresources_srv[subresource].index;
				}
			}
			else if (wiGraphicsGPUResourceIsAccelerationStructure(resource))
			{
				const auto internal_state = to_internal<RaytracingAccelerationStructure>(resource);
				return internal_state->index;
			}
			break;
		case SubresourceType::UAV:
			if (wiGraphicsGPUResourceIsBuffer(resource))
			{
				const auto internal_state = to_internal<GPUBuffer>(resource);
				if (subresource < 0)
				{
					return internal_state->uav.index;
				}
				else
				{
					if (subresource >= (int)internal_state->subresources_uav.size())
						return -1;
					return internal_state->subresources_uav[subresource].index;
				}
			}
			else if (wiGraphicsGPUResourceIsTexture(resource))
			{
				const auto internal_state = to_internal<Texture>(resource);
				if (subresource < 0)
				{
					return internal_state->uav.index;
				}
				else
				{
					if (subresource >= (int)internal_state->subresources_uav.size())
						return -1;
					return internal_state->subresources_uav[subresource].index;
				}
			}
			break;
		}

		return -1;
	}
	int GraphicsDevice_Vulkan::GetDescriptorIndex(const Sampler* sampler) const
	{
			if (!wiGraphicsSamplerIsValid(sampler))
			return -1;

		auto internal_state = to_internal(sampler);
		return internal_state->index;
	}

	void GraphicsDevice_Vulkan::WriteShadingRateValue(ShadingRate rate, void* dest) const
	{
		// How to compute shading rate value texel data:
		// https://www.khronos.org/registry/vulkan/specs/1.2-extensions/html/vkspec.html#primsrast-fragment-shading-rate-attachment

		switch (rate)
		{
		default:
		case ShadingRate::RATE_1X1:
			*(uint8_t*)dest = 0;
			break;
		case ShadingRate::RATE_1X2:
			*(uint8_t*)dest = 0x1;
			break;
		case ShadingRate::RATE_2X1:
			*(uint8_t*)dest = 0x4;
			break;
		case ShadingRate::RATE_2X2:
			*(uint8_t*)dest = 0x5;
			break;
		case ShadingRate::RATE_2X4:
			*(uint8_t*)dest = 0x6;
			break;
		case ShadingRate::RATE_4X2:
			*(uint8_t*)dest = 0x9;
			break;
		case ShadingRate::RATE_4X4:
			*(uint8_t*)dest = 0xa;
			break;
		}

	}
	void GraphicsDevice_Vulkan::WriteTopLevelAccelerationStructureInstance(const RaytracingAccelerationStructureDesc::TopLevel::Instance* instance, void* dest) const
	{
		VkAccelerationStructureInstanceKHR tmp = {};
		if (instance != nullptr)
		{
			tmp.transform = *(VkTransformMatrixKHR*)&instance->transform;
			tmp.instanceCustomIndex = instance->instance_id;
			tmp.mask = instance->instance_mask;
			tmp.instanceShaderBindingTableRecordOffset = instance->instance_contribution_to_hit_group_index;
			tmp.flags = instance->flags;

			SDL_assert(wiGraphicsGPUResourceIsAccelerationStructure(instance->bottom_level));
			auto internal_state = to_internal((RaytracingAccelerationStructure*)instance->bottom_level);
			tmp.accelerationStructureReference = internal_state->as_address;
		}
		std::memcpy(dest, &tmp, sizeof(tmp)); // memcpy whole structure into mapped pointer to avoid read from uncached memory
	}
	void GraphicsDevice_Vulkan::WriteShaderIdentifier(const RaytracingPipelineState* rtpso, uint32_t group_index, void* dest) const
	{
		vulkan_check(vkGetRayTracingShaderGroupHandlesKHR(device, to_internal(rtpso)->pipeline, group_index, 1, SHADER_IDENTIFIER_SIZE, dest));
	}

	void GraphicsDevice_Vulkan::SetName(GPUResource* pResource, const char* name) const
	{
		if (!debugUtils || !wiGraphicsGPUResourceIsValid(pResource))
			return;

		VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
		info.pObjectName = name;
		if (wiGraphicsGPUResourceIsTexture(pResource))
		{
			info.objectType = VK_OBJECT_TYPE_IMAGE;
			info.objectHandle = (uint64_t)to_internal<Texture>(pResource)->resource;
		}
		else if (wiGraphicsGPUResourceIsBuffer(pResource))
		{
			info.objectType = VK_OBJECT_TYPE_BUFFER;
			info.objectHandle = (uint64_t)to_internal<GPUBuffer>(pResource)->resource;
		}
		else if (wiGraphicsGPUResourceIsAccelerationStructure(pResource))
		{
			info.objectType = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;
			info.objectHandle = (uint64_t)to_internal<RaytracingAccelerationStructure>(pResource)->resource;
		}

		if (info.objectHandle == (uint64_t)VK_NULL_HANDLE)
			return;

		vulkan_check(vkSetDebugUtilsObjectNameEXT(device, &info));
	}
	void GraphicsDevice_Vulkan::SetName(Shader* shader, const char* name) const
	{
		if (!debugUtils || !wiGraphicsShaderIsValid(shader))
			return;

		VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
		info.pObjectName = name;
		info.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
		info.objectHandle = (uint64_t)to_internal(shader)->shaderModule;

		if (info.objectHandle == (uint64_t)VK_NULL_HANDLE)
			return;

		vulkan_check(vkSetDebugUtilsObjectNameEXT(device, &info));
	}

	CommandList GraphicsDevice_Vulkan::BeginCommandList(QUEUE_TYPE queue)
	{
		cmd_locker.lock();
		uint32_t cmd_current = cmd_count++;
		if (cmd_current >= commandlists.size())
		{
			commandlists.push_back(cmd_allocator.allocate());
		}
		CommandList cmd;
		cmd.internal_state = commandlists[cmd_current];
		cmd_locker.unlock();

		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		commandlist.reset(GetBufferIndex());
		commandlist.queue = queue;
		commandlist.id = cmd_current;

		if (commandlist.GetCommandBuffer() == VK_NULL_HANDLE)
		{
			// need to create one more command list:

			for (uint32_t buffer = 0; buffer < BUFFERCOUNT; ++buffer)
			{
				VkCommandPoolCreateInfo poolInfo = {};
				poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
				switch (queue)
				{
				case wi::QUEUE_GRAPHICS:
					poolInfo.queueFamilyIndex = graphicsFamily;
					break;
				case wi::QUEUE_COMPUTE:
					poolInfo.queueFamilyIndex = computeFamily;
					break;
				case wi::QUEUE_COPY:
					poolInfo.queueFamilyIndex = copyFamily;
					break;
				case wi::QUEUE_VIDEO_DECODE:
					poolInfo.queueFamilyIndex = videoFamily;
					break;
				default:
					SDL_assert(0); // queue type not handled
					break;
				}
				poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

				vulkan_check(vkCreateCommandPool(device, &poolInfo, nullptr, &commandlist.commandPools[buffer][queue]));

				VkCommandBufferAllocateInfo commandBufferInfo = {};
				commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
				commandBufferInfo.commandBufferCount = 1;
				commandBufferInfo.commandPool = commandlist.commandPools[buffer][queue];
				commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

				vulkan_check(vkAllocateCommandBuffers(device, &commandBufferInfo, &commandlist.commandBuffers[buffer][queue]));

				commandlist.binder_pools[buffer].init(this);
			}

			commandlist.binder.init(this);
		}

		vulkan_check(vkResetCommandPool(device, commandlist.GetCommandPool(), 0));

		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		beginInfo.pInheritanceInfo = nullptr; // Optional

		vulkan_check(vkBeginCommandBuffer(commandlist.GetCommandBuffer(), &beginInfo));

		if (queue == QUEUE_GRAPHICS)
		{
			vkCmdSetRasterizerDiscardEnable(commandlist.GetCommandBuffer(), VK_FALSE);

			VkViewport vp = {};
			vp.width = 1;
			vp.height = 1;
			vp.maxDepth = 1;
			vkCmdSetViewportWithCount(commandlist.GetCommandBuffer(), 1, &vp);

			VkRect2D scissor;
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent.width = 65535;
			scissor.extent.height = 65535;
			vkCmdSetScissorWithCount(commandlist.GetCommandBuffer(), 1, &scissor);

			float blendConstants[] = { 1,1,1,1 };
			vkCmdSetBlendConstants(commandlist.GetCommandBuffer(), blendConstants);

			vkCmdSetStencilReference(commandlist.GetCommandBuffer(), VK_STENCIL_FRONT_AND_BACK, commandlist.prev_stencilref);

			if (features2.features.depthBounds == VK_TRUE)
			{
				vkCmdSetDepthBounds(commandlist.GetCommandBuffer(), 0.0f, 1.0f);
			}

			const VkDeviceSize zero = {};
			vkCmdBindVertexBuffers2(commandlist.GetCommandBuffer(), 0, 1, &nullBuffer, &zero, &zero, &zero);

			if (CheckCapability(GraphicsDeviceCapability::VARIABLE_RATE_SHADING))
			{
				VkExtent2D fragmentSize = {};
				fragmentSize.width = 1;
				fragmentSize.height = 1;

				VkFragmentShadingRateCombinerOpKHR combiner[] = {
					VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
					VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR
				};

				vkCmdSetFragmentShadingRateKHR(
					commandlist.GetCommandBuffer(),
					&fragmentSize,
					combiner
				);
			}
		}

		return cmd;
	}
	bool GraphicsDevice_Vulkan::SupportsSubmissionTokens() const
	{
		return timeline_semaphore_supported;
	}

	void GraphicsDevice_Vulkan::WarnMissingTimelineSemaphore(const char* caller) const
	{
		if (!timeline_semaphore_warning_emitted.exchange(true))
		{
			SDL_LogWarn(
				SDL_LOG_CATEGORY_APPLICATION,
				"[Wicked::Vulkan] timeline semaphores are unavailable in %s; falling back to explicit CPU-synchronized submission dependencies",
				caller != nullptr ? caller : "unknown caller");
		}
	}

	SubmissionToken GraphicsDevice_Vulkan::SubmitCommandListsInternal(bool defer_presents)
	{
		const bool timeline_supported = SupportsSubmissionTokens();
		const bool token_mode_requested = true;
		if (!timeline_supported)
		{
			WarnMissingTimelineSemaphore("SubmitCommandListsEx");
		}

		copyAllocator.recycle_completed();

		SubmissionToken tokens = {};
		bool queue_has_work[QUEUE_COUNT] = {};
		const uint32_t submit_index = GetBufferIndex();
		for (int q = 0; q < QUEUE_COUNT; ++q)
		{
			frame_queue_active[submit_index][q] = false;
		}

		SubmissionToken pending_upload_tokens = {};
		{
			std::scoped_lock lock(upload_token_locker);
			pending_upload_tokens = pending_implicit_uploads;
			pending_implicit_uploads = {};
		}

		// Submit resource initialization transitions:
		{
			TransitionHandler& transition_handler = GetTransitionHandler();
			std::scoped_lock lck(transitionLocker);
			if (arrlenu(init_transitions) > 0)
			{
				if (transition_handler.commandBuffer == VK_NULL_HANDLE)
				{
					VkCommandPoolCreateInfo poolInfo = {};
					poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
					poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
					poolInfo.queueFamilyIndex = graphicsFamily;
					vulkan_check(vkCreateCommandPool(device, &poolInfo, nullptr, &transition_handler.commandPool));

					VkCommandBufferAllocateInfo commandBufferInfo = {};
					commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
					commandBufferInfo.commandBufferCount = 1;
					commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
					commandBufferInfo.commandPool = transition_handler.commandPool;
					vulkan_check(vkAllocateCommandBuffers(device, &commandBufferInfo, &transition_handler.commandBuffer));

					for (int i = 0; i < SDL_arraysize(transition_handler.semaphores); ++i)
					{
						VkSemaphoreCreateInfo info = {};
						info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
						vulkan_check(vkCreateSemaphore(device, &info, nullptr, &transition_handler.semaphores[i]));
					}
				}
				VkCommandBufferBeginInfo beginInfo = {};
				beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				beginInfo.pInheritanceInfo = nullptr;
				vulkan_check(vkResetCommandPool(device, transition_handler.commandPool, 0));
				vulkan_check(vkBeginCommandBuffer(transition_handler.commandBuffer, &beginInfo));
				VkDependencyInfo dependencyInfo = {};
				dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
				dependencyInfo.imageMemoryBarrierCount = (uint32_t)arrlenu(init_transitions);
				dependencyInfo.pImageMemoryBarriers = init_transitions;
				vkCmdPipelineBarrier2(transition_handler.commandBuffer, &dependencyInfo);
				vulkan_check(vkEndCommandBuffer(transition_handler.commandBuffer));
				CommandQueue& queue = queues[QUEUE_GRAPHICS];
				VkCommandBufferSubmitInfo cmd_submit = {};
				cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
				cmd_submit.commandBuffer = transition_handler.commandBuffer;
				arrput(queue.submit_cmds, cmd_submit);
				queue_has_work[QUEUE_GRAPHICS] = true;
				for (int q = QUEUE_GRAPHICS + 1; q < QUEUE_COUNT; ++q)
				{
					if (queues[q].queue == VK_NULL_HANDLE)
						continue;
					VkSemaphore sema = transition_handler.semaphores[q - 1];
					queue.signal(sema);
					queues[q].wait(sema);
				}
				queue.submit(this, VK_NULL_HANDLE, !token_mode_requested, 0, nullptr, !defer_presents);
				arrfree(init_transitions);
				init_transitions = nullptr;
			}
		}

		// Queue waits for pending implicit uploads:
		if (pending_upload_tokens.IsValid())
		{
			for (uint32_t dst = 0; dst < QUEUE_COUNT; ++dst)
			{
				if (queues[dst].queue == VK_NULL_HANDLE)
					continue;
				for (uint32_t src = 0; src < QUEUE_COUNT; ++src)
				{
					if ((pending_upload_tokens.queue_mask & (1u << src)) == 0)
						continue;
					QueueSyncPoint point = pending_upload_tokens.Get((QUEUE_TYPE)src);
					if (!point.IsValid() || src == dst)
						continue;
					if (!timeline_supported || queues[src].timeline_semaphore == VK_NULL_HANDLE)
					{
						WaitQueuePoint(point);
						continue;
					}
					queues[dst].wait(queues[src].timeline_semaphore, point.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
				}
			}
		}

		// Submit current frame:
		{
			uint32_t cmd_last = cmd_count;
			cmd_count = 0;
			for (uint32_t cmd = 0; cmd < cmd_last; ++cmd)
			{
				CommandList_Vulkan& commandlist = *commandlists[cmd];
				vulkan_check(vkEndCommandBuffer(commandlist.GetCommandBuffer()));

				CommandQueue& queue = queues[commandlist.queue];
				const bool dependency = !commandlist.signals.empty() || !commandlist.waits.empty();

				if (dependency)
				{
					// If the current commandlist must resolve a dependency, then previous ones will be submitted before doing that:
					//	This improves GPU utilization because not the whole batch of command lists will need to synchronize, but only the one that handles it
					queue.submit(this, VK_NULL_HANDLE, !token_mode_requested, 0, nullptr, !defer_presents);
				}

				VkCommandBufferSubmitInfo cbSubmitInfo = {};
				cbSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
				cbSubmitInfo.commandBuffer = commandlist.GetCommandBuffer();
				arrput(queue.submit_cmds, cbSubmitInfo);
				queue_has_work[commandlist.queue] = true;

				for (auto& swapchain : commandlist.prev_swapchains)
				{
					auto internal_state = to_internal(&swapchain);

					VkSemaphoreSubmitInfo waitSemaphore = {};
					waitSemaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
					waitSemaphore.semaphore = internal_state->swapchainAcquireSemaphores[internal_state->swapChainAcquireSemaphoreIndex];
					waitSemaphore.value = 0; // not a timeline semaphore
					waitSemaphore.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
					arrput(queue.submit_waitSemaphoreInfos, waitSemaphore);

					VkSemaphoreSubmitInfo signalSemaphore = {};
					signalSemaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
					signalSemaphore.semaphore = internal_state->swapchainReleaseSemaphores[internal_state->swapChainImageIndex];
					signalSemaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
					signalSemaphore.value = 0; // not a timeline semaphore
					arrput(queue.submit_signalSemaphoreInfos, signalSemaphore);

					queue.swapchain_updates.push_back(swapchain);
					arrput(queue.swapchains, internal_state->swapChain);
					arrput(queue.swapchainImageIndices, internal_state->swapChainImageIndex);
					arrput(queue.swapchainWaitSemaphores, signalSemaphore.semaphore);
				}

				if (dependency)
				{
					for (auto& semaphore : commandlist.waits)
					{
						// Wait for command list dependency:
						queue.wait(semaphore);
						// semaphore is not recycled here, only the signals recycle themselves because wait will use the same
					}
					commandlist.waits.clear();

					for (auto& semaphore : commandlist.signals)
					{
						// Signal this command list's completion:
						queue.signal(semaphore);
						// recycle semaphore
						free_semaphore(semaphore);
					}
					commandlist.signals.clear();

					queue.submit(this, VK_NULL_HANDLE, !token_mode_requested, 0, nullptr, !defer_presents);
				}

				for (auto& x : commandlist.pipelines_worker)
				{
					bool found = false;
					for (auto& item : pipelines_global)
					{
						if (item.first == x.first)
						{
							found = true;
							break;
						}
					}
					if (!found)
					{
						pipelines_global.emplace_back(x.first, x.second);
					}
				}
				commandlist.pipelines_worker.clear();
			}

				// final submits with fences:
				for (int q = 0; q < QUEUE_COUNT; ++q)
				{
					if (queues[q].queue == VK_NULL_HANDLE)
						continue;

					uint64_t timeline_signal_value = 0;
					uint64_t* out_timeline_signal_value = nullptr;
					if (queue_has_work[q] && queues[q].timeline_semaphore != VK_NULL_HANDLE)
					{
						out_timeline_signal_value = &timeline_signal_value;
					}

					const bool submitted = queues[q].submit(this, frame_fence[submit_index][q], !token_mode_requested, 0, out_timeline_signal_value, !defer_presents);
					frame_queue_active[submit_index][q] = submitted;

					if (!submitted || !queue_has_work[q])
					{
						if (!timeline_supported)
						{
							queue_timeline_value_per_buffer[submit_index][q].store(0, std::memory_order_release);
						}
						continue;
					}

					uint64_t queue_point = timeline_signal_value;
					if (queue_point == 0)
					{
						queue_point = queue_timeline_submitted_fallback[q].fetch_add(1, std::memory_order_relaxed) + 1;
						queue_timeline_value_per_buffer[submit_index][q].store(queue_point, std::memory_order_release);
					}
					{
						std::scoped_lock lock(allocationhandler->destroylocker);
						allocationhandler->submitted_queue_values[q] = queue_point;
					}
					if (token_mode_requested)
					{
						tokens.Merge(QueueSyncPoint{ (QUEUE_TYPE)q, queue_point });
					}
				}
			}

		// From here, we begin a new frame, this affects GetBufferIndex()!
		FRAMECOUNT++;

		// Optional legacy frame rollover wait. Engine-owned buffering can disable this path.
		if (IsBackendFrameRolloverWaitsEnabled() && FRAMECOUNT >= BUFFERCOUNT)
		{
			const uint32_t bufferindex = GetBufferIndex();
			VkFence waitFences[QUEUE_COUNT] = {};
			uint32_t waitFenceCount = 0;
			VkFence resetFences[QUEUE_COUNT] = {};
			uint32_t resetFenceCount = 0;
				for (int queue = 0; queue < QUEUE_COUNT; ++queue)
				{
					if (!frame_queue_active[bufferindex][queue])
						continue;
					VkFence fence = frame_fence[bufferindex][queue];
					if (fence == VK_NULL_HANDLE)
						continue;
					resetFences[resetFenceCount++] = fence;
					if (vkGetFenceStatus(device, fence) == VK_SUCCESS)
					{
						const uint64_t retired = queue_timeline_value_per_buffer[bufferindex][queue].load(std::memory_order_acquire);
						if (retired > 0)
						{
							uint64_t completed = queue_timeline_completed_fallback[queue].load(std::memory_order_relaxed);
							while (completed < retired && !queue_timeline_completed_fallback[queue].compare_exchange_weak(completed, retired, std::memory_order_release, std::memory_order_relaxed))
							{
							}
						}
						continue;
					}
					waitFences[waitFenceCount++] = fence;
				}
			if (waitFenceCount > 0)
			{
				while (vulkan_check(vkWaitForFences(device, waitFenceCount, waitFences, VK_TRUE, timeout_value)) == VK_TIMEOUT)
				{
					VULKAN_LOG_ERROR(
						"[SubmitCommandLists] vkWaitForFences resulted in VK_TIMEOUT, fence statuses:\nQUEUE_GRAPHICS = %s\nQUEUE_COMPUTE = %s\nQUEUE_COPY = %s\nQUEUE_VIDEO_DECODE = %s",
						frame_fence[bufferindex][QUEUE_GRAPHICS] == VK_NULL_HANDLE ? "OK" : string_VkResult(vkGetFenceStatus(device, frame_fence[bufferindex][QUEUE_GRAPHICS])),
						frame_fence[bufferindex][QUEUE_COMPUTE] == VK_NULL_HANDLE ? "OK" : string_VkResult(vkGetFenceStatus(device, frame_fence[bufferindex][QUEUE_COMPUTE])),
						frame_fence[bufferindex][QUEUE_COPY] == VK_NULL_HANDLE ? "OK" : string_VkResult(vkGetFenceStatus(device, frame_fence[bufferindex][QUEUE_COPY])),
						frame_fence[bufferindex][QUEUE_VIDEO_DECODE] == VK_NULL_HANDLE ? "OK" : string_VkResult(vkGetFenceStatus(device, frame_fence[bufferindex][QUEUE_VIDEO_DECODE]))
					);
						std::this_thread::yield();
					}
				}
				for (int queue = 0; queue < QUEUE_COUNT; ++queue)
				{
					if (!frame_queue_active[bufferindex][queue])
						continue;
					VkFence fence = frame_fence[bufferindex][queue];
					if (fence == VK_NULL_HANDLE || vkGetFenceStatus(device, fence) != VK_SUCCESS)
						continue;
					const uint64_t retired = queue_timeline_value_per_buffer[bufferindex][queue].load(std::memory_order_acquire);
					if (retired == 0)
						continue;
					uint64_t completed = queue_timeline_completed_fallback[queue].load(std::memory_order_relaxed);
					while (completed < retired && !queue_timeline_completed_fallback[queue].compare_exchange_weak(completed, retired, std::memory_order_release, std::memory_order_relaxed))
					{
					}
				}
				if (resetFenceCount > 0)
				{
					vulkan_check(vkResetFences(device, resetFenceCount, resetFences));
				}
			}

		for (int q = 0; q < QUEUE_COUNT; ++q)
		{
			if (queues[q].queue == VK_NULL_HANDLE)
			{
				queues[q].clear();
			}
		}

		allocationhandler->Update();
		copyAllocator.recycle_completed();
		return tokens;
	}

	SubmissionToken GraphicsDevice_Vulkan::SubmitCommandListsWithDesc(const SubmitDesc& desc, bool defer_presents)
	{
		bool partial_submit = desc.command_lists != nullptr && desc.command_list_count > 0;
		bool has_submit_batch = true;
		std::vector<CommandList_Vulkan*> remaining_commandlists;
		if (partial_submit)
		{
			std::scoped_lock lock(cmd_locker);
			std::vector<CommandList_Vulkan*> open_commandlists;
			open_commandlists.reserve(cmd_count);
			for (uint32_t i = 0; i < cmd_count; ++i)
			{
				open_commandlists.push_back(commandlists[i]);
			}

			std::vector<CommandList_Vulkan*> submit_commandlists;
			submit_commandlists.reserve(desc.command_list_count);
			auto is_in_open = [&](CommandList_Vulkan* candidate) -> bool
			{
				return candidate != nullptr && std::find(open_commandlists.begin(), open_commandlists.end(), candidate) != open_commandlists.end();
			};
			auto contains_submit = [&](CommandList_Vulkan* candidate) -> bool
			{
				return std::find(submit_commandlists.begin(), submit_commandlists.end(), candidate) != submit_commandlists.end();
			};

			for (uint32_t i = 0; i < desc.command_list_count; ++i)
			{
				const CommandList cmd = desc.command_lists[i];
				if (!cmd.IsValid())
					continue;
				CommandList_Vulkan* commandlist = (CommandList_Vulkan*)cmd.internal_state;
				if (!is_in_open(commandlist) || contains_submit(commandlist))
					continue;
				submit_commandlists.push_back(commandlist);
			}

			// Dependency closure: if a selected consumer waits on a producer, include that producer too.
			bool changed = true;
			while (changed)
			{
				changed = false;
				for (size_t ci = 0; ci < submit_commandlists.size(); ++ci)
				{
					CommandList_Vulkan* consumer = submit_commandlists[ci];
					if (consumer == nullptr)
						continue;
					for (VkSemaphore wait_sem : consumer->waits)
					{
						if (wait_sem == VK_NULL_HANDLE)
							continue;
						for (CommandList_Vulkan* producer : open_commandlists)
						{
							if (producer == nullptr || contains_submit(producer))
								continue;
							bool found = false;
							for (VkSemaphore signal_sem : producer->signals)
							{
								if (signal_sem == wait_sem)
								{
									found = true;
									break;
								}
							}
							if (found)
							{
								submit_commandlists.push_back(producer);
								changed = true;
								break;
							}
						}
					}
				}
			}

			if (submit_commandlists.empty())
			{
				has_submit_batch = false;
			}
			else
			{
				remaining_commandlists.reserve(open_commandlists.size());
				auto in_submit = [&](CommandList_Vulkan* candidate) -> bool
				{
					return std::find(submit_commandlists.begin(), submit_commandlists.end(), candidate) != submit_commandlists.end();
				};
				for (CommandList_Vulkan* commandlist : open_commandlists)
				{
					if (!in_submit(commandlist))
					{
						remaining_commandlists.push_back(commandlist);
					}
				}

				// Drop waits/signals in remaining-open commandlists that target submitted commandlists.
				auto has_remaining_signal = [&](VkSemaphore sem) -> bool
				{
					for (CommandList_Vulkan* producer : remaining_commandlists)
					{
						if (producer == nullptr)
							continue;
						for (VkSemaphore signal_sem : producer->signals)
						{
							if (signal_sem == sem)
								return true;
						}
					}
					return false;
				};
				for (CommandList_Vulkan* consumer : remaining_commandlists)
				{
					std::deque<VkSemaphore> kept_waits;
					for (VkSemaphore wait_sem : consumer->waits)
					{
						if (wait_sem != VK_NULL_HANDLE && has_remaining_signal(wait_sem))
						{
							kept_waits.push_back(wait_sem);
						}
					}
					consumer->waits.swap(kept_waits);
				}

				auto has_remaining_wait = [&](VkSemaphore sem) -> bool
				{
					for (CommandList_Vulkan* consumer : remaining_commandlists)
					{
						if (consumer == nullptr)
							continue;
						for (VkSemaphore wait_sem : consumer->waits)
						{
							if (wait_sem == sem)
								return true;
						}
					}
					return false;
				};
				for (CommandList_Vulkan* producer : remaining_commandlists)
				{
					std::deque<VkSemaphore> kept_signals;
					for (VkSemaphore signal_sem : producer->signals)
					{
						if (signal_sem != VK_NULL_HANDLE && has_remaining_wait(signal_sem))
						{
							kept_signals.push_back(signal_sem);
						}
						else if (signal_sem != VK_NULL_HANDLE)
						{
							free_semaphore(signal_sem);
						}
					}
					producer->signals.swap(kept_signals);
				}

				cmd_count = (uint32_t)submit_commandlists.size();
				for (uint32_t i = 0; i < cmd_count; ++i)
				{
					commandlists[i] = submit_commandlists[i];
					commandlists[i]->id = i;
				}
			}
		}

		if (partial_submit && !has_submit_batch)
		{
			return {};
		}

		if ((desc.submission_dependencies != nullptr && desc.submission_dependency_count > 0) ||
			(desc.queue_dependencies != nullptr && desc.queue_dependency_count > 0))
		{
			if (!SupportsSubmissionTokens())
			{
				// Explicit fallback for non-timeline Vulkan paths: honor SubmitDesc dependencies on CPU.
				for (uint32_t i = 0; i < desc.queue_dependency_count; ++i)
				{
					WaitQueuePoint(desc.queue_dependencies[i].point);
				}
				for (uint32_t i = 0; i < desc.submission_dependency_count; ++i)
				{
					WaitSubmission(desc.submission_dependencies[i]);
				}
			}
			else
			{
				std::scoped_lock lock(upload_token_locker);
				for (uint32_t i = 0; i < desc.submission_dependency_count; ++i)
				{
					pending_implicit_uploads.Merge(desc.submission_dependencies[i]);
				}
				for (uint32_t i = 0; i < desc.queue_dependency_count; ++i)
				{
					SubmissionToken token = {};
					token.Merge(desc.queue_dependencies[i].point);
					pending_implicit_uploads.Merge(token);
				}
			}
		}
		SubmissionToken token = SubmitCommandListsInternal(defer_presents);
		if (partial_submit)
		{
			cmd_locker.lock();
			cmd_count = (uint32_t)remaining_commandlists.size();
			for (uint32_t i = 0; i < cmd_count; ++i)
			{
				commandlists[i] = remaining_commandlists[i];
				commandlists[i]->id = i;
			}
			cmd_locker.unlock();
		}
		if (desc.throttle_cpu && token.IsValid())
		{
			const uint64_t budget = desc.max_inflight_per_queue > 0 ? desc.max_inflight_per_queue : 2u;
			for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
			{
				if ((token.queue_mask & (1u << q)) == 0)
					continue;
				const uint64_t submitted = token.values[q];
				if (submitted <= budget)
					continue;
				WaitQueuePoint(QueueSyncPoint{ (QUEUE_TYPE)q, submitted - budget });
			}
		}
		return token;
	}

	SubmissionToken GraphicsDevice_Vulkan::SubmitCommandListsEx(const SubmitDesc& desc)
	{
		return SubmitCommandListsWithDesc(desc, false);
	}

	SubmissionToken GraphicsDevice_Vulkan::QueueSubmit(QUEUE_TYPE type, const QueueSubmitDesc& desc)
	{
		if (type >= QUEUE_COUNT || queues[type].queue == VK_NULL_HANDLE)
		{
			return {};
		}
		if (desc.command_lists == nullptr || desc.command_list_count == 0)
		{
			return {};
		}

		std::vector<CommandList_Vulkan*> open_commandlists;
		std::vector<CommandList_Vulkan*> filtered_commandlists_internal;
		std::unique_ptr<CommandList[]> filtered_command_lists(new CommandList[desc.command_list_count]);
		uint32_t filtered_command_list_count = 0;

		{
			std::scoped_lock lock(cmd_locker);
			open_commandlists.reserve(cmd_count);
			for (uint32_t i = 0; i < cmd_count; ++i)
			{
				open_commandlists.push_back(commandlists[i]);
			}

			auto is_open_commandlist = [&](const CommandList_Vulkan* candidate) -> bool
			{
				return std::find(open_commandlists.begin(), open_commandlists.end(), candidate) != open_commandlists.end();
			};

			for (uint32_t i = 0; i < desc.command_list_count; ++i)
			{
				const CommandList cmd = desc.command_lists[i];
				if (!wiGraphicsCommandListIsValid(cmd))
					continue;

				CommandList_Vulkan* commandlist = (CommandList_Vulkan*)cmd.internal_state;
				if (commandlist == nullptr || !is_open_commandlist(commandlist))
					continue;
				if (commandlist->queue != type)
				{
					VULKAN_LOG_ERROR(
						"[Vulkan][QueueSubmit] Rejected command list from queue %s in submit to queue %s",
						GetQueueTypeName(commandlist->queue),
						GetQueueTypeName(type)
					);
#ifndef NDEBUG
					SDL_assert(false && "QueueSubmit requires command lists recorded for the same queue type");
#endif
					continue;
				}
				if (std::find(filtered_commandlists_internal.begin(), filtered_commandlists_internal.end(), commandlist) != filtered_commandlists_internal.end())
					continue;

				filtered_command_lists[filtered_command_list_count++] = cmd;
				filtered_commandlists_internal.push_back(commandlist);
			}
		}
		if (filtered_command_list_count == 0)
		{
			return {};
		}

		for (const CommandList_Vulkan* consumer : filtered_commandlists_internal)
		{
			if (consumer == nullptr)
				continue;
			for (VkSemaphore wait_sem : consumer->waits)
			{
				if (wait_sem == VK_NULL_HANDLE)
					continue;
				for (const CommandList_Vulkan* producer : open_commandlists)
				{
					if (producer == nullptr || producer->signals.empty())
						continue;
					for (VkSemaphore signal_sem : producer->signals)
					{
						if (signal_sem != wait_sem)
							continue;
						if (producer->queue != type)
						{
							VULKAN_LOG_ERROR(
								"[Vulkan][QueueSubmit] Rejected cross-queue command list dependency: consumer queue=%s producer queue=%s",
								GetQueueTypeName(type),
								GetQueueTypeName(producer->queue)
							);
#ifndef NDEBUG
							SDL_assert(false && "QueueSubmit command list dependencies must stay within the submitted queue type");
#endif
							return {};
						}
						break;
					}
				}
			}
		}

		SubmitDesc submit = {};
		submit.command_lists = filtered_command_lists.get();
		submit.command_list_count = filtered_command_list_count;
		submit.submission_dependencies = desc.wait_submissions;
		submit.submission_dependency_count = desc.wait_submission_count;
		submit.throttle_cpu = desc.throttle_cpu;
		submit.max_inflight_per_queue = desc.max_inflight_per_queue;

		std::unique_ptr<QueueDependency[]> queue_dependencies;
		if (desc.wait_points != nullptr && desc.wait_point_count > 0)
		{
			queue_dependencies.reset(new QueueDependency[desc.wait_point_count]);
			for (uint32_t i = 0; i < desc.wait_point_count; ++i)
			{
				queue_dependencies[i].point = desc.wait_points[i];
			}
			submit.queue_dependencies = queue_dependencies.get();
			submit.queue_dependency_count = desc.wait_point_count;
		}

		return SubmitCommandListsWithDesc(submit, true);
	}

	bool GraphicsDevice_Vulkan::AcquireNextImage(SwapChain* swapchain, AcquireDesc* desc)
	{
		if (!wiGraphicsSwapChainIsValid(swapchain))
		{
			return false;
		}

		auto internal_state = to_internal(swapchain);
		if (internal_state == nullptr || internal_state->swapChain == VK_NULL_HANDLE || internal_state->swapchainAcquireSemaphores.empty())
		{
			return false;
		}

		VkResult res = VK_SUCCESS;
		uint32_t image_index = 0;
		internal_state->locker.lock();
		if (internal_state->explicit_acquire_pending)
		{
			image_index = internal_state->swapChainImageIndex;
			internal_state->locker.unlock();
		}
		else
		{
			internal_state->swapChainAcquireSemaphoreIndex = (internal_state->swapChainAcquireSemaphoreIndex + 1) % internal_state->swapchainAcquireSemaphores.size();
			do
			{
				res = vkAcquireNextImageKHR(
					device,
					internal_state->swapChain,
					timeout_value,
					internal_state->swapchainAcquireSemaphores[internal_state->swapChainAcquireSemaphoreIndex],
					VK_NULL_HANDLE,
					&internal_state->swapChainImageIndex
				);
				if (res == VK_TIMEOUT)
				{
					VULKAN_LOG_ERROR("vkAcquireNextImageKHR resulted in VK_TIMEOUT, retrying");
					std::this_thread::yield();
				}
			} while (res == VK_TIMEOUT);
			if (res == VK_SUCCESS)
			{
				internal_state->explicit_acquire_pending = true;
				image_index = internal_state->swapChainImageIndex;
			}
			internal_state->locker.unlock();
		}

		if (res != VK_SUCCESS)
		{
			if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
			{
				{
					std::scoped_lock lock(allocationhandler->destroylocker);
					for (auto& x : internal_state->swapchainAcquireSemaphores)
					{
						allocationhandler->Retire(allocationhandler->destroyer_semaphores, x);
					}
				}
				internal_state->swapchainAcquireSemaphores.clear();
				if (CreateSwapChainInternal(internal_state, physicalDevice, device, allocationhandler))
				{
					return AcquireNextImage(swapchain, desc);
				}
			}
			SDL_assert(0);
			return false;
		}

		if (desc != nullptr)
		{
			desc->imageIndex = image_index;
			if (desc->signal_point != nullptr)
			{
				*desc->signal_point = GetLastSubmittedQueuePoint(desc->queue);
			}
		}

		return true;
	}

	void GraphicsDevice_Vulkan::QueuePresent(QUEUE_TYPE presentQueue, const QueuePresentDesc& desc)
	{
		if (presentQueue >= QUEUE_COUNT)
			return;

		CommandQueue& queue = queues[presentQueue];
		if (queue.queue == VK_NULL_HANDLE)
			return;

		VkSwapchainKHR* original_swapchains = queue.swapchains;
		uint32_t* original_image_indices = queue.swapchainImageIndices;
		VkSemaphore* original_wait_semaphores = queue.swapchainWaitSemaphores;
		std::deque<SwapChain> original_updates = std::move(queue.swapchain_updates);
		queue.swapchains = nullptr;
		queue.swapchainImageIndices = nullptr;
		queue.swapchainWaitSemaphores = nullptr;
		queue.swapchain_updates.clear();

		VkSwapchainKHR* present_swapchains = nullptr;
		uint32_t* present_image_indices = nullptr;
		VkSemaphore* present_wait_semaphores = nullptr;
		std::deque<SwapChain> present_updates;

		VkSwapchainKHR* remaining_swapchains = nullptr;
		uint32_t* remaining_image_indices = nullptr;
		VkSemaphore* remaining_wait_semaphores = nullptr;
		std::deque<SwapChain> remaining_updates;

		const auto* target_swapchain = desc.swapchain != nullptr ? to_internal(desc.swapchain) : nullptr;
		size_t last_match = std::numeric_limits<size_t>::max();
		const size_t pending_count = (size_t)arrlenu(original_swapchains);
		if (target_swapchain != nullptr)
		{
			for (size_t i = 0; i < pending_count; ++i)
			{
				if (original_swapchains[i] == target_swapchain->swapChain)
				{
					last_match = i;
				}
			}
		}

		for (size_t i = 0; i < pending_count; ++i)
		{
			const bool is_target = target_swapchain == nullptr || original_swapchains[i] == target_swapchain->swapChain;
			const bool select_for_present =
				target_swapchain == nullptr ? is_target : (is_target && i == last_match);
			const bool keep_for_later =
				target_swapchain != nullptr && !is_target;

			if (!select_for_present && !keep_for_later)
				continue;

			std::deque<SwapChain>& dst_updates = select_for_present ? present_updates : remaining_updates;
			VkSwapchainKHR*& dst_swapchains = select_for_present ? present_swapchains : remaining_swapchains;
			uint32_t*& dst_indices = select_for_present ? present_image_indices : remaining_image_indices;
			VkSemaphore*& dst_wait_semaphores = select_for_present ? present_wait_semaphores : remaining_wait_semaphores;

			if (i < original_updates.size())
			{
				dst_updates.push_back(original_updates[i]);
			}
			arrput(dst_swapchains, original_swapchains[i]);
			arrput(dst_indices, original_image_indices[i]);
			arrput(dst_wait_semaphores, original_wait_semaphores[i]);
		}

		arrfree(original_swapchains);
		arrfree(original_image_indices);
		arrfree(original_wait_semaphores);

		if (arrlenu(present_swapchains) == 0)
		{
			queue.swapchains = remaining_swapchains;
			queue.swapchainImageIndices = remaining_image_indices;
			queue.swapchainWaitSemaphores = remaining_wait_semaphores;
			queue.swapchain_updates = std::move(remaining_updates);
			return;
		}

		queue.swapchains = present_swapchains;
		queue.swapchainImageIndices = present_image_indices;
		queue.swapchainWaitSemaphores = present_wait_semaphores;
		queue.swapchain_updates = std::move(present_updates);

		uint64_t dependency_max[QUEUE_COUNT] = {};
		auto merge_dependency = [&](QueueSyncPoint point) {
			if (!point.IsValid() || point.queue >= QUEUE_COUNT)
				return;
			dependency_max[point.queue] = std::max(dependency_max[point.queue], point.value);
		};

		for (uint32_t i = 0; desc.wait_points != nullptr && i < desc.wait_point_count; ++i)
		{
			merge_dependency(desc.wait_points[i]);
		}
		for (uint32_t i = 0; desc.wait_submissions != nullptr && i < desc.wait_submission_count; ++i)
		{
			const SubmissionToken& token = desc.wait_submissions[i];
			for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
			{
				if ((token.queue_mask & (1u << q)) == 0 || token.values[q] == 0)
					continue;
				merge_dependency(QueueSyncPoint{ (QUEUE_TYPE)q, token.values[q] });
			}
		}

		bool bridge_submit_required = false;
		for (uint32_t producer_queue = 0; producer_queue < QUEUE_COUNT; ++producer_queue)
		{
			const uint64_t value = dependency_max[producer_queue];
			if (value == 0)
				continue;
			QueueSyncPoint point = QueueSyncPoint{ (QUEUE_TYPE)producer_queue, value };
			if (SupportsSubmissionTokens() && queues[producer_queue].timeline_semaphore != VK_NULL_HANDLE)
			{
				queue.wait(queues[producer_queue].timeline_semaphore, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
				bridge_submit_required = true;
			}
			else
			{
				WaitQueuePoint(point);
			}
		}

		VkSemaphore bridge_wait_semaphore = VK_NULL_HANDLE;
		if (bridge_submit_required)
		{
			bridge_wait_semaphore = new_semaphore();
			queue.signal(bridge_wait_semaphore, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
			arrput(queue.swapchainWaitSemaphores, bridge_wait_semaphore);
		}

		queue.submit(this, VK_NULL_HANDLE, false, 0, nullptr, true);

		if (bridge_wait_semaphore != VK_NULL_HANDLE)
		{
			free_semaphore(bridge_wait_semaphore);
		}

		queue.swapchains = remaining_swapchains;
		queue.swapchainImageIndices = remaining_image_indices;
		queue.swapchainWaitSemaphores = remaining_wait_semaphores;
		queue.swapchain_updates = std::move(remaining_updates);
	}

	bool GraphicsDevice_Vulkan::IsQueuePointComplete(QueueSyncPoint point) const
	{
		if (!point.IsValid())
			return true;
		if (point.queue >= QUEUE_COUNT)
			return true;
		if (queues[point.queue].queue == VK_NULL_HANDLE)
			return true;

		if (SupportsSubmissionTokens() && queues[point.queue].timeline_semaphore != VK_NULL_HANDLE)
		{
			uint64_t completed = 0;
			vulkan_check(vkGetSemaphoreCounterValue(device, queues[point.queue].timeline_semaphore, &completed));
			uint64_t known_completed = queue_timeline_completed_fallback[point.queue].load(std::memory_order_relaxed);
			while (known_completed < completed && !queue_timeline_completed_fallback[point.queue].compare_exchange_weak(known_completed, completed, std::memory_order_release, std::memory_order_relaxed))
			{
			}
			return completed >= point.value;
		}

		const uint64_t completed = queue_timeline_completed_fallback[point.queue].load(std::memory_order_acquire);
		if (completed >= point.value)
			return true;
		if (copyAllocator.is_point_complete(point))
			return true;

		for (uint32_t buffer = 0; buffer < BUFFERCOUNT; ++buffer)
		{
			const uint64_t mapped_value = queue_timeline_value_per_buffer[buffer][point.queue].load(std::memory_order_acquire);
			if (mapped_value != point.value)
				continue;
			VkFence fence = frame_fence[buffer][point.queue];
			if (fence == VK_NULL_HANDLE)
				return true;
			if (vkGetFenceStatus(device, fence) == VK_SUCCESS)
			{
				uint64_t known_completed = queue_timeline_completed_fallback[point.queue].load(std::memory_order_relaxed);
				while (known_completed < mapped_value && !queue_timeline_completed_fallback[point.queue].compare_exchange_weak(known_completed, mapped_value, std::memory_order_release, std::memory_order_relaxed))
				{
				}
				return true;
			}
			return false;
		}

		return queue_timeline_completed_fallback[point.queue].load(std::memory_order_acquire) >= point.value;
	}

	void GraphicsDevice_Vulkan::WaitQueuePoint(QueueSyncPoint point) const
	{
		if (!point.IsValid())
			return;
		if (point.queue >= QUEUE_COUNT)
			return;
		if (queues[point.queue].queue == VK_NULL_HANDLE)
			return;
		if (SupportsSubmissionTokens() && queues[point.queue].timeline_semaphore != VK_NULL_HANDLE)
		{
			VkSemaphore wait_semaphore = queues[point.queue].timeline_semaphore;
			VkSemaphoreWaitInfo wait_info = {};
			wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			wait_info.semaphoreCount = 1;
			wait_info.pSemaphores = &wait_semaphore;
			wait_info.pValues = &point.value;
			while (vulkan_check(vkWaitSemaphores(device, &wait_info, timeout_value)) == VK_TIMEOUT)
			{
				VULKAN_LOG_ERROR("[WaitQueuePoint] vkWaitSemaphores resulted in VK_TIMEOUT");
				std::this_thread::yield();
			}
			uint64_t known_completed = queue_timeline_completed_fallback[point.queue].load(std::memory_order_relaxed);
			while (known_completed < point.value && !queue_timeline_completed_fallback[point.queue].compare_exchange_weak(known_completed, point.value, std::memory_order_release, std::memory_order_relaxed))
			{
			}
			return;
		}

		if (queue_timeline_completed_fallback[point.queue].load(std::memory_order_acquire) >= point.value)
			return;
		if (copyAllocator.wait_point(point))
			return;

		for (uint32_t buffer = 0; buffer < BUFFERCOUNT; ++buffer)
		{
			const uint64_t mapped_value = queue_timeline_value_per_buffer[buffer][point.queue].load(std::memory_order_acquire);
			if (mapped_value != point.value)
				continue;
			VkFence fence = frame_fence[buffer][point.queue];
			if (fence == VK_NULL_HANDLE)
				return;
			while (vulkan_check(vkWaitForFences(device, 1, &fence, VK_TRUE, timeout_value)) == VK_TIMEOUT)
			{
				std::this_thread::yield();
			}
			uint64_t known_completed = queue_timeline_completed_fallback[point.queue].load(std::memory_order_relaxed);
			while (known_completed < mapped_value && !queue_timeline_completed_fallback[point.queue].compare_exchange_weak(known_completed, mapped_value, std::memory_order_release, std::memory_order_relaxed))
			{
			}
			return;
		}

		WaitForGPU();
		uint64_t known_completed = queue_timeline_completed_fallback[point.queue].load(std::memory_order_relaxed);
		while (known_completed < point.value && !queue_timeline_completed_fallback[point.queue].compare_exchange_weak(known_completed, point.value, std::memory_order_release, std::memory_order_relaxed))
		{
		}
	}

	QueueSyncPoint GraphicsDevice_Vulkan::GetLastSubmittedQueuePoint(QUEUE_TYPE queue) const
	{
		if (queue >= QUEUE_COUNT)
			return {};
		uint64_t value = 0;
		if (SupportsSubmissionTokens() && queues[queue].timeline_semaphore != VK_NULL_HANDLE)
		{
			value = queues[queue].timeline_value.load(std::memory_order_relaxed);
		}
		else
		{
			value = queue_timeline_submitted_fallback[queue].load(std::memory_order_relaxed);
		}
		if (value == 0)
			return {};
		return QueueSyncPoint{ queue, value };
	}

	QueueSyncPoint GraphicsDevice_Vulkan::GetLastCompletedQueuePoint(QUEUE_TYPE queue) const
	{
		if (queue >= QUEUE_COUNT)
			return {};
		uint64_t completed = 0;
		if (SupportsSubmissionTokens() && queues[queue].timeline_semaphore != VK_NULL_HANDLE)
		{
			if (vkGetSemaphoreCounterValue(device, queues[queue].timeline_semaphore, &completed) != VK_SUCCESS || completed == 0)
				return {};
			uint64_t known_completed = queue_timeline_completed_fallback[queue].load(std::memory_order_relaxed);
			while (known_completed < completed && !queue_timeline_completed_fallback[queue].compare_exchange_weak(known_completed, completed, std::memory_order_release, std::memory_order_relaxed))
			{
			}
		}
		else
		{
			completed = queue_timeline_completed_fallback[queue].load(std::memory_order_acquire);
		}
		if (completed == 0)
			return {};
		return QueueSyncPoint{ queue, completed };
	}

	UploadTicket GraphicsDevice_Vulkan::UploadAsyncInternal(const UploadDescInternal& upload) const
	{
		UploadTicket ticket = {};
		if (upload.type == UploadDescInternal::Type::BUFFER && (upload.src_data == nullptr || upload.src_size == 0))
			return ticket;

		const QUEUE_TYPE upload_queue = upload.queue < QUEUE_COUNT ? upload.queue : QUEUE_COPY;

		switch (upload.type)
		{
		case UploadDescInternal::Type::BUFFER:
		{
			if (upload.dst_buffer == nullptr || upload.src_size == 0)
				return ticket;
			auto* dst_internal = to_internal(upload.dst_buffer);
			if (dst_internal == nullptr || dst_internal->resource == VK_NULL_HANDLE)
				return ticket;
			if (upload.dst_buffer->mapped_data != nullptr)
			{
				std::memcpy((uint8_t*)upload.dst_buffer->mapped_data + upload.dst_offset, upload.src_data, (size_t)upload.src_size);
				return ticket;
			}

			CopyAllocator::CopyCMD cmd = copyAllocator.allocate(upload.src_size, upload_queue);
			if (!cmd.IsValid())
				return ticket;

			std::memcpy(cmd.uploadbuffer.mapped_data, upload.src_data, (size_t)upload.src_size);
			VkBufferCopy copyRegion = {};
			copyRegion.size = upload.src_size;
			copyRegion.srcOffset = 0;
			copyRegion.dstOffset = upload.dst_offset;
			vkCmdCopyBuffer(
				cmd.transferCommandBuffer,
				to_internal(&cmd.uploadbuffer)->resource,
				dst_internal->resource,
				1,
				&copyRegion
			);

				ticket.token = copyAllocator.submit(std::move(cmd), upload_queue);
				ticket.completion = ticket.token.Get(upload_queue);
				if (!ticket.completion.IsValid())
				{
					ticket.completion = GetLastSubmittedQueuePoint(upload_queue);
				}
				return ticket;
			}
		case UploadDescInternal::Type::TEXTURE:
		{
			if (upload.dst_texture == nullptr || upload.subresources == nullptr)
				return ticket;
			auto* dst_internal = to_internal(upload.dst_texture);
			if (dst_internal == nullptr || dst_internal->resource == VK_NULL_HANDLE)
				return ticket;

			if (upload.dst_texture->mapped_data != nullptr && upload.dst_texture->mapped_subresources != nullptr)
			{
				const uint32_t subresource_count = upload.subresource_count == 0 ? GetTextureSubresourceCount(upload.dst_texture->desc) : upload.subresource_count;
				for (uint32_t i = 0; i < subresource_count; ++i)
				{
					const SubresourceData& src = upload.subresources[i];
					const SubresourceData& dst = upload.dst_texture->mapped_subresources[i];
					std::memcpy(const_cast<void*>(dst.data_ptr), src.data_ptr, (size_t)std::min(src.slice_pitch, dst.slice_pitch));
				}
				return ticket;
			}

			const uint32_t subresource_count = upload.subresource_count == 0 ? GetTextureSubresourceCount(upload.dst_texture->desc) : upload.subresource_count;
			VkDeviceSize required_staging_size = 0;
			{
				uint32_t probe_idx = 0;
				for (uint32_t layer = 0; layer < upload.dst_texture->desc.array_size && probe_idx < subresource_count; ++layer)
				{
					uint32_t width = upload.dst_texture->desc.width;
					uint32_t height = upload.dst_texture->desc.height;
					uint32_t depth = upload.dst_texture->desc.depth;
					for (uint32_t mip = 0; mip < upload.dst_texture->desc.mip_levels && probe_idx < subresource_count; ++mip)
					{
						const SubresourceData& src_data = upload.subresources[probe_idx++];
						const uint32_t dst_rowpitch = (uint32_t)align(src_data.row_pitch, (uint32_t)properties2.properties.limits.optimalBufferCopyRowPitchAlignment);
						const uint32_t dst_slicepitch = dst_rowpitch * (uint32_t)std::max(1u, height / GetFormatBlockSize(upload.dst_texture->desc.format));
						required_staging_size += VkDeviceSize(dst_slicepitch) * depth;
						required_staging_size = align(required_staging_size, VkDeviceSize(4));
						width = std::max(1u, width / 2);
						height = std::max(1u, height / 2);
						depth = std::max(1u, depth / 2);
					}
				}
			}
			if (required_staging_size == 0)
			{
				required_staging_size = (VkDeviceSize)ComputeTextureMemorySizeInBytes(upload.dst_texture->desc);
			}
			CopyAllocator::CopyCMD cmd = copyAllocator.allocate((uint64_t)required_staging_size, upload_queue);
			if (!cmd.IsValid())
				return ticket;

			VkBufferImageCopy* copyRegions = nullptr;
			VkDeviceSize copyOffset = 0;
			uint32_t initDataIdx = 0;
			for (uint32_t layer = 0; layer < upload.dst_texture->desc.array_size && initDataIdx < subresource_count; ++layer)
			{
				uint32_t width = upload.dst_texture->desc.width;
				uint32_t height = upload.dst_texture->desc.height;
				uint32_t depth = upload.dst_texture->desc.depth;
				for (uint32_t mip = 0; mip < upload.dst_texture->desc.mip_levels && initDataIdx < subresource_count; ++mip)
				{
					const SubresourceData& src_data = upload.subresources[initDataIdx++];
					const uint32_t dst_rowpitch = (uint32_t)align(src_data.row_pitch, (uint32_t)properties2.properties.limits.optimalBufferCopyRowPitchAlignment);
					const uint32_t dst_slicepitch = dst_rowpitch * (uint32_t)std::max(1u, height / GetFormatBlockSize(upload.dst_texture->desc.format));
					for (uint32_t z = 0; z < depth; ++z)
					{
						std::memcpy(
							(uint8_t*)cmd.uploadbuffer.mapped_data + copyOffset + dst_slicepitch * z,
							(uint8_t*)src_data.data_ptr + src_data.slice_pitch * z,
							src_data.slice_pitch
						);
					}

					VkBufferImageCopy copyRegion = {};
					copyRegion.bufferOffset = copyOffset;
					copyRegion.bufferRowLength = 0;
					copyRegion.bufferImageHeight = 0;
					copyRegion.imageSubresource.aspectMask = IsFormatDepthSupport(upload.dst_texture->desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
					copyRegion.imageSubresource.mipLevel = mip;
					copyRegion.imageSubresource.baseArrayLayer = layer;
					copyRegion.imageSubresource.layerCount = 1;
					copyRegion.imageOffset = { 0, 0, 0 };
					copyRegion.imageExtent = { width, height, depth };
					arrput(copyRegions, copyRegion);

					copyOffset += dst_slicepitch * depth;
					copyOffset = align(copyOffset, VkDeviceSize(4));
					width = std::max(1u, width / 2);
					height = std::max(1u, height / 2);
					depth = std::max(1u, depth / 2);
				}
			}

			VkImageMemoryBarrier2 to_copy = {};
			to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			to_copy.image = dst_internal->resource;
			to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			to_copy.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			to_copy.srcAccessMask = 0;
			to_copy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			to_copy.subresourceRange.aspectMask = IsFormatDepthSupport(upload.dst_texture->desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			if (IsFormatStencilSupport(upload.dst_texture->desc.format))
			{
				to_copy.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			}
			to_copy.subresourceRange.baseArrayLayer = 0;
			to_copy.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
			to_copy.subresourceRange.baseMipLevel = 0;
			to_copy.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
			to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

			VkDependencyInfo to_copy_dependency = {};
			to_copy_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			to_copy_dependency.imageMemoryBarrierCount = 1;
			to_copy_dependency.pImageMemoryBarriers = &to_copy;
			vkCmdPipelineBarrier2(cmd.transferCommandBuffer, &to_copy_dependency);

			vkCmdCopyBufferToImage(
				cmd.transferCommandBuffer,
				to_internal(&cmd.uploadbuffer)->resource,
				dst_internal->resource,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				(uint32_t)arrlenu(copyRegions),
				copyRegions
			);

			VkImageMemoryBarrier2 to_final = to_copy;
			std::swap(to_final.srcStageMask, to_final.dstStageMask);
			to_final.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			to_final.newLayout = _ConvertImageLayout(upload.texture_final_layout);
			to_final.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			to_final.dstAccessMask = _ParseResourceState(upload.texture_final_layout);
			VkDependencyInfo to_final_dependency = {};
			to_final_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			to_final_dependency.imageMemoryBarrierCount = 1;
			to_final_dependency.pImageMemoryBarriers = &to_final;
			vkCmdPipelineBarrier2(cmd.transferCommandBuffer, &to_final_dependency);

				ticket.token = copyAllocator.submit(std::move(cmd), upload_queue);
				ticket.completion = ticket.token.Get(upload_queue);
				if (!ticket.completion.IsValid())
				{
					ticket.completion = GetLastSubmittedQueuePoint(upload_queue);
				}
				arrfree(copyRegions);
				return ticket;
			}
		default:
			break;
		}

		return ticket;
	}

	UploadTicket GraphicsDevice_Vulkan::EnqueueBufferUpload(const BufferUploadDesc& upload) 
	{
		UploadDescInternal internal = {};
		internal.type = UploadDescInternal::Type::BUFFER;
		internal.queue = QUEUE_COPY;
		internal.src_data = upload.data;
		internal.src_size = upload.size;
		internal.dst_buffer = upload.dst;
		internal.dst_offset = upload.dst_offset;
		UploadTicket ticket = UploadAsyncInternal(internal);
		if (upload.block_until_complete && ticket.IsValid())
		{
			WaitUpload(ticket);
		}
		return ticket;
	}

	UploadTicket GraphicsDevice_Vulkan::EnqueueTextureUpload(const TextureUploadDesc& upload)
	{
		UploadDescInternal internal = {};
		internal.type = UploadDescInternal::Type::TEXTURE;
		internal.queue = QUEUE_COPY;
		internal.src_data = upload.subresources != nullptr && upload.subresource_count > 0 ? upload.subresources[0].data_ptr : nullptr;
		internal.src_size = upload.subresources != nullptr && upload.subresource_count > 0 ? upload.subresources[0].slice_pitch : 0;
		internal.dst_texture = upload.dst;
		internal.subresources = upload.subresources;
		internal.subresource_count = upload.subresource_count;
		internal.texture_final_layout = ResourceState::SHADER_RESOURCE;
		UploadTicket ticket = UploadAsyncInternal(internal);
		if (upload.block_until_complete && ticket.IsValid())
		{
			WaitUpload(ticket);
		}
		return ticket;
	}

	bool GraphicsDevice_Vulkan::IsUploadComplete(const UploadTicket& ticket) const
	{
		copyAllocator.recycle_completed();
		if (!ticket.completion.IsValid())
		{
			return true;
		}
		const bool complete = IsQueuePointComplete(ticket.completion);
		return complete;
	}

	void GraphicsDevice_Vulkan::WaitUpload(const UploadTicket& ticket) const
	{
		if (!ticket.completion.IsValid())
			return;
		WaitQueuePoint(ticket.completion);
		copyAllocator.recycle_completed();
	}

	void GraphicsDevice_Vulkan::WaitForGPU() const
	{
		vulkan_check(vkDeviceWaitIdle(device));
		for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
		{
			uint64_t submitted = queue_timeline_submitted_fallback[q].load(std::memory_order_relaxed);
			uint64_t completed = queue_timeline_completed_fallback[q].load(std::memory_order_relaxed);
			while (completed < submitted && !queue_timeline_completed_fallback[q].compare_exchange_weak(completed, submitted, std::memory_order_release, std::memory_order_relaxed))
			{
			}
		}
		copyAllocator.recycle_completed();
	}
	void GraphicsDevice_Vulkan::ClearPipelineStateCache()
	{
		layout_locker.lock();
		pso_layouts.clear();
		layout_locker.unlock();

		pipelines_global.clear();

		for (auto& x : commandlists)
		{
			x->pipelines_worker.clear();
		}

		if (pipelineCache != VK_NULL_HANDLE)
		{
			vkDestroyPipelineCache(device, pipelineCache, nullptr);
			pipelineCache = VK_NULL_HANDLE;

			VkPipelineCacheCreateInfo createInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
			createInfo.initialDataSize = 0;
			createInfo.pInitialData = nullptr;
			vulkan_check(vkCreatePipelineCache(device, &createInfo, nullptr, &pipelineCache));
		}
	}

	Texture GraphicsDevice_Vulkan::GetBackBuffer(const SwapChain* swapchain) const
	{
		auto swapchain_internal = to_internal(swapchain);

		auto internal_state = swapchain_internal->textures[swapchain_internal->swapChainImageIndex];

		Texture result;
		result.type = GPUResource::Type::TEXTURE;
		result.internal_state = internal_state;
		result.desc.type = TextureDesc::Type::TEXTURE_2D;
		result.desc.width = swapchain_internal->swapChainExtent.width;
		result.desc.height = swapchain_internal->swapChainExtent.height;
		result.desc.format = swapchain->desc.format;
		result.desc.layout = ResourceState::SWAPCHAIN;
		result.desc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::RENDER_TARGET;
		return result;
	}
	ColorSpace GraphicsDevice_Vulkan::GetSwapChainColorSpace(const SwapChain* swapchain) const
	{
		auto internal_state = to_internal(swapchain);
		return internal_state->colorSpace;
	}
	bool GraphicsDevice_Vulkan::IsSwapChainSupportsHDR(const SwapChain* swapchain) const
	{
		auto internal_state = to_internal(swapchain);

		uint32_t formatCount;
		VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, internal_state->surface, &formatCount, nullptr);
		if (res == VK_SUCCESS)
		{
			VkSurfaceFormatKHR* swapchain_formats = nullptr;
			arrsetlen(swapchain_formats, formatCount);
			res = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, internal_state->surface, &formatCount, swapchain_formats);
			if (res == VK_SUCCESS)
			{
				for (uint32_t i = 0; i < formatCount; ++i)
				{
					const auto& format = swapchain_formats[i];
					if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	void GraphicsDevice_Vulkan::SparseUpdate(QUEUE_TYPE queue, const SparseUpdateCommand* commands, uint32_t command_count)
	{
		thread_local VkBindSparseInfo* sparse_infos = nullptr;
		struct DataPerBind
		{
			VkSparseBufferMemoryBindInfo buffer_bind_info;
			VkSparseImageOpaqueMemoryBindInfo image_opaque_bind_info;
			VkSparseImageMemoryBindInfo image_bind_info;
			VkSparseMemoryBind* memory_binds = nullptr;
			VkSparseImageMemoryBind* image_memory_binds = nullptr;
		};
		thread_local DataPerBind* sparse_binds = nullptr;

		arrsetlen(sparse_infos, command_count);
		arrsetlen(sparse_binds, command_count);

		for (uint32_t i = 0; i < command_count; ++i)
		{
			const SparseUpdateCommand& in_command = commands[i];
			VkBindSparseInfo& out_info = sparse_infos[i];
			out_info = {};
			out_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;

			DataPerBind& out_bind = sparse_binds[i];

			VkDeviceMemory tile_pool_memory = VK_NULL_HANDLE;
			VkDeviceSize tile_pool_offset = 0;
			if (in_command.tile_pool != nullptr)
			{
				auto internal_tile_pool = to_internal(in_command.tile_pool);
				tile_pool_memory = internal_tile_pool->allocation->GetMemory();
				tile_pool_offset = internal_tile_pool->allocation->GetOffset();
			}

			arrsetlen(out_bind.memory_binds, 0);
			arrsetlen(out_bind.image_memory_binds, 0);

			if (wiGraphicsGPUResourceIsBuffer(in_command.sparse_resource))
			{
				auto internal_sparse = to_internal<GPUBuffer>(in_command.sparse_resource);

				VkSparseBufferMemoryBindInfo& info = out_bind.buffer_bind_info;
				info = {};
				info.buffer = internal_sparse->resource;
				info.pBinds = out_bind.memory_binds;
				info.bindCount = 0;

				for (uint32_t j = 0; j < in_command.num_resource_regions; ++j)
				{
					const SparseResourceCoordinate& in_coordinate = in_command.coordinates[j];
					const SparseRegionSize& in_size = in_command.sizes[j];

					const TileRangeFlags& in_flags = in_command.range_flags[j];
					uint32_t in_offset = in_command.range_start_offsets[j];
					uint32_t in_tile_count = in_command.range_tile_counts[j];
					arrput(out_bind.memory_binds, {});
					VkSparseMemoryBind& out_memory_bind = out_bind.memory_binds[arrlenu(out_bind.memory_binds) - 1];
					out_memory_bind = {};
					out_memory_bind.resourceOffset = in_coordinate.x * in_command.sparse_resource->sparse_page_size;
					out_memory_bind.size = in_tile_count * in_command.sparse_resource->sparse_page_size;
					if (in_flags == TileRangeFlags::Null)
					{
						out_memory_bind.memory = VK_NULL_HANDLE;
					}
					else
					{
						out_memory_bind.memory = tile_pool_memory;
						out_memory_bind.memoryOffset = tile_pool_offset + in_offset * in_command.sparse_resource->sparse_page_size;
					}
					info.bindCount++;
				}
				info.pBinds = out_bind.memory_binds;

				if (info.bindCount > 0)
				{
					out_info.pBufferBinds = &out_bind.buffer_bind_info;
					out_info.bufferBindCount = 1;
				}
			}
			else if (wiGraphicsGPUResourceIsTexture(in_command.sparse_resource))
			{
				const Texture* sparse_texture = (const Texture*)in_command.sparse_resource;
				const TextureDesc& texture_desc = *wiGraphicsTextureGetDesc(sparse_texture);
				auto internal_sparse = to_internal(sparse_texture);

				VkImageAspectFlags aspectMask = {};
				if (IsFormatDepthSupport(texture_desc.format))
				{
					aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
					if (IsFormatStencilSupport(texture_desc.format))
					{
						aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
				}
				if (has_flag(texture_desc.bind_flags, BindFlag::RENDER_TARGET) ||
					has_flag(texture_desc.bind_flags, BindFlag::BIND_SHADER_RESOURCE) ||
					has_flag(texture_desc.bind_flags, BindFlag::BIND_UNORDERED_ACCESS))
				{
					aspectMask |= VK_IMAGE_ASPECT_COLOR_BIT;
				}

				VkSparseImageOpaqueMemoryBindInfo& opaque_info = out_bind.image_opaque_bind_info;
				opaque_info = {};
				opaque_info.image = internal_sparse->resource;
				opaque_info.pBinds = out_bind.memory_binds;
				opaque_info.bindCount = 0;

				VkSparseImageMemoryBindInfo& info = out_bind.image_bind_info;
				info = {};
				info.image = internal_sparse->resource;
				info.pBinds = out_bind.image_memory_binds;
				info.bindCount = 0;

				for (uint32_t j = 0; j < in_command.num_resource_regions; ++j)
				{
					const SparseResourceCoordinate& in_coordinate = in_command.coordinates[j];
					const SparseRegionSize& in_size = in_command.sizes[j];
					const bool is_miptail = in_coordinate.mip >= internal_sparse->sparse_texture_properties.packed_mip_start;

					if (is_miptail)
					{
						opaque_info.bindCount++;

						const TileRangeFlags& in_flags = in_command.range_flags[j];
						uint32_t in_offset = in_command.range_start_offsets[j];
						uint32_t in_tile_count = in_command.range_tile_counts[j];
							arrput(out_bind.memory_binds, {});
							VkSparseMemoryBind& out_memory_bind = out_bind.memory_binds[arrlenu(out_bind.memory_binds) - 1];
							out_memory_bind = {};
						out_memory_bind.resourceOffset = internal_sparse->sparse_texture_properties.packed_mip_tile_offset * sparse_texture->sparse_page_size;
						out_memory_bind.size = in_tile_count * in_command.sparse_resource->sparse_page_size;
						if (in_flags == TileRangeFlags::Null)
						{
							out_memory_bind.memory = VK_NULL_HANDLE;
						}
						else
						{
							out_memory_bind.memory = tile_pool_memory;
							out_memory_bind.memoryOffset = tile_pool_offset + in_offset * in_command.sparse_resource->sparse_page_size;
						}
					}
					else
					{
						info.bindCount++;

						const TileRangeFlags& in_flags = in_command.range_flags[j];
						uint32_t in_offset = in_command.range_start_offsets[j];
						uint32_t in_tile_count = in_command.range_tile_counts[j];
							arrput(out_bind.image_memory_binds, {});
							VkSparseImageMemoryBind& out_image_memory_bind = out_bind.image_memory_binds[arrlenu(out_bind.image_memory_binds) - 1];
							out_image_memory_bind = {};
						if (in_flags == TileRangeFlags::Null)
						{
							out_image_memory_bind.memory = VK_NULL_HANDLE;
						}
						else
						{
							out_image_memory_bind.memory = tile_pool_memory;
							out_image_memory_bind.memoryOffset = tile_pool_offset + in_offset * in_command.sparse_resource->sparse_page_size;
						}
						out_image_memory_bind.subresource.mipLevel = in_coordinate.mip;
						out_image_memory_bind.subresource.arrayLayer = in_coordinate.slice;
						out_image_memory_bind.subresource.aspectMask = aspectMask;
						out_image_memory_bind.offset.x = in_coordinate.x * internal_sparse->sparse_texture_properties.tile_width;
						out_image_memory_bind.offset.y = in_coordinate.y * internal_sparse->sparse_texture_properties.tile_height;
						out_image_memory_bind.offset.z = in_coordinate.z * internal_sparse->sparse_texture_properties.tile_depth;
						out_image_memory_bind.extent.width = std::min(texture_desc.width, in_size.width * internal_sparse->sparse_texture_properties.tile_width);
						out_image_memory_bind.extent.height = std::min(texture_desc.height, in_size.height * internal_sparse->sparse_texture_properties.tile_height);
						out_image_memory_bind.extent.depth = std::min(texture_desc.depth, in_size.depth * internal_sparse->sparse_texture_properties.tile_depth);
					}

				}
				opaque_info.pBinds = out_bind.memory_binds;
				info.pBinds = out_bind.image_memory_binds;

				if (opaque_info.bindCount > 0)
				{
					out_info.pImageOpaqueBinds = &out_bind.image_opaque_bind_info;
					out_info.imageOpaqueBindCount = 1;
				}
				if (info.bindCount > 0)
				{
					out_info.pImageBinds = &out_bind.image_bind_info;
					out_info.imageBindCount = 1;
				}

			}

		}

		// Queue command:
		{
			CommandQueue* q = &queues[queue];
			if (!q->sparse_binding_supported)
			{
				// 1.) fall back to any sparse supporting queue
				q = &queue_sparse;
			}
			std::scoped_lock lock(*q->locker);
			VULKAN_ASSERT_MSG(q->sparse_binding_supported, "Vulkan sparse mapping was used while the feature is not available! This can result in broken rendering or crash. Try to update the graphics driver if this happens.");

				vulkan_check(vkQueueBindSparse(q->queue, (uint32_t)arrlenu(sparse_infos), sparse_infos, VK_NULL_HANDLE));
			}
		}

	void GraphicsDevice_Vulkan::WaitCommandList(CommandList cmd, CommandList wait_for)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		CommandList_Vulkan& commandlist_wait_for = GetCommandList(wait_for);
		SDL_assert(commandlist_wait_for.id < commandlist.id); // can't wait for future command list!
		VkSemaphore semaphore = new_semaphore();
		commandlist.waits.push_back(semaphore);
		commandlist_wait_for.signals.push_back(semaphore);
	}
	void GraphicsDevice_Vulkan::RenderPassBegin(const SwapChain* swapchain, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		arrsetlen(commandlist.renderpass_barriers_begin, 0);
		arrsetlen(commandlist.renderpass_barriers_end, 0);
		auto internal_state = to_internal(swapchain);

		VkResult res = VK_SUCCESS;
		bool acquired_here = false;
		internal_state->locker.lock();
		if (internal_state->explicit_acquire_pending)
		{
			internal_state->explicit_acquire_pending = false;
		}
		else
		{
			acquired_here = true;
			internal_state->swapChainAcquireSemaphoreIndex = (internal_state->swapChainAcquireSemaphoreIndex + 1) % internal_state->swapchainAcquireSemaphores.size();
			do {
				res = vkAcquireNextImageKHR(
					device,
					internal_state->swapChain,
					timeout_value,
					internal_state->swapchainAcquireSemaphores[internal_state->swapChainAcquireSemaphoreIndex],
					VK_NULL_HANDLE,
					&internal_state->swapChainImageIndex
				);
				if (res == VK_TIMEOUT)
				{
					VULKAN_LOG_ERROR("vkAcquireNextImageKHR resulted in VK_TIMEOUT, retrying");
					std::this_thread::yield();
				}
			} while (res == VK_TIMEOUT);
		}
		internal_state->locker.unlock();

		if (acquired_here && res != VK_SUCCESS)
		{
			// Handle outdated error in acquire:
			if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
			{
				// we need to create a new semaphore or jump through a few hoops to
				// wait for the current one to be unsignalled before we can use it again
				// creating a new one is easiest. See also:
				// https://github.com/KhronosGroup/Vulkan-Docs/issues/152
				// https://www.khronos.org/blog/resolving-longstanding-issues-with-wsi
				{
					std::scoped_lock lock(allocationhandler->destroylocker);
					for (auto& x : internal_state->swapchainAcquireSemaphores)
					{
						allocationhandler->Retire(allocationhandler->destroyer_semaphores, x);
					}
				}
				internal_state->swapchainAcquireSemaphores.clear();
				if (CreateSwapChainInternal(internal_state, physicalDevice, device, allocationhandler))
				{
					RenderPassBegin(swapchain, cmd);
					return;
				}
			}
			SDL_assert(0);
		}
		commandlist.prev_swapchains.push_back(*swapchain);

		VkRenderingInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		info.renderArea.offset.x = 0;
		info.renderArea.offset.y = 0;
		info.renderArea.extent.width = std::min(swapchain->desc.width, internal_state->swapChainExtent.width);
		info.renderArea.extent.height = std::min(swapchain->desc.height, internal_state->swapChainExtent.height);
		info.layerCount = 1;

		VkRenderingAttachmentInfo color_attachment = {};
		color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		color_attachment.imageView = internal_state->textures[internal_state->swapChainImageIndex]->rtv.image_view;
		color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color_attachment.clearValue.color.float32[0] = swapchain->desc.clear_color[0];
		color_attachment.clearValue.color.float32[1] = swapchain->desc.clear_color[1];
		color_attachment.clearValue.color.float32[2] = swapchain->desc.clear_color[2];
		color_attachment.clearValue.color.float32[3] = swapchain->desc.clear_color[3];

		info.colorAttachmentCount = 1;
		info.pColorAttachments = &color_attachment;

		VkImageMemoryBarrier2 barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.image = internal_state->textures[internal_state->swapChainImageIndex]->resource;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_NONE;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

		VkDependencyInfo dependencyInfo = {};
		dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dependencyInfo.imageMemoryBarrierCount = 1;
		dependencyInfo.pImageMemoryBarriers = &barrier;
		vkCmdPipelineBarrier2(commandlist.GetCommandBuffer(), &dependencyInfo);

		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_NONE;
		arrput(commandlist.renderpass_barriers_end, barrier);

		vkCmdBeginRendering(commandlist.GetCommandBuffer(), &info);

		commandlist.renderpass_info = wiGraphicsCreateRenderPassInfoFromSwapChainDesc(&swapchain->desc);
	}
	void GraphicsDevice_Vulkan::RenderPassBegin(const RenderPassImage* images, uint32_t image_count, CommandList cmd, RenderPassFlags flags)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		arrsetlen(commandlist.renderpass_barriers_begin, 0);
		arrsetlen(commandlist.renderpass_barriers_end, 0);

		VkRenderingInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		if (has_flag(flags, RenderPassFlags::SUSPENDING))
		{
			info.flags |= VK_RENDERING_SUSPENDING_BIT;
		}
		if (has_flag(flags, RenderPassFlags::RESUMING))
		{
			info.flags |= VK_RENDERING_RESUMING_BIT;
		}
		info.layerCount = 1;
		info.renderArea.offset.x = 0;
		info.renderArea.offset.y = 0;
		info.renderArea.extent.width = properties2.properties.limits.maxFramebufferWidth;
		info.renderArea.extent.height = properties2.properties.limits.maxFramebufferHeight;
		VkRenderingAttachmentInfo color_attachments[8] = {};
		VkRenderingAttachmentInfo depth_attachment = {};
		VkRenderingAttachmentInfo stencil_attachment = {};
		VkRenderingFragmentShadingRateAttachmentInfoKHR shading_rate_attachment = {};
		bool color = false;
		bool depth = false;
		bool stencil = false;
		uint32_t color_resolve_count = 0;
		for (uint32_t i = 0; i < image_count; ++i)
		{
			const RenderPassImage& image = images[i];
			const Texture* texture = image.texture;
			const TextureDesc& desc = *wiGraphicsTextureGetDesc(texture);
			int subresource = image.subresource;
			auto internal_state = to_internal(texture);

			if (image.type == RenderPassImage::Type::RENDERTARGET || image.type == RenderPassImage::Type::DEPTH_STENCIL)
			{
				info.renderArea.extent.width = std::min(info.renderArea.extent.width, desc.width);
				info.renderArea.extent.height = std::min(info.renderArea.extent.height, desc.height);
			}

			VkAttachmentLoadOp loadOp;
			switch (image.loadop)
			{
			default:
			case RenderPassImage::LoadOp::LOAD:
				loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				break;
			case RenderPassImage::LoadOp::CLEAR:
				loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				break;
			case RenderPassImage::LoadOp::LOADOP_DONTCARE:
				loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
				break;
			}

			VkAttachmentStoreOp storeOp;
			switch (image.storeop)
			{
			default:
			case RenderPassImage::StoreOp::STORE:
				storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				break;
			case RenderPassImage::StoreOp::STOREOP_DONTCARE:
				storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				break;
			}

			Texture_Vulkan::TextureSubresource descriptor;

			switch (image.type)
			{
			case RenderPassImage::Type::RENDERTARGET:
			{
				descriptor = subresource < 0 ? internal_state->rtv : internal_state->subresources_rtv[subresource];
				VkRenderingAttachmentInfo& color_attachment = color_attachments[info.colorAttachmentCount++];
				color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				color_attachment.imageView = descriptor.image_view;
				color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				color_attachment.loadOp = loadOp;
				color_attachment.storeOp = storeOp;
				color_attachment.clearValue.color.float32[0] = desc.clear.color[0];
				color_attachment.clearValue.color.float32[1] = desc.clear.color[1];
				color_attachment.clearValue.color.float32[2] = desc.clear.color[2];
				color_attachment.clearValue.color.float32[3] = desc.clear.color[3];
				color = true;
			}
			break;

			case RenderPassImage::Type::RESOLVE:
			{
				descriptor = subresource < 0 ? internal_state->rtv : internal_state->subresources_rtv[subresource];
				VkRenderingAttachmentInfo& color_attachment = color_attachments[color_resolve_count++];
				color_attachment.resolveImageView = descriptor.image_view;
				color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
			}
			break;

			case RenderPassImage::Type::DEPTH_STENCIL:
			{
				descriptor = subresource < 0 ? internal_state->dsv : internal_state->subresources_dsv[subresource];
				depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				depth_attachment.imageView = descriptor.image_view;
				if (image.layout == ResourceState::DEPTHSTENCIL_READONLY)
				{
					depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
				}
				else
				{
					depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
				}
				depth_attachment.loadOp = loadOp;
				depth_attachment.storeOp = storeOp;
				depth_attachment.clearValue.depthStencil.depth = desc.clear.depth_stencil.depth;
				depth = true;
				if (IsFormatStencilSupport(desc.format))
				{
					stencil_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
					stencil_attachment.imageView = subresource < 0 ? internal_state->dsv.image_view : internal_state->subresources_dsv[subresource].image_view;
					if (image.layout == ResourceState::DEPTHSTENCIL_READONLY)
					{
						stencil_attachment.imageLayout = VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
					}
					else
					{
						stencil_attachment.imageLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
					}
					stencil_attachment.loadOp = loadOp;
					stencil_attachment.storeOp = storeOp;
					stencil_attachment.clearValue.depthStencil.stencil = desc.clear.depth_stencil.stencil;
					stencil = true;
				}
			}
			break;

			case RenderPassImage::Type::RESOLVE_DEPTH:
			{
				descriptor = subresource < 0 ? internal_state->dsv : internal_state->subresources_dsv[subresource];
				depth_attachment.resolveImageView = descriptor.image_view;
				stencil_attachment.resolveImageView = descriptor.image_view;
				depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
				stencil_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
				switch (image.depth_resolve_mode)
				{
				default:
				case RenderPassImage::DepthResolveMode::Min:
					depth_attachment.resolveMode = VK_RESOLVE_MODE_MIN_BIT;
					stencil_attachment.resolveMode = VK_RESOLVE_MODE_MIN_BIT;
					break;
				case RenderPassImage::DepthResolveMode::Max:
					depth_attachment.resolveMode = VK_RESOLVE_MODE_MAX_BIT;
					stencil_attachment.resolveMode = VK_RESOLVE_MODE_MAX_BIT;
					break;
				}
			}
			break;

			case RenderPassImage::Type::SHADING_RATE_SOURCE:
				descriptor = subresource < 0 ? internal_state->uav : internal_state->subresources_uav[subresource];
				shading_rate_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
				shading_rate_attachment.imageView = descriptor.image_view;
				shading_rate_attachment.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
				shading_rate_attachment.shadingRateAttachmentTexelSize.width = VARIABLE_RATE_SHADING_TILE_SIZE;
				shading_rate_attachment.shadingRateAttachmentTexelSize.height = VARIABLE_RATE_SHADING_TILE_SIZE;
				info.pNext = &shading_rate_attachment;
				break;
			default:
				break;
			}

			if (image.layout_before != image.layout)
			{
				arrput(commandlist.renderpass_barriers_begin, {});
				VkImageMemoryBarrier2& barrier = commandlist.renderpass_barriers_begin[arrlenu(commandlist.renderpass_barriers_begin) - 1];
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrier.image = internal_state->resource;
				barrier.oldLayout = _ConvertImageLayout(image.layout_before);
				barrier.newLayout = _ConvertImageLayout(image.layout);

				SDL_assert(barrier.newLayout != VK_IMAGE_LAYOUT_UNDEFINED);

				barrier.srcStageMask = _ConvertPipelineStage(image.layout_before);
				barrier.dstStageMask = _ConvertPipelineStage(image.layout);
				barrier.srcAccessMask = _ParseResourceState(image.layout_before);
				barrier.dstAccessMask = _ParseResourceState(image.layout);

				if (IsFormatDepthSupport(desc.format))
				{
					barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (IsFormatStencilSupport(desc.format))
					{
						barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
				}
				else
				{
					barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				}
				barrier.subresourceRange.baseMipLevel = descriptor.firstMip;
				barrier.subresourceRange.levelCount = descriptor.mipCount;
				barrier.subresourceRange.baseArrayLayer = descriptor.firstSlice;
				barrier.subresourceRange.layerCount = descriptor.sliceCount;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			}

			if (image.layout != image.layout_after)
			{
				arrput(commandlist.renderpass_barriers_end, {});
				VkImageMemoryBarrier2& barrier = commandlist.renderpass_barriers_end[arrlenu(commandlist.renderpass_barriers_end) - 1];
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrier.image = internal_state->resource;
				barrier.oldLayout = _ConvertImageLayout(image.layout);
				barrier.newLayout = _ConvertImageLayout(image.layout_after);

				SDL_assert(barrier.newLayout != VK_IMAGE_LAYOUT_UNDEFINED);

				barrier.srcStageMask = _ConvertPipelineStage(image.layout);
				barrier.dstStageMask = _ConvertPipelineStage(image.layout_after);
				barrier.srcAccessMask = _ParseResourceState(image.layout);
				barrier.dstAccessMask = _ParseResourceState(image.layout_after);

				if (IsFormatDepthSupport(desc.format))
				{
					barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (IsFormatStencilSupport(desc.format))
					{
						barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
				}
				else
				{
					barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				}
				barrier.subresourceRange.baseMipLevel = descriptor.firstMip;
				barrier.subresourceRange.levelCount = descriptor.mipCount;
				barrier.subresourceRange.baseArrayLayer = descriptor.firstSlice;
				barrier.subresourceRange.layerCount = descriptor.sliceCount;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			}

			info.layerCount = std::min(desc.array_size, std::max(info.layerCount, descriptor.sliceCount));
		}
		info.pColorAttachments = color ? color_attachments : nullptr;
		info.pDepthAttachment = depth ? &depth_attachment : nullptr;
		info.pStencilAttachment = stencil ? &stencil_attachment : nullptr;

		if (arrlenu(commandlist.renderpass_barriers_begin) > 0)
		{
			VkDependencyInfo dependencyInfo = {};
			dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(arrlenu(commandlist.renderpass_barriers_begin));
			dependencyInfo.pImageMemoryBarriers = commandlist.renderpass_barriers_begin;

			vkCmdPipelineBarrier2(commandlist.GetCommandBuffer(), &dependencyInfo);
		}

		vkCmdBeginRendering(commandlist.GetCommandBuffer(), &info);

		commandlist.renderpass_info = wiGraphicsCreateRenderPassInfoFromImages(images, image_count);
	}
	void GraphicsDevice_Vulkan::RenderPassEnd(CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdEndRendering(commandlist.GetCommandBuffer());

		if (arrlenu(commandlist.renderpass_barriers_end) > 0)
		{
			VkDependencyInfo dependencyInfo = {};
			dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(arrlenu(commandlist.renderpass_barriers_end));
			dependencyInfo.pImageMemoryBarriers = commandlist.renderpass_barriers_end;

			vkCmdPipelineBarrier2(commandlist.GetCommandBuffer(), &dependencyInfo);
			arrsetlen(commandlist.renderpass_barriers_end, 0);
		}

		commandlist.renderpass_info = {};
	}
	void GraphicsDevice_Vulkan::BindScissorRects(uint32_t numRects, const Rect* rects, CommandList cmd)
	{
		SDL_assert(rects != nullptr);
		VkRect2D scissors[16];
		SDL_assert(numRects <= SDL_arraysize(scissors));
		SDL_assert(numRects <= properties2.properties.limits.maxViewports);
		for(uint32_t i = 0; i < numRects; ++i)
		{
			scissors[i].extent.width = abs(rects[i].right - rects[i].left);
			scissors[i].extent.height = abs(rects[i].top - rects[i].bottom);
			scissors[i].offset.x = std::max(0, rects[i].left);
			scissors[i].offset.y = std::max(0, rects[i].top);
		}
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdSetScissorWithCount(commandlist.GetCommandBuffer(), numRects, scissors);
	}
	void GraphicsDevice_Vulkan::BindViewports(uint32_t NumViewports, const Viewport* pViewports, CommandList cmd)
	{
		SDL_assert(pViewports != nullptr);
		VkViewport vp[16];
		SDL_assert(NumViewports < SDL_arraysize(vp));
		SDL_assert(NumViewports < properties2.properties.limits.maxViewports);
		for (uint32_t i = 0; i < NumViewports; ++i)
		{
			vp[i].x = pViewports[i].top_left_x;
			vp[i].y = pViewports[i].top_left_y + pViewports[i].height;
			vp[i].width = std::max(1.0f, pViewports[i].width); // must be > 0 according to validation layer
			vp[i].height = -pViewports[i].height;
			vp[i].minDepth = pViewports[i].min_depth;
			vp[i].maxDepth = pViewports[i].max_depth;
		}
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdSetViewportWithCount(commandlist.GetCommandBuffer(), NumViewports, vp);
	}
	void GraphicsDevice_Vulkan::BindResource(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_SRV_COUNT);
		auto& binder = commandlist.binder;
		if (binder.table.SRV[slot].internal_state != resource->internal_state || binder.table.SRV_index[slot] != subresource)
		{
			binder.table.SRV[slot] = *resource;
			binder.table.SRV_index[slot] = subresource;
			binder.dirty |= DescriptorBinder::DIRTY_DESCRIPTOR;
		}
	}
	void GraphicsDevice_Vulkan::BindResources(const GPUResource *const* resources, uint32_t slot, uint32_t count, CommandList cmd)
	{
		if (resources != nullptr)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				BindResource(resources[i], slot + i, cmd, -1);
			}
		}
	}
	void GraphicsDevice_Vulkan::BindUAV(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_UAV_COUNT);
		auto& binder = commandlist.binder;
		if (binder.table.UAV[slot].internal_state != resource->internal_state || binder.table.UAV_index[slot] != subresource)
		{
			binder.table.UAV[slot] = *resource;
			binder.table.UAV_index[slot] = subresource;
			binder.dirty |= DescriptorBinder::DIRTY_DESCRIPTOR;
		}
	}
	void GraphicsDevice_Vulkan::BindUAVs(const GPUResource *const* resources, uint32_t slot, uint32_t count, CommandList cmd)
	{
		if (resources != nullptr)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				BindUAV(resources[i], slot + i, cmd, -1);
			}
		}
	}
	void GraphicsDevice_Vulkan::BindSampler(const Sampler* sampler, uint32_t slot, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_SAMPLER_COUNT);
		auto& binder = commandlist.binder;
		if (binder.table.SAM[slot].internal_state != sampler->internal_state)
		{
			binder.table.SAM[slot] = *sampler;
			binder.dirty |= DescriptorBinder::DIRTY_DESCRIPTOR;
		}
	}
	void GraphicsDevice_Vulkan::BindConstantBuffer(const GPUBuffer* buffer, uint32_t slot, CommandList cmd, uint64_t offset)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_CBV_COUNT);
		auto& binder = commandlist.binder;

		if (binder.table.CBV[slot].internal_state != buffer->internal_state)
		{
			binder.table.CBV[slot] = *buffer;
			binder.dirty |= DescriptorBinder::DIRTY_DESCRIPTOR;
		}

		if (binder.table.CBV_offset[slot] != offset)
		{
			binder.table.CBV_offset[slot] = offset;
			if (slot < dynamic_cbv_count)
			{
				binder.dirty |= DescriptorBinder::DIRTY_SET_OR_OFFSET;
			}
			else
			{
				binder.dirty |= DescriptorBinder::DIRTY_DESCRIPTOR;
			}
		}
	}
	void GraphicsDevice_Vulkan::BindVertexBuffers(const GPUBuffer *const* vertexBuffers, uint32_t slot, uint32_t count, const uint32_t* strides, const uint64_t* offsets, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);

		VkDeviceSize voffsets[8] = {};
		VkDeviceSize vstrides[8] = {};
		VkBuffer vbuffers[8] = {};
		SDL_assert(count <= 8);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (!wiGraphicsGPUResourceIsValid(vertexBuffers[i]))
			{
				vbuffers[i] = nullBuffer;
			}
			else
			{
				auto internal_state = to_internal(vertexBuffers[i]);
				vbuffers[i] = internal_state->resource;
				if (offsets != nullptr)
				{
					voffsets[i] = offsets[i];
				}
				if (strides != nullptr)
				{
					vstrides[i] = strides[i];
				}
			}
		}

		vkCmdBindVertexBuffers2(commandlist.GetCommandBuffer(), slot, count, vbuffers, voffsets, nullptr, vstrides);
	}
	void GraphicsDevice_Vulkan::BindIndexBuffer(const GPUBuffer* indexBuffer, const IndexBufferFormat format, uint64_t offset, CommandList cmd)
	{
		if (indexBuffer != nullptr)
		{
			auto internal_state = to_internal(indexBuffer);
			CommandList_Vulkan& commandlist = GetCommandList(cmd);
			vkCmdBindIndexBuffer(commandlist.GetCommandBuffer(), internal_state->resource, offset, format == IndexBufferFormat::UINT16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
		}
	}
	void GraphicsDevice_Vulkan::BindStencilRef(uint32_t value, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		if (commandlist.prev_stencilref != value)
		{
			commandlist.prev_stencilref = value;
			vkCmdSetStencilReference(commandlist.GetCommandBuffer(), VK_STENCIL_FRONT_AND_BACK, value);
		}
	}
	void GraphicsDevice_Vulkan::BindBlendFactor(float r, float g, float b, float a, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		float blendConstants[] = { r, g, b, a };
		vkCmdSetBlendConstants(commandlist.GetCommandBuffer(), blendConstants);
	}
	void GraphicsDevice_Vulkan::BindShadingRate(ShadingRate rate, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		if (CheckCapability(GraphicsDeviceCapability::VARIABLE_RATE_SHADING) && commandlist.prev_shadingrate != rate)
		{
			commandlist.prev_shadingrate = rate;

			VkExtent2D fragmentSize;
			switch (rate)
			{
			case ShadingRate::RATE_1X1:
				fragmentSize.width = 1;
				fragmentSize.height = 1;
				break;
			case ShadingRate::RATE_1X2:
				fragmentSize.width = 1;
				fragmentSize.height = 2;
				break;
			case ShadingRate::RATE_2X1:
				fragmentSize.width = 2;
				fragmentSize.height = 1;
				break;
			case ShadingRate::RATE_2X2:
				fragmentSize.width = 2;
				fragmentSize.height = 2;
				break;
			case ShadingRate::RATE_2X4:
				fragmentSize.width = 2;
				fragmentSize.height = 4;
				break;
			case ShadingRate::RATE_4X2:
				fragmentSize.width = 4;
				fragmentSize.height = 2;
				break;
			case ShadingRate::RATE_4X4:
				fragmentSize.width = 4;
				fragmentSize.height = 4;
				break;
			default:
				break;
			}

			VkFragmentShadingRateCombinerOpKHR combiner[] = {
				VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
				VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR
			};

			if (fragment_shading_rate_properties.fragmentShadingRateNonTrivialCombinerOps == VK_TRUE)
			{
				if (fragment_shading_rate_features.primitiveFragmentShadingRate == VK_TRUE)
				{
					combiner[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
				}
				if (fragment_shading_rate_features.attachmentFragmentShadingRate == VK_TRUE)
				{
					combiner[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
				}
			}
			else
			{
				if (fragment_shading_rate_features.primitiveFragmentShadingRate == VK_TRUE)
				{
					combiner[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
				}
				if (fragment_shading_rate_features.attachmentFragmentShadingRate == VK_TRUE)
				{
					combiner[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
				}
			}

			vkCmdSetFragmentShadingRateKHR(
				commandlist.GetCommandBuffer(),
				&fragmentSize,
				combiner
			);
		}
	}
	void GraphicsDevice_Vulkan::BindPipelineState(const PipelineState* pso, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		if (commandlist.active_cs != nullptr)
		{
			commandlist.binder.dirty |= DescriptorBinder::DIRTY_SET_OR_OFFSET; // need to refresh bind point at least when gfx/compute changes!
		}
		commandlist.active_cs = nullptr;
		commandlist.active_rt = nullptr;

		auto internal_state = to_internal(pso);

		if (commandlist.layout.pipeline_layout != internal_state->layout.pipeline_layout)
		{
			commandlist.layout = internal_state->layout;
			commandlist.binder.dirty |= DescriptorBinder::DIRTY_DESCRIPTOR;
		}

		if (internal_state->pipeline != VK_NULL_HANDLE)
		{
			if (commandlist.active_pso != pso)
			{
				vkCmdBindPipeline(commandlist.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, internal_state->pipeline);
			}
			else
				return; // early exit for static pso

			commandlist.prev_pipeline_hash = {};
			commandlist.dirty_pso = false;
		}
		else
		{
			PipelineHash pipeline_hash;
			pipeline_hash.pso = pso;
			pipeline_hash.renderpass_hash = commandlist.renderpass_info.get_hash();
			if (commandlist.prev_pipeline_hash == pipeline_hash)
			{
				commandlist.active_pso = pso;
				return; // early exit for dynamic pso|renderpass
			}
			commandlist.prev_pipeline_hash = pipeline_hash;
			commandlist.dirty_pso = true;
		}

		commandlist.active_pso = pso;
	}
	void GraphicsDevice_Vulkan::BindComputeShader(const Shader* cs, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		if (commandlist.active_cs == cs)
			return;
		if (commandlist.active_pso != nullptr)
		{
			commandlist.binder.dirty |= DescriptorBinder::DIRTY_SET_OR_OFFSET; // need to refresh bind point at least when gfx/compute changes!
		}
		commandlist.active_pso = nullptr;
		commandlist.active_rt = nullptr;

		SDL_assert(cs->stage == ShaderStage::CS || cs->stage == ShaderStage::LIB);

		auto internal_state = to_internal(cs);

		if (cs->stage == ShaderStage::CS)
		{
			vkCmdBindPipeline(commandlist.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, internal_state->pipeline_cs);
		}

		if (commandlist.layout.pipeline_layout != internal_state->layout.pipeline_layout)
		{
			commandlist.layout = internal_state->layout;
			commandlist.binder.dirty |= DescriptorBinder::DIRTY_DESCRIPTOR;
		}

		commandlist.active_cs = cs;
	}
	void GraphicsDevice_Vulkan::BindDepthBounds(float min_bounds, float max_bounds, CommandList cmd)
	{
		if (features2.features.depthBounds == VK_TRUE)
		{
			CommandList_Vulkan& commandlist = GetCommandList(cmd);
			vkCmdSetDepthBounds(commandlist.GetCommandBuffer(), min_bounds, max_bounds);
		}
	}
	void GraphicsDevice_Vulkan::Draw(uint32_t vertexCount, uint32_t startVertexLocation, CommandList cmd)
	{
		predraw(cmd);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDraw(commandlist.GetCommandBuffer(), vertexCount, 1, startVertexLocation, 0);
	}
	void GraphicsDevice_Vulkan::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation, CommandList cmd)
	{
		predraw(cmd);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawIndexed(commandlist.GetCommandBuffer(), indexCount, 1, startIndexLocation, baseVertexLocation, 0);
	}
	void GraphicsDevice_Vulkan::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation, CommandList cmd)
	{
		predraw(cmd);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDraw(commandlist.GetCommandBuffer(), vertexCount, instanceCount, startVertexLocation, startInstanceLocation);
	}
	void GraphicsDevice_Vulkan::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation, CommandList cmd)
	{
		predraw(cmd);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawIndexed(commandlist.GetCommandBuffer(), indexCount, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
	}
	void GraphicsDevice_Vulkan::DrawInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		predraw(cmd);
		auto internal_state = to_internal(args);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawIndirect(commandlist.GetCommandBuffer(), internal_state->resource, args_offset, 1, (uint32_t)sizeof(VkDrawIndirectCommand));
	}
	void GraphicsDevice_Vulkan::DrawIndexedInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		predraw(cmd);
		auto internal_state = to_internal(args);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawIndexedIndirect(commandlist.GetCommandBuffer(), internal_state->resource, args_offset, 1, sizeof(VkDrawIndexedIndirectCommand));
	}
	void GraphicsDevice_Vulkan::DrawInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd)
	{
		predraw(cmd);
		auto args_internal = to_internal(args);
		auto count_internal = to_internal(count);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawIndirectCount(commandlist.GetCommandBuffer(), args_internal->resource, args_offset, count_internal->resource, count_offset, max_count, sizeof(VkDrawIndirectCommand));
	}
	void GraphicsDevice_Vulkan::DrawIndexedInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd)
	{
		predraw(cmd);
		auto args_internal = to_internal(args);
		auto count_internal = to_internal(count);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawIndexedIndirectCount(commandlist.GetCommandBuffer(), args_internal->resource, args_offset, count_internal->resource, count_offset, max_count, sizeof(VkDrawIndexedIndirectCommand));
	}
	void GraphicsDevice_Vulkan::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ, CommandList cmd)
	{
		predispatch(cmd);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDispatch(commandlist.GetCommandBuffer(), threadGroupCountX, threadGroupCountY, threadGroupCountZ);
	}
	void GraphicsDevice_Vulkan::DispatchIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		predispatch(cmd);
		auto internal_state = to_internal(args);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDispatchIndirect(commandlist.GetCommandBuffer(), internal_state->resource, args_offset);
	}
	void GraphicsDevice_Vulkan::DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ, CommandList cmd)
	{
		predraw(cmd);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawMeshTasksEXT(commandlist.GetCommandBuffer(), threadGroupCountX, threadGroupCountY, threadGroupCountZ);
	}
	void GraphicsDevice_Vulkan::DispatchMeshIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		predraw(cmd);
		auto internal_state = to_internal(args);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawMeshTasksIndirectEXT(commandlist.GetCommandBuffer(), internal_state->resource, args_offset, 1, sizeof(VkDispatchIndirectCommand));
	}
	void GraphicsDevice_Vulkan::DispatchMeshIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd)
	{
		predraw(cmd);
		auto args_internal = to_internal(args);
		auto count_internal = to_internal(count);
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		vkCmdDrawMeshTasksIndirectCountEXT(commandlist.GetCommandBuffer(), args_internal->resource, args_offset, count_internal->resource, count_offset, max_count, sizeof(VkDispatchIndirectCommand));
	}
	void GraphicsDevice_Vulkan::CopyResource(const GPUResource* pDst, const GPUResource* pSrc, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		if (pDst->type == GPUResource::Type::TEXTURE && pSrc->type == GPUResource::Type::TEXTURE)
		{
			auto internal_state_src = to_internal<Texture>(pSrc);
			auto internal_state_dst = to_internal<Texture>(pDst);

			const TextureDesc& src_desc = *wiGraphicsTextureGetDesc((const Texture*)pSrc);
			const TextureDesc& dst_desc = *wiGraphicsTextureGetDesc((const Texture*)pDst);

			if (src_desc.usage == Usage::UPLOAD && dst_desc.usage == Usage::DEFAULT)
			{
				// CPU (buffer) -> GPU (texture)
				VkBufferImageCopy copy = {};
				copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				const uint32_t data_stride = GetFormatStride(dst_desc.format);
				const uint32_t block_size = GetFormatBlockSize(dst_desc.format);
				for (uint32_t slice = 0; slice < dst_desc.array_size; ++slice)
				{
					copy.imageSubresource.baseArrayLayer = slice;
					copy.imageSubresource.layerCount = 1;
					uint32_t mip_width = dst_desc.width;
					uint32_t mip_height = dst_desc.height;
					uint32_t mip_depth = dst_desc.depth;
					for (uint32_t mip = 0; mip < dst_desc.mip_levels; ++mip)
					{
						const uint32_t num_blocks_x = mip_width / block_size;
						const uint32_t num_blocks_y = mip_height / block_size;
						copy.imageExtent.width = mip_width;
						copy.imageExtent.height = mip_height;
						copy.imageExtent.depth = mip_depth;
						copy.imageSubresource.mipLevel = mip;
						vkCmdCopyBufferToImage(
							commandlist.GetCommandBuffer(),
							internal_state_src->staging_resource,
							internal_state_dst->resource,
							_ConvertImageLayout(ResourceState::COPY_DST),
							1,
							&copy
						);

						copy.bufferOffset += num_blocks_x * num_blocks_y * mip_depth * data_stride;
						mip_width = std::max(1u, mip_width / 2);
						mip_height = std::max(1u, mip_height / 2);
						mip_depth = std::max(1u, mip_depth / 2);
					}
				}
			}
			else if (src_desc.usage == Usage::DEFAULT && dst_desc.usage == Usage::READBACK)
			{
				// GPU (texture) -> CPU (buffer)
				VkBufferImageCopy copy = {};
				copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				const uint32_t data_stride = GetFormatStride(dst_desc.format);
				const uint32_t block_size = GetFormatBlockSize(dst_desc.format);
				for (uint32_t slice = 0; slice < dst_desc.array_size; ++slice)
				{
					copy.imageSubresource.baseArrayLayer = slice;
					copy.imageSubresource.layerCount = 1;
					uint32_t mip_width = dst_desc.width;
					uint32_t mip_height = dst_desc.height;
					uint32_t mip_depth = dst_desc.depth;
					for (uint32_t mip = 0; mip < dst_desc.mip_levels; ++mip)
					{
						const uint32_t num_blocks_x = mip_width / block_size;
						const uint32_t num_blocks_y = mip_height / block_size;
						copy.imageExtent.width = mip_width;
						copy.imageExtent.height = mip_height;
						copy.imageExtent.depth = mip_depth;
						copy.imageSubresource.mipLevel = mip;
						vkCmdCopyImageToBuffer(
							commandlist.GetCommandBuffer(),
							internal_state_src->resource,
							_ConvertImageLayout(ResourceState::COPY_SRC),
							internal_state_dst->staging_resource,
							1,
							&copy
						);

						copy.bufferOffset += num_blocks_x * num_blocks_y * mip_depth * data_stride;
						mip_width = std::max(1u, mip_width / 2);
						mip_height = std::max(1u, mip_height / 2);
						mip_depth = std::max(1u, mip_depth / 2);
					}
				}
			}
			else if (src_desc.usage == Usage::DEFAULT && dst_desc.usage == Usage::DEFAULT)
			{
				// GPU (texture) -> GPU (texture)
				VkImageCopy copy = {};
				copy.extent.width = dst_desc.width;
				copy.extent.height = dst_desc.height;
				copy.extent.depth = std::max(1u, dst_desc.depth);

				copy.srcOffset.x = 0;
				copy.srcOffset.y = 0;
				copy.srcOffset.z = 0;

				copy.dstOffset.x = 0;
				copy.dstOffset.y = 0;
				copy.dstOffset.z = 0;

				if (IsFormatDepthSupport(src_desc.format))
				{
					copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (IsFormatStencilSupport(src_desc.format))
					{
						copy.srcSubresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
				}
				else
				{
					copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				}
				copy.srcSubresource.baseArrayLayer = 0;
				copy.srcSubresource.layerCount = src_desc.array_size;
				copy.srcSubresource.mipLevel = 0;

				if (has_flag(dst_desc.bind_flags, BindFlag::DEPTH_STENCIL))
				{
					copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (IsFormatStencilSupport(dst_desc.format))
					{
						copy.dstSubresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
				}
				else
				{
					copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				}
				copy.dstSubresource.baseArrayLayer = 0;
				copy.dstSubresource.layerCount = dst_desc.array_size;
				copy.dstSubresource.mipLevel = 0;

				vkCmdCopyImage(commandlist.GetCommandBuffer(),
					internal_state_src->resource, _ConvertImageLayout(ResourceState::COPY_SRC),
					internal_state_dst->resource, _ConvertImageLayout(ResourceState::COPY_DST),
					1, &copy
				);
			}
			else
			{
				// CPU (buffer) -> CPU (buffer)
				VkBufferCopy copy = {};
				copy.srcOffset = 0;
				copy.dstOffset = 0;
				copy.size = std::min(pSrc->mapped_size, pSrc->mapped_size);

				vkCmdCopyBuffer(commandlist.GetCommandBuffer(),
					internal_state_src->staging_resource,
					internal_state_dst->staging_resource,
					1, &copy
				);
			}
		}
		else if (pDst->type == GPUResource::Type::BUFFER && pSrc->type == GPUResource::Type::BUFFER)
		{
			auto internal_state_src = to_internal<GPUBuffer>(pSrc);
			auto internal_state_dst = to_internal<GPUBuffer>(pDst);

			const GPUBufferDesc& src_desc = *wiGraphicsGPUBufferGetDesc((const GPUBuffer*)pSrc);
			const GPUBufferDesc& dst_desc = *wiGraphicsGPUBufferGetDesc((const GPUBuffer*)pDst);

			VkBufferCopy copy = {};
			copy.srcOffset = 0;
			copy.dstOffset = 0;
			copy.size = std::min(src_desc.size, dst_desc.size);

			vkCmdCopyBuffer(commandlist.GetCommandBuffer(),
				internal_state_src->resource,
				internal_state_dst->resource,
				1, &copy
			);
		}
	}
	void GraphicsDevice_Vulkan::CopyBuffer(const GPUBuffer* pDst, uint64_t dst_offset, const GPUBuffer* pSrc, uint64_t src_offset, uint64_t size, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		auto internal_state_src = to_internal(pSrc);
		auto internal_state_dst = to_internal(pDst);

		VkBufferCopy copy = {};
		copy.srcOffset = src_offset;
		copy.dstOffset = dst_offset;
		copy.size = size;

		vkCmdCopyBuffer(commandlist.GetCommandBuffer(),
			internal_state_src->resource,
			internal_state_dst->resource,
			1, &copy
		);
	}
	void GraphicsDevice_Vulkan::CopyTexture(const Texture* dst, uint32_t dstX, uint32_t dstY, uint32_t dstZ, uint32_t dstMip, uint32_t dstSlice, const Texture* src, uint32_t srcMip, uint32_t srcSlice, CommandList cmd, const Box* srcbox, ImageAspect dst_aspect, ImageAspect src_aspect)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		auto src_internal = to_internal(src);
		auto dst_internal = to_internal(dst);

		const TextureDesc& src_desc = *wiGraphicsTextureGetDesc(src);
		const TextureDesc& dst_desc = *wiGraphicsTextureGetDesc(dst);

		if (src_desc.usage == Usage::UPLOAD && dst_desc.usage == Usage::DEFAULT)
		{
			// CPU (buffer) -> GPU (texture)
			VkBufferImageCopy copy = {};
			const uint32_t subresource_index = ComputeSubresource(srcMip, srcSlice, src_aspect, src_desc.mip_levels, src_desc.array_size);
			SDL_assert(src->mapped_subresource_count > subresource_index);
			const SubresourceData& data0 = src->mapped_subresources[0];
			const SubresourceData& data = src->mapped_subresources[subresource_index];
			copy.bufferOffset = (uint64_t)data.data_ptr - (uint64_t)data0.data_ptr;
			copy.bufferRowLength = std::max(1u, src_desc.width >> srcMip);
			copy.bufferImageHeight = std::max(1u, src_desc.height >> srcMip);
			copy.imageSubresource.mipLevel = dstMip;
			copy.imageSubresource.baseArrayLayer = dstSlice;
			copy.imageSubresource.layerCount = 1;
			copy.imageSubresource.aspectMask = _ConvertImageAspect(dst_aspect);
			copy.imageOffset.x = dstX;
			copy.imageOffset.y = dstY;
			copy.imageOffset.z = dstZ;
			if (srcbox == nullptr)
			{
				copy.imageExtent.width = src_desc.width;
				copy.imageExtent.height = src_desc.height;
				copy.imageExtent.depth = src_desc.depth;
			}
			else
			{
				copy.imageExtent.width = srcbox->right - srcbox->left;
				copy.imageExtent.height = srcbox->bottom - srcbox->top;
				copy.imageExtent.depth = srcbox->back - srcbox->front;
				copy.bufferOffset += srcbox->top * data.row_pitch + srcbox->left * GetFormatStride(src_desc.format) / GetFormatBlockSize(src_desc.format);
			}
			vkCmdCopyBufferToImage(commandlist.GetCommandBuffer(), src_internal->staging_resource, dst_internal->resource, _ConvertImageLayout(ResourceState::COPY_DST), 1, &copy);
		}
		else if (src_desc.usage == Usage::DEFAULT && dst_desc.usage == Usage::READBACK)
		{
			// GPU (texture) -> CPU (buffer)
			VkBufferImageCopy copy = {};
			const uint32_t subresource_index = ComputeSubresource(dstMip, dstSlice, dst_aspect, dst_desc.mip_levels, dst_desc.array_size);
			SDL_assert(dst->mapped_subresource_count > subresource_index);
			const SubresourceData& data0 = dst->mapped_subresources[0];
			const SubresourceData& data = dst->mapped_subresources[subresource_index];
			copy.bufferOffset = (uint64_t)data.data_ptr - (uint64_t)data0.data_ptr;
			copy.bufferRowLength = std::max(1u, dst_desc.width >> dstMip);
			copy.bufferImageHeight = std::max(1u, dst_desc.height >> dstMip);
			copy.imageSubresource.mipLevel = srcMip;
			copy.imageSubresource.baseArrayLayer = srcSlice;
			copy.imageSubresource.layerCount = 1;
			copy.imageSubresource.aspectMask = _ConvertImageAspect(src_aspect);
			if (srcbox == nullptr)
			{
				copy.imageExtent.width = src_desc.width;
				copy.imageExtent.height = src_desc.height;
				copy.imageExtent.depth = src_desc.depth;
			}
			else
			{
				copy.imageExtent.width = srcbox->right - srcbox->left;
				copy.imageExtent.height = srcbox->bottom - srcbox->top;
				copy.imageExtent.depth = srcbox->back - srcbox->front;
				copy.imageOffset.x = srcbox->left;
				copy.imageOffset.y = srcbox->top;
				copy.imageOffset.z = srcbox->front;
			}
			vkCmdCopyImageToBuffer(commandlist.GetCommandBuffer(), src_internal->resource, _ConvertImageLayout(ResourceState::COPY_SRC), dst_internal->staging_resource, 1, &copy);
		}
		else if (src_desc.usage == Usage::DEFAULT && dst_desc.usage == Usage::DEFAULT)
		{
			// GPU (texture) -> GPU (texture)
			VkImageCopy copy = {};
			copy.dstSubresource.aspectMask = _ConvertImageAspect(dst_aspect);
			copy.dstSubresource.baseArrayLayer = dstSlice;
			copy.dstSubresource.layerCount = 1;
			copy.dstSubresource.mipLevel = dstMip;
			copy.dstOffset.x = dstX;
			copy.dstOffset.y = dstY;
			copy.dstOffset.z = dstZ;

			copy.srcSubresource.aspectMask = _ConvertImageAspect(src_aspect);
			copy.srcSubresource.baseArrayLayer = srcSlice;
			copy.srcSubresource.layerCount = 1;
			copy.srcSubresource.mipLevel = srcMip;

			if (srcbox == nullptr)
			{
				copy.srcOffset.x = 0;
				copy.srcOffset.y = 0;
				copy.srcOffset.z = 0;
				if (src->desc.format == Format::NV12 && src_aspect == ImageAspect::CHROMINANCE)
				{
					copy.extent.width = std::min(dst->desc.width, src->desc.width / 2);
					copy.extent.height = std::min(dst->desc.height, src->desc.height / 2);
				}
				else
				{
					copy.extent.width = std::min(dst->desc.width, src->desc.width);
					copy.extent.height = std::min(dst->desc.height, src->desc.height);
				}
				copy.extent.depth = std::min(dst->desc.depth, src->desc.depth);

				copy.extent.width = std::max(1u, copy.extent.width >> srcMip);
				copy.extent.height = std::max(1u, copy.extent.height >> srcMip);
				copy.extent.depth = std::max(1u, copy.extent.depth >> srcMip);
			}
			else
			{
				copy.srcOffset.x = srcbox->left;
				copy.srcOffset.y = srcbox->top;
				copy.srcOffset.z = srcbox->front;
				copy.extent.width = srcbox->right - srcbox->left;
				copy.extent.height = srcbox->bottom - srcbox->top;
				copy.extent.depth = srcbox->back - srcbox->front;
			}

			vkCmdCopyImage(
				commandlist.GetCommandBuffer(),
				src_internal->resource,
				_ConvertImageLayout(ResourceState::COPY_SRC),
				dst_internal->resource,
				_ConvertImageLayout(ResourceState::COPY_DST),
				1,
				&copy
			);
		}
		else
		{
			SDL_assert(0); // not supported
		}
	}
	void GraphicsDevice_Vulkan::QueryBegin(const GPUQueryHeap* heap, uint32_t index, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		auto internal_state = to_internal(heap);

		switch (heap->desc.type)
		{
		case GpuQueryType::OCCLUSION_BINARY:
			vkCmdBeginQuery(commandlist.GetCommandBuffer(), internal_state->pool, index, 0);
			break;
		case GpuQueryType::OCCLUSION:
			vkCmdBeginQuery(commandlist.GetCommandBuffer(), internal_state->pool, index, VK_QUERY_CONTROL_PRECISE_BIT);
			break;
		case GpuQueryType::TIMESTAMP:
			break;
		}
	}
	void GraphicsDevice_Vulkan::QueryEnd(const GPUQueryHeap* heap, uint32_t index, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		auto internal_state = to_internal(heap);

		switch (heap->desc.type)
		{
		case GpuQueryType::TIMESTAMP:
			vkCmdWriteTimestamp2(commandlist.GetCommandBuffer(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, internal_state->pool, index);
			break;
		case GpuQueryType::OCCLUSION_BINARY:
		case GpuQueryType::OCCLUSION:
			vkCmdEndQuery(commandlist.GetCommandBuffer(), internal_state->pool, index);
			break;
		}
	}
	void GraphicsDevice_Vulkan::QueryResolve(const GPUQueryHeap* heap, uint32_t index, uint32_t count, const GPUBuffer* dest, uint64_t dest_offset, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);

		auto internal_state = to_internal(heap);
		auto dst_internal = to_internal(dest);

		VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
		flags |= VK_QUERY_RESULT_WAIT_BIT;

		switch (heap->desc.type)
		{
		case GpuQueryType::OCCLUSION_BINARY:
			flags |= VK_QUERY_RESULT_PARTIAL_BIT;
			break;
		default:
			break;
		}

		vkCmdCopyQueryPoolResults(
			commandlist.GetCommandBuffer(),
			internal_state->pool,
			index,
			count,
			dst_internal->resource,
			dest_offset,
			sizeof(uint64_t),
			flags
		);

	}
	void GraphicsDevice_Vulkan::QueryReset(const GPUQueryHeap* heap, uint32_t index, uint32_t count, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);

		auto internal_state = to_internal(heap);

		vkCmdResetQueryPool(
			commandlist.GetCommandBuffer(),
			internal_state->pool,
			index,
			count
		);

	}
	void GraphicsDevice_Vulkan::Barrier(const GPUBarrier* barriers, uint32_t numBarriers, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		auto sanitize_barrier_stage_mask = [&](VkPipelineStageFlags2 mask)
		{
			if (commandlist.queue == QUEUE_COMPUTE)
			{
				constexpr VkPipelineStageFlags2 kInvalidGraphicsOnlyStages =
					VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
					VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
					VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
					VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
					VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;

				if ((mask & kInvalidGraphicsOnlyStages) != 0)
				{
					mask &= ~kInvalidGraphicsOnlyStages;

					if (mask == VK_PIPELINE_STAGE_2_NONE)
					{
						return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
					}
				}
			}

			return mask;
		};

		auto sanitize_barrier_access_mask = [&](VkAccessFlags2 mask)
		{
			if (commandlist.queue == QUEUE_COMPUTE)
			{
				if (mask & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT)
				{
					mask &= ~VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
					mask |= VK_ACCESS_2_SHADER_READ_BIT;
				}
				if (mask & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)
				{
					mask &= ~VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
					mask |= VK_ACCESS_2_SHADER_WRITE_BIT;
				}
				if (mask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
				{
					mask &= ~VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
					mask |= VK_ACCESS_2_SHADER_READ_BIT;
				}
				if (mask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
				{
					mask &= ~VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
					mask |= VK_ACCESS_2_SHADER_WRITE_BIT;
				}
				if (mask & VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR)
				{
					mask &= ~VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
					mask |= VK_ACCESS_2_SHADER_READ_BIT;
				}

				if (mask & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT)
				{
					mask &= ~VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
					mask |= VK_ACCESS_2_SHADER_READ_BIT;
				}

				if (mask & VK_ACCESS_2_INDEX_READ_BIT)
				{
					mask &= ~VK_ACCESS_2_INDEX_READ_BIT;
					mask |= VK_ACCESS_2_SHADER_READ_BIT;
				}
			}

			return mask;
		};

		auto& memoryBarriers = commandlist.frame_memoryBarriers;
		auto& imageBarriers = commandlist.frame_imageBarriers;
		auto& bufferBarriers = commandlist.frame_bufferBarriers;

		for (uint32_t i = 0; i < numBarriers; ++i)
		{
			const GPUBarrier& barrier = barriers[i];

			if (barrier.type == GPUBarrier::Type::IMAGE && (barrier.image.texture == nullptr || !wiGraphicsGPUResourceIsValid(barrier.image.texture)))
				continue;
			if (barrier.type == GPUBarrier::Type::BUFFER && (barrier.buffer.buffer == nullptr || !wiGraphicsGPUResourceIsValid(barrier.buffer.buffer)))
				continue;

			switch (barrier.type)
			{
			default:
			case GPUBarrier::Type::MEMORY:
			case GPUBarrier::Type::ALIASING:
			{
				VkMemoryBarrier2 barrierdesc = {};
				barrierdesc.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
				barrierdesc.pNext = nullptr;
				barrierdesc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				barrierdesc.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				barrierdesc.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
				barrierdesc.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;

				if (CheckCapability(GraphicsDeviceCapability::RAYTRACING))
				{
					barrierdesc.srcStageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
					barrierdesc.dstStageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
					barrierdesc.srcAccessMask |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
					barrierdesc.dstAccessMask |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
				}

				if (CheckCapability(GraphicsDeviceCapability::GRAPHICS_DEVICE_CAPABILITY_PREDICATION))
				{
					barrierdesc.srcStageMask |= VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
					barrierdesc.dstStageMask |= VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
					barrierdesc.srcAccessMask |= VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
					barrierdesc.dstAccessMask |= VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
				}

					arrput(memoryBarriers, barrierdesc);
			}
			break;
			case GPUBarrier::Type::IMAGE:
			{
				const TextureDesc& desc = barrier.image.texture->desc;
				auto internal_state = to_internal(barrier.image.texture);

				VkImageMemoryBarrier2 barrierdesc = {};
				barrierdesc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrierdesc.pNext = nullptr;
				barrierdesc.image = internal_state->resource;
				barrierdesc.oldLayout = _ConvertImageLayout(barrier.image.layout_before);
				barrierdesc.newLayout = _ConvertImageLayout(barrier.image.layout_after);
				barrierdesc.srcStageMask = sanitize_barrier_stage_mask(_ConvertPipelineStage(barrier.image.layout_before));
				barrierdesc.dstStageMask = sanitize_barrier_stage_mask(_ConvertPipelineStage(barrier.image.layout_after));
				barrierdesc.srcAccessMask = sanitize_barrier_access_mask(_ParseResourceState(barrier.image.layout_before));
				barrierdesc.dstAccessMask = sanitize_barrier_access_mask(_ParseResourceState(barrier.image.layout_after));
				if (IsFormatDepthSupport(desc.format))
				{
					barrierdesc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					if (IsFormatStencilSupport(desc.format))
					{
						barrierdesc.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
					}
				}
				else
				{
					barrierdesc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				}
				if (barrier.image.aspect != nullptr)
				{
					barrierdesc.subresourceRange.aspectMask = _ConvertImageAspect(*barrier.image.aspect);
				}
				if (barrier.image.mip >= 0 || barrier.image.slice >= 0)
				{
					barrierdesc.subresourceRange.baseMipLevel = (uint32_t)std::max(0, barrier.image.mip);
					barrierdesc.subresourceRange.levelCount = 1;
					barrierdesc.subresourceRange.baseArrayLayer = (uint32_t)std::max(0, barrier.image.slice);
					barrierdesc.subresourceRange.layerCount = 1;
				}
				else
				{
					barrierdesc.subresourceRange.baseMipLevel = 0;
					barrierdesc.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
					barrierdesc.subresourceRange.baseArrayLayer = 0;
					barrierdesc.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
				}
				barrierdesc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrierdesc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

				if (commandlist.queue == QUEUE_COPY)
				{
					// Simplified barrier on copy queue:
					barrierdesc.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
					barrierdesc.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
					barrierdesc.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
					barrierdesc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
				}
				else if (commandlist.queue == QUEUE_VIDEO_DECODE)
				{
					// Simplified barrier on video queue:
					barrierdesc.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
					barrierdesc.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
					barrierdesc.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
					barrierdesc.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
				}

				arrput(imageBarriers, barrierdesc);
			}
			break;
			case GPUBarrier::Type::BUFFER:
			{
				const GPUBufferDesc& desc = barrier.buffer.buffer->desc;
				auto internal_state = to_internal(barrier.buffer.buffer);

				VkBufferMemoryBarrier2 barrierdesc = {};
				barrierdesc.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
				barrierdesc.pNext = nullptr;
				barrierdesc.buffer = internal_state->resource;
				barrierdesc.size = desc.size;
				barrierdesc.offset = 0;
				barrierdesc.srcStageMask = sanitize_barrier_stage_mask(_ConvertPipelineStage(barrier.buffer.state_before));
				barrierdesc.dstStageMask = sanitize_barrier_stage_mask(_ConvertPipelineStage(barrier.buffer.state_after));
				barrierdesc.srcAccessMask = sanitize_barrier_access_mask(_ParseResourceState(barrier.buffer.state_before));
				barrierdesc.dstAccessMask = sanitize_barrier_access_mask(_ParseResourceState(barrier.buffer.state_after));
				barrierdesc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrierdesc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

				if (has_flag(desc.misc_flags, ResourceMiscFlag::RAY_TRACING))
				{
					SDL_assert(CheckCapability(GraphicsDeviceCapability::RAYTRACING));
					barrierdesc.srcStageMask |= _ConvertPipelineStage(ResourceState::RAYTRACING_ACCELERATION_STRUCTURE);
					barrierdesc.dstStageMask |= _ConvertPipelineStage(ResourceState::RAYTRACING_ACCELERATION_STRUCTURE);
				}

				if (has_flag(desc.misc_flags, ResourceMiscFlag::RESOURCE_MISC_PREDICATION))
				{
					SDL_assert(CheckCapability(GraphicsDeviceCapability::GRAPHICS_DEVICE_CAPABILITY_PREDICATION));
					barrierdesc.srcStageMask |= _ConvertPipelineStage(ResourceState::PREDICATION);
					barrierdesc.dstStageMask |= _ConvertPipelineStage(ResourceState::PREDICATION);
				}

				if (commandlist.queue == QUEUE_COPY)
				{
					// Simplified barrier on copy queue:
					barrierdesc.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
					barrierdesc.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
					barrierdesc.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
					barrierdesc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
				}
				else if (commandlist.queue == QUEUE_VIDEO_DECODE)
				{
					// Simplified barrier on video queue:
					barrierdesc.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
					barrierdesc.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
					barrierdesc.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
					barrierdesc.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
				}

					arrput(bufferBarriers, barrierdesc);
			}
			break;
			}
		}

		if (arrlenu(memoryBarriers) > 0 ||
			arrlenu(bufferBarriers) > 0 ||
			arrlenu(imageBarriers) > 0
			)
		{
			VkDependencyInfo dependencyInfo = {};
			dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dependencyInfo.memoryBarrierCount = static_cast<uint32_t>(arrlenu(memoryBarriers));
			dependencyInfo.pMemoryBarriers = memoryBarriers;
			dependencyInfo.bufferMemoryBarrierCount = static_cast<uint32_t>(arrlenu(bufferBarriers));
			dependencyInfo.pBufferMemoryBarriers = bufferBarriers;
			dependencyInfo.imageMemoryBarrierCount = static_cast<uint32_t>(arrlenu(imageBarriers));
			dependencyInfo.pImageMemoryBarriers = imageBarriers;

			vkCmdPipelineBarrier2(commandlist.GetCommandBuffer(), &dependencyInfo);

			arrsetlen(memoryBarriers, 0);
			arrsetlen(imageBarriers, 0);
			arrsetlen(bufferBarriers, 0);
		}
	}
	void GraphicsDevice_Vulkan::BuildRaytracingAccelerationStructure(const RaytracingAccelerationStructure* dst, CommandList cmd, const RaytracingAccelerationStructure* src)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		auto dst_internal = to_internal(dst);

		VkAccelerationStructureBuildGeometryInfoKHR info = dst_internal->buildInfo;
		info.dstAccelerationStructure = dst_internal->resource;
		info.srcAccelerationStructure = VK_NULL_HANDLE;
		info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

		info.scratchData.deviceAddress = dst_internal->scratch_address;

		if (src != nullptr && (dst->desc.flags & RaytracingAccelerationStructureDesc::FLAG_ALLOW_UPDATE))
		{
			info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;

			auto src_internal = to_internal(src);
			info.srcAccelerationStructure = src_internal->resource;
		}

		arrsetlen(commandlist.accelerationstructure_build_geometries, arrlenu(dst_internal->geometries));
		if (arrlenu(commandlist.accelerationstructure_build_geometries) > 0)
		{
			std::memcpy(
				commandlist.accelerationstructure_build_geometries,
				dst_internal->geometries,
				arrlenu(commandlist.accelerationstructure_build_geometries) * sizeof(VkAccelerationStructureGeometryKHR)
			);
		}
		arrsetlen(commandlist.accelerationstructure_build_ranges, 0);

		info.type = dst_internal->createInfo.type;
		info.geometryCount = static_cast<uint32_t>(arrlenu(commandlist.accelerationstructure_build_geometries));

		switch (dst->desc.type)
		{
		case RaytracingAccelerationStructureDesc::Type::BOTTOMLEVEL:
		{
				size_t i = 0;
				for (size_t geometry_index = 0; geometry_index < arrlenu(dst->desc.bottom_level.geometries); ++geometry_index)
				{
					auto& x = dst->desc.bottom_level.geometries[geometry_index];
					auto& geometry = commandlist.accelerationstructure_build_geometries[i];

				arrput(commandlist.accelerationstructure_build_ranges, {});
				auto& range = commandlist.accelerationstructure_build_ranges[arrlenu(commandlist.accelerationstructure_build_ranges) - 1];
				range = {};

				if (x.flags & RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_OPAQUE)
				{
					geometry.flags |= VK_GEOMETRY_OPAQUE_BIT_KHR;
				}
				if (x.flags & RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_NO_DUPLICATE_ANYHIT_INVOCATION)
				{
					geometry.flags |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
				}

				if (x.type == RaytracingAccelerationStructureDesc::BottomLevel::Geometry::Type::TRIANGLES)
				{
					geometry.geometry.triangles.vertexData.deviceAddress = to_internal(&x.triangles.vertex_buffer)->address +
						x.triangles.vertex_byte_offset;

					geometry.geometry.triangles.indexData.deviceAddress = to_internal(&x.triangles.index_buffer)->address +
						x.triangles.index_offset * (x.triangles.index_format == IndexBufferFormat::UINT16 ? sizeof(uint16_t) : sizeof(uint32_t));

					if (x.flags & RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_USE_TRANSFORM)
					{
						geometry.geometry.triangles.transformData.deviceAddress = to_internal(&x.triangles.transform_3x4_buffer)->address;
						range.transformOffset = x.triangles.transform_3x4_buffer_offset;
					}

					range.primitiveCount = x.triangles.index_count / 3;
					range.primitiveOffset = 0;
				}
				else if (x.type == RaytracingAccelerationStructureDesc::BottomLevel::Geometry::Type::PROCEDURAL_AABBS)
				{
					geometry.geometry.aabbs.data.deviceAddress = to_internal(&x.aabbs.aabb_buffer)->address;

					range.primitiveCount = x.aabbs.count;
					range.primitiveOffset = x.aabbs.offset;
				}

				i++;
			}
		}
		break;
		case RaytracingAccelerationStructureDesc::Type::TOPLEVEL:
		{
			auto& geometry = commandlist.accelerationstructure_build_geometries[arrlenu(commandlist.accelerationstructure_build_geometries) - 1];
			geometry.geometry.instances.data.deviceAddress = to_internal(&dst->desc.top_level.instance_buffer)->address;

			arrput(commandlist.accelerationstructure_build_ranges, {});
			auto& range = commandlist.accelerationstructure_build_ranges[arrlenu(commandlist.accelerationstructure_build_ranges) - 1];
			range = {};
			range.primitiveCount = dst->desc.top_level.count;
			range.primitiveOffset = dst->desc.top_level.offset;
		}
		break;
		}

		info.pGeometries = commandlist.accelerationstructure_build_geometries;

		VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = commandlist.accelerationstructure_build_ranges;

		vkCmdBuildAccelerationStructuresKHR(
			commandlist.GetCommandBuffer(),
			1,
			&info,
			&pRangeInfo
		);
	}
	void GraphicsDevice_Vulkan::BindRaytracingPipelineState(const RaytracingPipelineState* rtpso, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		commandlist.prev_pipeline_hash = {};

		if (commandlist.active_rt == nullptr)
		{
			commandlist.binder.dirty |= DescriptorBinder::DIRTY_SET_OR_OFFSET;
		}

		commandlist.active_rt = rtpso;

		SDL_assert(arrlenu(rtpso->desc.shader_libraries) > 0);
		BindComputeShader(rtpso->desc.shader_libraries[0].shader, cmd);

		vkCmdBindPipeline(commandlist.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, to_internal(rtpso)->pipeline);
	}
	void GraphicsDevice_Vulkan::DispatchRays(const DispatchRaysDesc* desc, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		predispatch(cmd);

		VkStridedDeviceAddressRegionKHR raygen = {};
		raygen.deviceAddress = desc->ray_generation.buffer ? to_internal(desc->ray_generation.buffer)->address : 0;
		raygen.deviceAddress += desc->ray_generation.offset;
		raygen.size = desc->ray_generation.size;
		raygen.stride = raygen.size; // raygen specifically must be size == stride

		VkStridedDeviceAddressRegionKHR miss = {};
		miss.deviceAddress = desc->miss.buffer ? to_internal(desc->miss.buffer)->address : 0;
		miss.deviceAddress += desc->miss.offset;
		miss.size = desc->miss.size;
		miss.stride = desc->miss.stride;

		VkStridedDeviceAddressRegionKHR hitgroup = {};
		hitgroup.deviceAddress = desc->hit_group.buffer ? to_internal(desc->hit_group.buffer)->address : 0;
		hitgroup.deviceAddress += desc->hit_group.offset;
		hitgroup.size = desc->hit_group.size;
		hitgroup.stride = desc->hit_group.stride;

		VkStridedDeviceAddressRegionKHR callable = {};
		callable.deviceAddress = desc->callable.buffer ? to_internal(desc->callable.buffer)->address : 0;
		callable.deviceAddress += desc->callable.offset;
		callable.size = desc->callable.size;
		callable.stride = desc->callable.stride;

		vkCmdTraceRaysKHR(
			commandlist.GetCommandBuffer(),
			&raygen,
			&miss,
			&hitgroup,
			&callable,
			desc->width,
			desc->height,
			desc->depth
		);
	}
	void GraphicsDevice_Vulkan::PushConstants(const void* data, uint32_t size, CommandList cmd, uint32_t offset)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		SDL_assert(commandlist.layout.pipeline_layout != VK_NULL_HANDLE); // No pipeline was set!

		vkCmdPushConstants(
			commandlist.GetCommandBuffer(),
			commandlist.layout.pipeline_layout,
			VK_SHADER_STAGE_ALL,
			offset,
			size,
			data
		);
	}
	void GraphicsDevice_Vulkan::PredicationBegin(const GPUBuffer* buffer, uint64_t offset, PredicationOp op, CommandList cmd)
	{
		if (CheckCapability(GraphicsDeviceCapability::GRAPHICS_DEVICE_CAPABILITY_PREDICATION))
		{
			CommandList_Vulkan& commandlist = GetCommandList(cmd);
			auto internal_state = to_internal(buffer);

			VkConditionalRenderingBeginInfoEXT info = {};
			info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
			if (op == PredicationOp::NOT_EQUAL_ZERO)
			{
				info.flags = VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT;
			}
			info.offset = offset;
			info.buffer = internal_state->resource;
			vkCmdBeginConditionalRenderingEXT(commandlist.GetCommandBuffer(), &info);
		}
	}
	void GraphicsDevice_Vulkan::PredicationEnd(CommandList cmd)
	{
		if (CheckCapability(GraphicsDeviceCapability::GRAPHICS_DEVICE_CAPABILITY_PREDICATION))
		{
			CommandList_Vulkan& commandlist = GetCommandList(cmd);
			vkCmdEndConditionalRenderingEXT(commandlist.GetCommandBuffer());
		}
	}
	void GraphicsDevice_Vulkan::ClearUAV(const GPUResource* resource, uint32_t value, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);

		if (wiGraphicsGPUResourceIsBuffer(resource))
		{
			auto internal_state = to_internal<GPUBuffer>(resource);
			vkCmdFillBuffer(
				commandlist.GetCommandBuffer(),
				internal_state->resource,
				0,
				VK_WHOLE_SIZE,
				value
			);
		}
		else if (wiGraphicsGPUResourceIsTexture(resource))
		{
			VkClearColorValue color = {};
			color.uint32[0] = value;
			color.uint32[1] = value;
			color.uint32[2] = value;
			color.uint32[3] = value;

			VkImageSubresourceRange range = {};
			range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			range.baseArrayLayer = 0;
			range.baseMipLevel = 0;
			range.layerCount = VK_REMAINING_ARRAY_LAYERS;
			range.levelCount = VK_REMAINING_MIP_LEVELS;

			auto internal_state = to_internal<Texture>(resource);
			vkCmdClearColorImage(
				commandlist.GetCommandBuffer(),
				internal_state->resource,
				VK_IMAGE_LAYOUT_GENERAL, // "ClearUAV" so must be in UNORDERED_ACCESS state, that's a given
				&color,
				1,
				&range
			);
		}

	}
	void GraphicsDevice_Vulkan::VideoDecode(const VideoDecoder* video_decoder, const VideoDecodeOperation* op, CommandList cmd)
	{
		CommandList_Vulkan& commandlist = GetCommandList(cmd);
		auto decoder_internal = to_internal(video_decoder);
		auto stream_internal = to_internal(op->stream);
		auto dpb_internal = to_internal(op->DPB);

		if (video_decoder->desc.profile == VideoProfile::H264)
		{
			const h264::SliceHeader* slice_header = (const h264::SliceHeader*)op->slice_header;
			const h264::PPS* pps = (const h264::PPS*)op->pps;
			const h264::SPS* sps = (const h264::SPS*)op->sps;

			StdVideoDecodeH264PictureInfo std_picture_info_h264 = {};
			std_picture_info_h264.pic_parameter_set_id = slice_header->pic_parameter_set_id;
			std_picture_info_h264.seq_parameter_set_id = pps->seq_parameter_set_id;
			std_picture_info_h264.frame_num = slice_header->frame_num;
			std_picture_info_h264.PicOrderCnt[0] = op->poc[0];
			std_picture_info_h264.PicOrderCnt[1] = op->poc[1];
			std_picture_info_h264.idr_pic_id = slice_header->idr_pic_id;
			std_picture_info_h264.flags.is_intra = op->frame_type == VideoFrameType::Intra ? 1 : 0;
			std_picture_info_h264.flags.is_reference = op->reference_priority > 0 ? 1 : 0;
			std_picture_info_h264.flags.IdrPicFlag = (std_picture_info_h264.flags.is_intra && std_picture_info_h264.flags.is_reference) ? 1 : 0;
			std_picture_info_h264.flags.field_pic_flag = slice_header->field_pic_flag;
			std_picture_info_h264.flags.bottom_field_flag = slice_header->bottom_field_flag;
			std_picture_info_h264.flags.complementary_field_pair = 0;

			VkVideoReferenceSlotInfoKHR reference_slot_infos[17] = {};
			VkVideoPictureResourceInfoKHR reference_slot_pictures[17] = {};
			VkVideoDecodeH264DpbSlotInfoKHR dpb_slots_h264[17] = {};
			StdVideoDecodeH264ReferenceInfo reference_infos_h264[17] = {};
			for (uint32_t i = 0; i < op->DPB->desc.array_size; ++i)
			{
				VkVideoReferenceSlotInfoKHR& slot = reference_slot_infos[i];
				VkVideoPictureResourceInfoKHR& pic = reference_slot_pictures[i];
				VkVideoDecodeH264DpbSlotInfoKHR& dpb = dpb_slots_h264[i];
				StdVideoDecodeH264ReferenceInfo& ref = reference_infos_h264[i];

				slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
				slot.pPictureResource = &pic;
				slot.slotIndex = i;
				slot.pNext = &dpb;

				pic.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
				pic.codedOffset.x = 0;
				pic.codedOffset.y = 0;
				pic.codedExtent.width = op->DPB->desc.width;
				pic.codedExtent.height = op->DPB->desc.height;
				pic.baseArrayLayer = i;
				pic.imageViewBinding = dpb_internal->video_decode_view;

				dpb.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
				dpb.pStdReferenceInfo = &ref;

				ref.flags.bottom_field_flag = 0;
				ref.flags.top_field_flag = 0;
				ref.flags.is_non_existing = 0;
				ref.flags.used_for_long_term_reference = 0;
				ref.FrameNum = op->dpb_framenum[i];
				ref.PicOrderCnt[0] = op->dpb_poc[i];
				ref.PicOrderCnt[1] = op->dpb_poc[i];
			}

			VkVideoReferenceSlotInfoKHR reference_slots[17] = {};
			for (size_t i = 0; i < op->dpb_reference_count; ++i)
			{
				uint32_t ref_slot = op->dpb_reference_slots[i];
				SDL_assert(ref_slot != op->current_dpb);
				reference_slots[i] = reference_slot_infos[ref_slot];
			}
			reference_slots[op->dpb_reference_count] = reference_slot_infos[op->current_dpb];
			reference_slots[op->dpb_reference_count].slotIndex = -1;

			VkVideoBeginCodingInfoKHR begin_info = {};
			begin_info.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
			begin_info.videoSession = decoder_internal->video_session;
			begin_info.videoSessionParameters = decoder_internal->session_parameters;
			begin_info.referenceSlotCount = op->dpb_reference_count + 1; // add in the current reconstructed DPB image
			begin_info.pReferenceSlots = begin_info.referenceSlotCount == 0 ? nullptr : reference_slots;
			vkCmdBeginVideoCodingKHR(commandlist.GetCommandBuffer(), &begin_info);

			if (op->flags & VideoDecodeOperation::FLAG_SESSION_RESET)
			{
				VkVideoCodingControlInfoKHR control_info = {};
				control_info.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
				control_info.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
				vkCmdControlVideoCodingKHR(commandlist.GetCommandBuffer(), &control_info);
			}

			VkVideoDecodeInfoKHR decode_info = {};
			decode_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
			decode_info.srcBuffer = stream_internal->resource;
			decode_info.srcBufferOffset = (VkDeviceSize)op->stream_offset;
			decode_info.srcBufferRange = (VkDeviceSize)align(op->stream_size, VIDEO_DECODE_BITSTREAM_ALIGNMENT);
			if (op->output == nullptr)
			{
				decode_info.dstPictureResource = *reference_slot_infos[op->current_dpb].pPictureResource;
			}
			else
			{
				auto output_internal = to_internal(op->output);
				decode_info.dstPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
				decode_info.dstPictureResource.codedOffset.x = 0;
				decode_info.dstPictureResource.codedOffset.y = 0;
				decode_info.dstPictureResource.codedExtent.width = op->DPB->desc.width;
				decode_info.dstPictureResource.codedExtent.height = op->DPB->desc.height;
				decode_info.dstPictureResource.baseArrayLayer = 0;
				decode_info.dstPictureResource.imageViewBinding = output_internal->video_decode_view;
			}
			decode_info.referenceSlotCount = op->dpb_reference_count;
			decode_info.pReferenceSlots = decode_info.referenceSlotCount == 0 ? nullptr : reference_slots;
			decode_info.pSetupReferenceSlot = &reference_slot_infos[op->current_dpb];

			uint32_t slice_offset = 0;

			// https://vulkan.lunarg.com/doc/view/1.3.239.0/windows/1.3-extensions/vkspec.html#_h_264_decoding_parameters
			VkVideoDecodeH264PictureInfoKHR picture_info_h264 = {};
			picture_info_h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
			picture_info_h264.pStdPictureInfo = &std_picture_info_h264;
			picture_info_h264.sliceCount = 1;
			picture_info_h264.pSliceOffsets = &slice_offset;
			decode_info.pNext = &picture_info_h264;

			vkCmdDecodeVideoKHR(commandlist.GetCommandBuffer(), &decode_info);

			VkVideoEndCodingInfoKHR end_info = {};
			end_info.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
			vkCmdEndVideoCodingKHR(commandlist.GetCommandBuffer(), &end_info);
		}
		else if (video_decoder->desc.profile == VideoProfile::H265)
		{
			SDL_assert(0); // TODO
		}
	}

	void GraphicsDevice_Vulkan::EventBegin(const char* name, CommandList cmd)
	{
		if (!debugUtils)
			return;
		CommandList_Vulkan& commandlist = GetCommandList(cmd);

		VkDebugUtilsLabelEXT label = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
		label.pLabelName = name;
		label.color[0] = 0.0f;
		label.color[1] = 0.0f;
		label.color[2] = 0.0f;
		label.color[3] = 1.0f;
		vkCmdBeginDebugUtilsLabelEXT(commandlist.GetCommandBuffer(), &label);
	}
	void GraphicsDevice_Vulkan::EventEnd(CommandList cmd)
	{
		if (!debugUtils)
			return;
		CommandList_Vulkan& commandlist = GetCommandList(cmd);

		vkCmdEndDebugUtilsLabelEXT(commandlist.GetCommandBuffer());
	}
	void GraphicsDevice_Vulkan::SetMarker(const char* name, CommandList cmd)
	{
		if (!debugUtils)
			return;
		CommandList_Vulkan& commandlist = GetCommandList(cmd);

		VkDebugUtilsLabelEXT label { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
		label.pLabelName = name;
		label.color[0] = 0.0f;
		label.color[1] = 0.0f;
		label.color[2] = 0.0f;
		label.color[3] = 1.0f;
		vkCmdInsertDebugUtilsLabelEXT(commandlist.GetCommandBuffer(), &label);
	}
	VkDevice GraphicsDevice_Vulkan::GetDevice()
	{
		return device;
	}
	VkImage GraphicsDevice_Vulkan::GetTextureInternalResource(const Texture* texture)
	{
		return to_internal(texture)->resource;
	}
	VkPhysicalDevice GraphicsDevice_Vulkan::GetPhysicalDevice()
	{
		return physicalDevice;
	}
	VkInstance GraphicsDevice_Vulkan::GetInstance()
	{
		return instance;
	}
	VkQueue GraphicsDevice_Vulkan::GetGraphicsCommandQueue()
	{
		return queues[QUEUE_GRAPHICS].queue;
	}
	uint32_t GraphicsDevice_Vulkan::GetGraphicsFamilyIndex()
	{
		return graphicsFamily;
	}
}

#endif // WICKEDENGINE_BUILD_VULKAN
