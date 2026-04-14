#ifdef __APPLE__
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define IR_PRIVATE_IMPLEMENTATION
#include "wiGraphicsDevice_Metal.h"

#if __has_include(<SDL3/SDL_stdinc.h>) && __has_include(<SDL3/SDL_timer.h>)
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#endif

#include <Metal/MTL4AccelerationStructure.hpp>
#include <cmath>
#include <memory>

#if defined(WICKED_MMGR_ENABLED)
// MMGR include must come after standard/project includes to avoid macro collisions in system headers.
#include "../forge-mmgr/FluidStudios/MemoryManager/mmgr.h"
#endif

#define METAL_LOG_ERROR(...) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define METAL_LOG(...) SDL_Log(__VA_ARGS__)
#define METAL_ASSERT_MSG(cond, ...) do { if (!(cond)) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__); SDL_assert(cond); } } while (0)

namespace wi
{

	namespace
	{
		static constexpr uint64_t METAL_WAIT_TIMEOUT_MS = 10000;
		static constexpr uint32_t METAL_ICB_BIND_ARGS = 24;
		static constexpr uint32_t METAL_ICB_BIND_COUNT = 25;
		static constexpr uint32_t METAL_ICB_BIND_CONTAINER = 26;
		static constexpr uint32_t METAL_ICB_BIND_PARAMS = 27;
		static constexpr uint32_t METAL_ICB_BIND_INDEXBUFFER = 28;
		static constexpr uint32_t METAL_ICB_BIND_EXEC_RANGE = 29;
		static constexpr uint32_t METAL_ICB_CONTAINER_ID = 0;
		static constexpr uint32_t METAL_ICB_CONTAINER_BUFFER_INDEX = METAL_ICB_BIND_CONTAINER;

		struct MetalICBExecutionRange
		{
			uint32_t location = 0;
			uint32_t length = 0;
		};

		struct MetalICBDrawCountEncodeParams
		{
			uint32_t maxCount = 0;
			uint32_t primitiveType = 0;
			uint32_t indexType = 0;
			uint32_t drawArgsStride = 0;
			uint32_t drawArgsBindIndex = 0;
			uint32_t needsDrawParams = 0;
			uint32_t drawArgsDataOffset = 0;
			uint32_t objectThreadgroupSizeX = 0;
			uint32_t objectThreadgroupSizeY = 0;
			uint32_t objectThreadgroupSizeZ = 0;
			uint32_t meshThreadgroupSizeX = 0;
			uint32_t meshThreadgroupSizeY = 0;
			uint32_t meshThreadgroupSizeZ = 0;
			uint32_t padding = 0;
		};

		static constexpr const char* METAL_ICB_DRAWCOUNT_KERNEL_SOURCE = R"msl(
#include <metal_stdlib>
using namespace metal;

struct IndirectDrawArgsInstanced
{
	uint VertexCountPerInstance;
	uint InstanceCount;
	uint StartVertexLocation;
	uint StartInstanceLocation;
};

struct IndirectDrawArgsIndexedInstanced
{
	uint IndexCountPerInstance;
	uint InstanceCount;
	uint StartIndexLocation;
	int BaseVertexLocation;
	uint StartInstanceLocation;
};

struct IndirectDispatchArgs
{
	uint ThreadGroupCountX;
	uint ThreadGroupCountY;
	uint ThreadGroupCountZ;
};

struct ICBContainer
{
	command_buffer commandBuffer [[id(0)]];
};

struct ICBExecutionRange
{
	uint location;
	uint length;
};

struct DrawCountEncodeParams
{
	uint maxCount;
	uint primitiveType;
	uint indexType;
	uint drawArgsStride;
	uint drawArgsBindIndex;
	uint needsDrawParams;
	uint drawArgsDataOffset;
	uint objectThreadgroupSizeX;
	uint objectThreadgroupSizeY;
	uint objectThreadgroupSizeZ;
	uint meshThreadgroupSizeX;
	uint meshThreadgroupSizeY;
	uint meshThreadgroupSizeZ;
	uint padding;
};

kernel void wicked_encode_draw_indirect_count_icb(
	uint commandIndex [[thread_position_in_grid]],
	device const uchar* argsBytes [[buffer(24)]],
	device const uint* countPtr [[buffer(25)]],
	constant ICBContainer& icbContainer [[buffer(26)]],
	constant DrawCountEncodeParams& params [[buffer(27)]],
	device ICBExecutionRange* executionRangeOut [[buffer(29)]])
{
	if (commandIndex >= params.maxCount)
	{
		return;
	}

	const uint drawCount = min(countPtr[0], params.maxCount);
	if (commandIndex == 0u)
	{
		executionRangeOut[0].location = 0u;
		executionRangeOut[0].length = drawCount;
	}
	render_command cmd(icbContainer.commandBuffer, commandIndex);
	if (commandIndex < drawCount)
	{
		const uint byteOffset = commandIndex * params.drawArgsStride;
		const device IndirectDrawArgsInstanced* drawArgs = (const device IndirectDrawArgsInstanced*)(argsBytes + byteOffset);
		if (params.needsDrawParams != 0u)
		{
			cmd.set_vertex_buffer(drawArgs, params.drawArgsBindIndex);
		}
		cmd.draw_primitives(
			primitive_type(params.primitiveType),
			drawArgs->StartVertexLocation,
			drawArgs->VertexCountPerInstance,
			drawArgs->InstanceCount,
			drawArgs->StartInstanceLocation);
	}
	else
	{
		cmd.reset();
	}
}

kernel void wicked_encode_draw_indexed_indirect_count_icb(
	uint commandIndex [[thread_position_in_grid]],
	device const uchar* argsBytes [[buffer(24)]],
	device const uint* countPtr [[buffer(25)]],
	constant ICBContainer& icbContainer [[buffer(26)]],
	constant DrawCountEncodeParams& params [[buffer(27)]],
	device const uchar* indexBytes [[buffer(28)]],
	device ICBExecutionRange* executionRangeOut [[buffer(29)]])
{
	if (commandIndex >= params.maxCount)
	{
		return;
	}

	const uint drawCount = min(countPtr[0], params.maxCount);
	if (commandIndex == 0u)
	{
		executionRangeOut[0].location = 0u;
		executionRangeOut[0].length = drawCount;
	}
	render_command cmd(icbContainer.commandBuffer, commandIndex);
	if (commandIndex < drawCount)
	{
		const uint byteOffset = commandIndex * params.drawArgsStride;
		const device IndirectDrawArgsIndexedInstanced* drawArgs = (const device IndirectDrawArgsIndexedInstanced*)(argsBytes + byteOffset);
		if (params.needsDrawParams != 0u)
		{
			cmd.set_vertex_buffer(drawArgs, params.drawArgsBindIndex);
		}
		if (params.indexType == 0u)
		{
			const device ushort* index16 = (const device ushort*)indexBytes;
			cmd.draw_indexed_primitives(
				primitive_type(params.primitiveType),
				drawArgs->IndexCountPerInstance,
				index16 + drawArgs->StartIndexLocation,
				drawArgs->InstanceCount,
				drawArgs->BaseVertexLocation,
				drawArgs->StartInstanceLocation);
		}
		else
		{
			const device uint* index32 = (const device uint*)indexBytes;
			cmd.draw_indexed_primitives(
				primitive_type(params.primitiveType),
				drawArgs->IndexCountPerInstance,
				index32 + drawArgs->StartIndexLocation,
				drawArgs->InstanceCount,
				drawArgs->BaseVertexLocation,
				drawArgs->StartInstanceLocation);
		}
	}
	else
	{
		cmd.reset();
	}
}

kernel void wicked_encode_draw_mesh_indirect_count_icb(
	uint commandIndex [[thread_position_in_grid]],
	device const uchar* argsBytes [[buffer(24)]],
	device const uint* countPtr [[buffer(25)]],
	constant ICBContainer& icbContainer [[buffer(26)]],
	constant DrawCountEncodeParams& params [[buffer(27)]],
	device ICBExecutionRange* executionRangeOut [[buffer(29)]])
{
	if (commandIndex >= params.maxCount)
	{
		return;
	}

	const uint drawCount = min(countPtr[0], params.maxCount);
	if (commandIndex == 0u)
	{
		executionRangeOut[0].location = 0u;
		executionRangeOut[0].length = drawCount;
	}
	render_command cmd(icbContainer.commandBuffer, commandIndex);
	if (commandIndex < drawCount)
	{
		const uint byteOffset = commandIndex * params.drawArgsStride + params.drawArgsDataOffset;
		const device IndirectDispatchArgs* drawArgs = (const device IndirectDispatchArgs*)(argsBytes + byteOffset);
		cmd.draw_mesh_threadgroups(
			uint3(drawArgs->ThreadGroupCountX, drawArgs->ThreadGroupCountY, drawArgs->ThreadGroupCountZ),
			uint3(params.objectThreadgroupSizeX, params.objectThreadgroupSizeY, params.objectThreadgroupSizeZ),
			uint3(params.meshThreadgroupSizeX, params.meshThreadgroupSizeY, params.meshThreadgroupSizeZ));
	}
	else
	{
		cmd.reset();
	}
}
)msl";

#if !defined(SDL_clamp)
#define SDL_clamp(x, a, b) (((x) < (a)) ? (a) : (((x) > (b)) ? (b) : (x)))
#endif

	static double GetElapsedMilliseconds(Uint64 begin_counter)
	{
		const Uint64 end_counter = SDL_GetPerformanceCounter();
		const Uint64 frequency = SDL_GetPerformanceFrequency();
		if (frequency == 0)
		{
			return 0.0;
		}
		return (double)(end_counter - begin_counter) * 1000.0 / (double)frequency;
	}

	static bool WaitForSharedEventWithAssert(MTL::SharedEvent* event, uint64_t value, const char* context)
	{
		SDL_assert(event != nullptr);
		if (event == nullptr)
			return false;

		const bool signaled = event->waitUntilSignaledValue(value, METAL_WAIT_TIMEOUT_MS);
		if (!signaled)
		{
			SDL_LogError(
				SDL_LOG_CATEGORY_APPLICATION,
				"[Wicked::Metal] wait timeout in %s (target=%llu current=%llu timeout_ms=%llu)",
				context != nullptr ? context : "unknown",
				(unsigned long long)value,
				(unsigned long long)event->signaledValue(),
				(unsigned long long)METAL_WAIT_TIMEOUT_MS);
			SDL_assert(false && "Metal shared event wait timeout");
		}
		return signaled;
	}

#if defined(WICKED_MMGR_ENABLED) && WI_ENGINECONFIG_MMGR_METAL_TEST_LEAK
	// Intentional MMGR leak for validation. This is purposely never freed.
	static void* g_metal_mmgr_intentional_leak = nullptr;
#endif
}

namespace metal_internal
{
	static constexpr MTL::SparsePageSize sparse_page_size = MTL::SparsePageSize256;

	constexpr MTL::AttributeFormat _ConvertAttributeFormat(Format value)
	{
		switch (value) {
			case Format::R32G32B32A32_FLOAT:
				return MTL::AttributeFormatFloat4;
			case Format::R32G32B32_FLOAT:
				return MTL::AttributeFormatFloat3;
			case Format::R32G32_FLOAT:
				return MTL::AttributeFormatFloat2;
			case Format::R32_FLOAT:
				return MTL::AttributeFormatFloat;
				
			case Format::R16G16B16A16_FLOAT:
				return MTL::AttributeFormatHalf4;
			case Format::R16G16_FLOAT:
				return MTL::AttributeFormatHalf2;
			case Format::R16_FLOAT:
				return MTL::AttributeFormatHalf;
				
			case Format::R32G32B32A32_UINT:
				return MTL::AttributeFormatUInt4;
			case Format::R32G32B32_UINT:
				return MTL::AttributeFormatUInt3;
			case Format::R32G32_UINT:
				return MTL::AttributeFormatUInt2;
			case Format::R32_UINT:
				return MTL::AttributeFormatUInt;
				
			case Format::R32G32B32A32_SINT:
				return MTL::AttributeFormatInt4;
			case Format::R32G32B32_SINT:
				return MTL::AttributeFormatInt3;
			case Format::R32G32_SINT:
				return MTL::AttributeFormatInt2;
			case Format::R32_SINT:
				return MTL::AttributeFormatInt;
				
			case Format::R16G16B16A16_UINT:
				return MTL::AttributeFormatUShort4;
			case Format::R16G16_UINT:
				return MTL::AttributeFormatUShort2;
			case Format::R16_UINT:
				return MTL::AttributeFormatUShort;
				
			case Format::R16G16B16A16_SINT:
				return MTL::AttributeFormatShort4;
			case Format::R16G16_SINT:
				return MTL::AttributeFormatShort2;
			case Format::R16_SINT:
				return MTL::AttributeFormatShort;
				
			case Format::R16G16B16A16_UNORM:
				return MTL::AttributeFormatUShort4Normalized;
			case Format::R16G16_UNORM:
				return MTL::AttributeFormatUShort2Normalized;
			case Format::R16_UNORM:
				return MTL::AttributeFormatUShortNormalized;
				
			case Format::R16G16B16A16_SNORM:
				return MTL::AttributeFormatShort4Normalized;
			case Format::R16G16_SNORM:
				return MTL::AttributeFormatShort2Normalized;
			case Format::R16_SNORM:
				return MTL::AttributeFormatShortNormalized;
				
			case Format::R8G8B8A8_UINT:
				return MTL::AttributeFormatUChar4;
			case Format::R8G8_UINT:
				return MTL::AttributeFormatUChar2;
			case Format::R8_UINT:
				return MTL::AttributeFormatUChar;
				
			case Format::B8G8R8A8_UNORM:
				return MTL::AttributeFormatUChar4Normalized_BGRA;
			case Format::R8G8B8A8_UNORM:
				return MTL::AttributeFormatUChar4Normalized;
			case Format::R8G8_UNORM:
				return MTL::AttributeFormatUChar2Normalized;
			case Format::R8_UNORM:
				return MTL::AttributeFormatUCharNormalized;
				
			case Format::R8G8B8A8_SNORM:
				return MTL::AttributeFormatChar4Normalized;
			case Format::R8G8_SNORM:
				return MTL::AttributeFormatChar2Normalized;
			case Format::R8_SNORM:
				return MTL::AttributeFormatCharNormalized;
				
			case Format::R10G10B10A2_UNORM:
				return MTL::AttributeFormatUInt1010102Normalized;
			case Format::R11G11B10_FLOAT:
				return MTL::AttributeFormatFloatRG11B10;
			case Format::R9G9B9E5_SHAREDEXP:
				return MTL::AttributeFormatFloatRGB9E5;
				
			default:
				break;
		}
		return MTL::AttributeFormatInvalid;
	}
	constexpr MTL::VertexFormat _ConvertVertexFormat(Format value)
	{
		switch (value)
		{
		default:
		case Format::UNKNOWN:
			return MTL::VertexFormatInvalid;
		case Format::R32G32B32A32_FLOAT:
			return MTL::VertexFormatFloat4;
		case Format::R32G32B32A32_UINT:
			return MTL::VertexFormatUInt4;
		case Format::R32G32B32A32_SINT:
			return MTL::VertexFormatInt4;
		case Format::R32G32B32_FLOAT:
			return MTL::VertexFormatFloat3;
		case Format::R32G32B32_UINT:
			return MTL::VertexFormatUInt3;
		case Format::R32G32B32_SINT:
			return MTL::VertexFormatInt3;
		case Format::R16G16B16A16_FLOAT:
			return MTL::VertexFormatHalf4;
		case Format::R16G16B16A16_UNORM:
			return MTL::VertexFormatUShort4Normalized;
		case Format::R16G16B16A16_UINT:
			return MTL::VertexFormatUShort4;
		case Format::R16G16B16A16_SNORM:
			return MTL::VertexFormatShort4Normalized;
		case Format::R16G16B16A16_SINT:
			return MTL::VertexFormatShort4;
		case Format::R32G32_FLOAT:
			return MTL::VertexFormatFloat2;
		case Format::R32G32_UINT:
			return MTL::VertexFormatUInt2;
		case Format::R32G32_SINT:
			return MTL::VertexFormatInt2;
		case Format::D32_FLOAT_S8X24_UINT:
			return MTL::VertexFormatInvalid;
		case Format::R10G10B10A2_UNORM:
			return MTL::VertexFormatUInt1010102Normalized;
		case Format::R10G10B10A2_UINT:
			return MTL::VertexFormatInvalid;
		case Format::R11G11B10_FLOAT:
			return MTL::VertexFormatFloatRG11B10;
		case Format::R8G8B8A8_UNORM:
			return MTL::VertexFormatUChar4Normalized;
		case Format::R8G8B8A8_UNORM_SRGB:
			return MTL::VertexFormatInvalid;
		case Format::R8G8B8A8_UINT:
			return MTL::VertexFormatUChar4;
		case Format::R8G8B8A8_SNORM:
			return MTL::VertexFormatChar4Normalized;
		case Format::R8G8B8A8_SINT:
			return MTL::VertexFormatChar4;
		case Format::R16G16_FLOAT:
			return MTL::VertexFormatHalf2;
		case Format::R16G16_UNORM:
			return MTL::VertexFormatUShort2Normalized;
		case Format::R16G16_UINT:
			return MTL::VertexFormatUShort2;
		case Format::R16G16_SNORM:
			return MTL::VertexFormatShort2Normalized;
		case Format::R16G16_SINT:
			return MTL::VertexFormatShort2;
		case Format::D32_FLOAT:
			return MTL::VertexFormatInvalid;
		case Format::R32_FLOAT:
			return MTL::VertexFormatFloat;
		case Format::R32_UINT:
			return MTL::VertexFormatUInt;
		case Format::R32_SINT:
			return MTL::VertexFormatInt;
		case Format::D24_UNORM_S8_UINT:
			return MTL::VertexFormatInvalid;
		case Format::R9G9B9E5_SHAREDEXP:
			return MTL::VertexFormatFloatRGB9E5;
		case Format::R8G8_UNORM:
			return MTL::VertexFormatUChar2Normalized;
		case Format::R8G8_UINT:
			return MTL::VertexFormatUChar2;
		case Format::R8G8_SNORM:
			return MTL::VertexFormatChar2Normalized;
		case Format::R8G8_SINT:
			return MTL::VertexFormatChar2;
		case Format::R16_FLOAT:
			return MTL::VertexFormatHalf;
		case Format::D16_UNORM:
			return MTL::VertexFormatInvalid;
		case Format::R16_UNORM:
			return MTL::VertexFormatUShortNormalized;
		case Format::R16_UINT:
			return MTL::VertexFormatUShort;
		case Format::R16_SNORM:
			return MTL::VertexFormatShortNormalized;
		case Format::R16_SINT:
			return MTL::VertexFormatShort;
		case Format::R8_UNORM:
			return MTL::VertexFormatUCharNormalized;
		case Format::R8_UINT:
			return MTL::VertexFormatUChar;
		case Format::R8_SNORM:
			return MTL::VertexFormatCharNormalized;
		case Format::R8_SINT:
			return MTL::VertexFormatChar;
		}
		return MTL::VertexFormatInvalid;
	}
	constexpr MTL::PixelFormat _ConvertPixelFormat(Format value)
	{
	   switch (value)
	   {
	   case Format::UNKNOWN:
		   return MTL::PixelFormatInvalid;
	   case Format::R32G32B32A32_FLOAT:
		   return MTL::PixelFormatRGBA32Float;
	   case Format::R32G32B32A32_UINT:
		   return MTL::PixelFormatRGBA32Uint;
	   case Format::R32G32B32A32_SINT:
		   return MTL::PixelFormatRGBA32Sint;
	   case Format::R32G32B32_FLOAT:
		   return MTL::PixelFormatInvalid;
	   case Format::R32G32B32_UINT:
		   return MTL::PixelFormatInvalid;
	   case Format::R32G32B32_SINT:
		   return MTL::PixelFormatInvalid;
	   case Format::R16G16B16A16_FLOAT:
		   return MTL::PixelFormatRGBA16Float;
	   case Format::R16G16B16A16_UNORM:
		   return MTL::PixelFormatRGBA16Unorm;
	   case Format::R16G16B16A16_UINT:
		   return MTL::PixelFormatRGBA16Uint;
	   case Format::R16G16B16A16_SNORM:
		   return MTL::PixelFormatRGBA16Snorm;
	   case Format::R16G16B16A16_SINT:
		   return MTL::PixelFormatRGBA16Sint;
	   case Format::R32G32_FLOAT:
		   return MTL::PixelFormatRG32Float;
	   case Format::R32G32_UINT:
		   return MTL::PixelFormatRG32Uint;
	   case Format::R32G32_SINT:
		   return MTL::PixelFormatRG32Sint;
	   case Format::D32_FLOAT_S8X24_UINT:
		   return MTL::PixelFormatDepth32Float_Stencil8;
	   case Format::R10G10B10A2_UNORM:
		   return MTL::PixelFormatBGR10A2Unorm;
	   case Format::R10G10B10A2_UINT:
		   return MTL::PixelFormatInvalid;
	   case Format::R11G11B10_FLOAT:
		   return MTL::PixelFormatRG11B10Float;
	   case Format::R8G8B8A8_UNORM:
		   return MTL::PixelFormatRGBA8Unorm;
	   case Format::R8G8B8A8_UNORM_SRGB:
		   return MTL::PixelFormatRGBA8Unorm_sRGB;
	   case Format::R8G8B8A8_UINT:
		   return MTL::PixelFormatRGBA8Uint;
	   case Format::R8G8B8A8_SNORM:
		   return MTL::PixelFormatRGBA8Snorm;
	   case Format::R8G8B8A8_SINT:
		   return MTL::PixelFormatRGBA8Sint;
	   case Format::R16G16_FLOAT:
		   return MTL::PixelFormatRG16Float;
	   case Format::R16G16_UNORM:
		   return MTL::PixelFormatRG16Unorm;
	   case Format::R16G16_UINT:
		   return MTL::PixelFormatRG16Uint;
	   case Format::R16G16_SNORM:
		   return MTL::PixelFormatRG16Snorm;
	   case Format::R16G16_SINT:
		   return MTL::PixelFormatRG16Sint;
	   case Format::D32_FLOAT:
		   return MTL::PixelFormatDepth32Float;
	   case Format::R32_FLOAT:
		   return MTL::PixelFormatR32Float;
	   case Format::R32_UINT:
		   return MTL::PixelFormatR32Uint;
	   case Format::R32_SINT:
		   return MTL::PixelFormatR32Sint;
	   case Format::D24_UNORM_S8_UINT:
		   return MTL::PixelFormatDepth24Unorm_Stencil8;
	   case Format::R9G9B9E5_SHAREDEXP:
		   return MTL::PixelFormatRGB9E5Float;
	   case Format::R8G8_UNORM:
		   return MTL::PixelFormatRG8Unorm;
	   case Format::R8G8_UINT:
		   return MTL::PixelFormatRG8Uint;
	   case Format::R8G8_SNORM:
		   return MTL::PixelFormatRG8Snorm;
	   case Format::R8G8_SINT:
		   return MTL::PixelFormatRG8Sint;
	   case Format::R16_FLOAT:
		   return MTL::PixelFormatR16Float;
	   case Format::D16_UNORM:
		   return MTL::PixelFormatDepth16Unorm;
	   case Format::R16_UNORM:
		   return MTL::PixelFormatR16Unorm;
	   case Format::R16_UINT:
		   return MTL::PixelFormatR16Uint;
	   case Format::R16_SNORM:
		   return MTL::PixelFormatR16Snorm;
	   case Format::R16_SINT:
		   return MTL::PixelFormatR16Sint;
	   case Format::R8_UNORM:
		   return MTL::PixelFormatR8Unorm;
	   case Format::R8_UINT:
		   return MTL::PixelFormatR8Uint;
	   case Format::R8_SNORM:
		   return MTL::PixelFormatR8Snorm;
	   case Format::R8_SINT:
		   return MTL::PixelFormatR8Sint;
	   case Format::BC1_UNORM:
		   return MTL::PixelFormatBC1_RGBA;
	   case Format::BC1_UNORM_SRGB:
		   return MTL::PixelFormatBC1_RGBA_sRGB;
	   case Format::BC2_UNORM:
		   return MTL::PixelFormatBC2_RGBA;
	   case Format::BC2_UNORM_SRGB:
		   return MTL::PixelFormatBC2_RGBA_sRGB;
	   case Format::BC3_UNORM:
		   return MTL::PixelFormatBC3_RGBA;
	   case Format::BC3_UNORM_SRGB:
		   return MTL::PixelFormatBC3_RGBA_sRGB;
	   case Format::BC4_UNORM:
		   return MTL::PixelFormatBC4_RUnorm;
	   case Format::BC4_SNORM:
		   return MTL::PixelFormatBC4_RSnorm;
	   case Format::BC5_UNORM:
		   return MTL::PixelFormatBC5_RGUnorm;
	   case Format::BC5_SNORM:
		   return MTL::PixelFormatBC5_RGSnorm;
	   case Format::B8G8R8A8_UNORM:
		   return MTL::PixelFormatBGRA8Unorm;
	   case Format::B8G8R8A8_UNORM_SRGB:
		   return MTL::PixelFormatBGRA8Unorm_sRGB;
	   case Format::BC6H_UF16:
		   return MTL::PixelFormatBC6H_RGBUfloat;
	   case Format::BC6H_SF16:
		   return MTL::PixelFormatBC6H_RGBFloat;
	   case Format::BC7_UNORM:
		   return MTL::PixelFormatBC7_RGBAUnorm;
	   case Format::BC7_UNORM_SRGB:
		   return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
	   case Format::NV12:
		   return MTL::PixelFormatInvalid;
	   }
	   return MTL::PixelFormatInvalid;
	}
	constexpr MTL::BlendOperation _ConvertBlendOp(BlendOp value)
	{
		switch (value)
		{
			case BlendOp::ADD:
				return MTL::BlendOperationAdd;
			case BlendOp::SUBTRACT:
				return MTL::BlendOperationSubtract;
			case BlendOp::REV_SUBTRACT:
				return MTL::BlendOperationReverseSubtract;
			case BlendOp::MIN:
				return MTL::BlendOperationMin;
			case BlendOp::MAX:
				return MTL::BlendOperationMax;
		}
		return MTL::BlendOperationUnspecialized;
	}
	constexpr MTL::BlendFactor _ConvertBlendFactor(Blend value)
	{
		switch (value)
		{
			case Blend::ZERO:
				return MTL::BlendFactorZero;
			case Blend::ONE:
				return MTL::BlendFactorOne;
			case Blend::BLEND_FACTOR:
				return MTL::BlendFactorBlendColor;
			case Blend::INV_BLEND_FACTOR:
				return MTL::BlendFactorOneMinusBlendColor;
			case Blend::DEST_ALPHA:
				return MTL::BlendFactorDestinationAlpha;
			case Blend::DEST_COLOR:
				return MTL::BlendFactorDestinationColor;
			case Blend::INV_DEST_ALPHA:
				return MTL::BlendFactorOneMinusDestinationAlpha;
			case Blend::INV_DEST_COLOR:
				return MTL::BlendFactorOneMinusDestinationColor;
			case Blend::SRC_ALPHA:
				return MTL::BlendFactorSourceAlpha;
			case Blend::SRC_COLOR:
				return MTL::BlendFactorSourceColor;
			case Blend::INV_SRC_ALPHA:
				return MTL::BlendFactorOneMinusSourceAlpha;
			case Blend::INV_SRC_COLOR:
				return MTL::BlendFactorOneMinusSourceColor;
			case Blend::SRC1_ALPHA:
				return MTL::BlendFactorSource1Alpha;
			case Blend::SRC1_COLOR:
				return MTL::BlendFactorSource1Color;
			case Blend::INV_SRC1_ALPHA:
				return MTL::BlendFactorOneMinusSource1Alpha;
			case Blend::INV_SRC1_COLOR:
				return MTL::BlendFactorOneMinusSource1Color;
			case Blend::SRC_ALPHA_SAT:
				return MTL::BlendFactorSourceAlphaSaturated;
		}
		return MTL::BlendFactorUnspecialized;
	}
	constexpr MTL::CompareFunction _ConvertCompareFunction(ComparisonFunc value)
	{
		switch (value)
		{
			case ComparisonFunc::ALWAYS:
				return MTL::CompareFunctionAlways;
			case ComparisonFunc::NEVER:
				return MTL::CompareFunctionNever;
			case ComparisonFunc::EQUAL:
				return MTL::CompareFunctionEqual;
			case ComparisonFunc::NOT_EQUAL:
				return MTL::CompareFunctionNotEqual;
			case ComparisonFunc::LESS:
				return MTL::CompareFunctionLess;
			case ComparisonFunc::LESS_EQUAL:
				return MTL::CompareFunctionLessEqual;
			case ComparisonFunc::GREATER:
				return MTL::CompareFunctionGreater;
			case ComparisonFunc::GREATER_EQUAL:
				return MTL::CompareFunctionGreaterEqual;
		}
		return MTL::CompareFunctionNever;
	}
	constexpr MTL::StencilOperation _ConvertStencilOperation(StencilOp value)
	{
		switch (value)
		{
			case StencilOp::KEEP:
				return MTL::StencilOperationKeep;
			case StencilOp::REPLACE:
				return MTL::StencilOperationReplace;
			case StencilOp::STENCIL_ZERO:
				return MTL::StencilOperationZero;
			case StencilOp::INVERT:
				return MTL::StencilOperationInvert;
			case StencilOp::INCR:
				return MTL::StencilOperationIncrementWrap;
			case StencilOp::INCR_SAT:
				return MTL::StencilOperationIncrementClamp;
			case StencilOp::DECR:
				return MTL::StencilOperationDecrementWrap;
			case StencilOp::DECR_SAT:
				return MTL::StencilOperationDecrementClamp;
		}
		return MTL::StencilOperationKeep;
	}
	constexpr MTL::TextureSwizzle _ConvertComponentSwizzle(ComponentSwizzle value)
	{
		switch (value)
		{
			case ComponentSwizzle::R:
				return MTL::TextureSwizzleRed;
			case ComponentSwizzle::G:
				return MTL::TextureSwizzleGreen;
			case ComponentSwizzle::B:
				return MTL::TextureSwizzleBlue;
			case ComponentSwizzle::A:
				return MTL::TextureSwizzleAlpha;
			case ComponentSwizzle::SWIZZLE_ONE:
				return MTL::TextureSwizzleOne;
			case ComponentSwizzle::SWIZZLE_ZERO:
				return MTL::TextureSwizzleZero;
			default:
				break;
		}
		return MTL::TextureSwizzleOne;
	}
	constexpr MTL::SamplerAddressMode _ConvertAddressMode(TextureAddressMode value)
	{
		switch (value) {
			case TextureAddressMode::CLAMP:
				return MTL::SamplerAddressModeClampToEdge;
			case TextureAddressMode::WRAP:
				return MTL::SamplerAddressModeRepeat;
			case TextureAddressMode::MIRROR_ONCE:
				return MTL::SamplerAddressModeMirrorClampToEdge;
			case TextureAddressMode::MIRROR:
				return MTL::SamplerAddressModeMirrorRepeat;
			case TextureAddressMode::BORDER:
				return MTL::SamplerAddressModeClampToBorderColor;
			default:
				break;
		}
		return MTL::SamplerAddressModeClampToEdge;
	}

	IRDescriptorTableEntry create_entry(MTL::Texture* res, float min_lod_clamp = 0, uint32_t metadata = 0)
	{
		IRDescriptorTableEntry entry;
		IRDescriptorTableSetTexture(&entry, res, min_lod_clamp, metadata);
		return entry;
	}
	IRDescriptorTableEntry create_entry(MTL::ResourceID resourceID, float min_lod_clamp = 0, uint32_t metadata = 0)
	{
		// This is the expanded version of IRDescriptorTableSetTexture, made to be compatible with texture view pooling
		IRDescriptorTableEntry entry;
		entry.gpuVA = 0;
		entry.textureViewID = resourceID._impl;
		entry.metadata = (uint64_t)(*((uint32_t*)&min_lod_clamp) | ((uint64_t)metadata) << 32);
		return entry;
	}
	IRDescriptorTableEntry create_entry(MTL::Buffer* res, uint64_t size, uint64_t offset = 0, MTL::Texture* texture_buffer_view = nullptr, Format format = Format::UNKNOWN, bool structured = false)
	{
		IRDescriptorTableEntry entry;
		IRBufferView view = {};
		view.buffer = res;
		view.bufferSize = size;
		view.bufferOffset = offset;
		if (texture_buffer_view == nullptr)
		{
			view.typedBuffer = structured || (format != Format::UNKNOWN);
		}
		else
		{
			view.typedBuffer = true;
			view.textureBufferView = texture_buffer_view;
			view.textureViewOffsetInElements = uint32_t(offset / (uint64_t)GetFormatStride(format));
		}
		IRDescriptorTableSetBufferView(&entry, &view);
		return entry;
	}
	IRDescriptorTableEntry create_entry(MTL::Buffer* res, uint64_t size, uint64_t offset = 0, MTL::ResourceID resourceID = {}, Format format = Format::UNKNOWN, bool structured = false)
	{
		// This is the expanded version of IRDescriptorTableSetBufferView, made to be compatible with texture view pooling
		IRDescriptorTableEntry entry;
		entry.gpuVA = res->gpuAddress() + offset;
		entry.textureViewID = resourceID._impl;
		entry.metadata = (size & kIRBufSizeMask) << kIRBufSizeOffset;
		bool typedBuffer = false;
		uint64_t textureViewOffsetInElements = 0;
		if (entry.textureViewID == 0)
		{
			typedBuffer = structured || (format != Format::UNKNOWN);
		}
		else
		{
			typedBuffer = true;
			textureViewOffsetInElements = uint32_t(offset / (uint64_t)GetFormatStride(format));
		}
		entry.metadata |= ((uint64_t)textureViewOffsetInElements & kIRTexViewMask) << kIRTexViewOffset;
		entry.metadata |= (uint64_t)typedBuffer << kIRTypedBufferOffset;
		return entry;
	}
	IRDescriptorTableEntry create_entry(MTL::SamplerState* sam, float lod_bias = 0)
	{
		IRDescriptorTableEntry entry;
		IRDescriptorTableSetSampler(&entry, sam, lod_bias);
		return entry;
	}

	struct Buffer_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		NS::SharedPtr<MTL::Buffer> buffer;
		MTL::GPUAddress gpu_address = {};
		
		struct Subresource
		{
			IRDescriptorTableEntry entry = {};
			uint64_t offset = 0;
			uint64_t size = 0;
			int index = -1;
#ifndef USE_TEXTURE_VIEW_POOL
			MTL::Texture* view = nullptr;
#endif // USE_TEXTURE_VIEW_POOL
			
			bool IsValid() const { return entry.gpuVA != 0; }
		};
		Subresource srv;
		Subresource uav;
		Subresource* subresources_srv = nullptr;
		Subresource* subresources_uav = nullptr;
		
		void destroy_subresources()
		{
			
#ifndef USE_TEXTURE_VIEW_POOL
			if (srv.view != nullptr)
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(srv.view));
			if (uav.view != nullptr)
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(uav.view));
			for (size_t i = 0; i < arrlenu(subresources_srv); ++i)
			{
				auto& x = subresources_srv[i];
				if (x.view != nullptr)
					allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(x.view));
			}
			for (size_t i = 0; i < arrlenu(subresources_uav); ++i)
			{
				auto& x = subresources_uav[i];
				if (x.view != nullptr)
					allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(x.view));
			}
#endif // USE_TEXTURE_VIEW_POOL
			
			if (srv.index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_res, srv.index);
			srv = {};
			
			if (uav.index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_res, uav.index);
			uav = {};
			
			for (size_t i = 0; i < arrlenu(subresources_srv); ++i)
			{
				auto& x = subresources_srv[i];
				if (x.index >= 0)
					allocationhandler->Retire(allocationhandler->destroyer_bindless_res, x.index);
			}
			arrfree(subresources_srv);
			for (size_t i = 0; i < arrlenu(subresources_uav); ++i)
			{
				auto& x = subresources_uav[i];
				if (x.index >= 0)
					allocationhandler->Retire(allocationhandler->destroyer_bindless_res, x.index);
			}
			arrfree(subresources_uav);
		}

		~Buffer_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_resources, std::move(buffer));
			destroy_subresources();
			allocationhandler->destroylocker.unlock();
		}
	};
	struct Texture_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		NS::SharedPtr<MTL::Texture> texture;
		
		// Things for readback and upload texture types, linear tiling, using buffer:
		NS::SharedPtr<MTL::Buffer> buffer;
		SubresourceData* mapped_subresources = nullptr; // stb_ds array: mapped texture subresource layout metadata.
		
		struct Subresource
		{
			IRDescriptorTableEntry entry = {};
			int index = -1;
#ifndef USE_TEXTURE_VIEW_POOL
			MTL::Texture* view = nullptr;
#endif // USE_TEXTURE_VIEW_POOL
			
			uint32_t firstMip = 0;
			uint32_t mipCount = 0;
			uint32_t firstSlice = 0;
			uint32_t sliceCount = 0;
			
			bool IsValid() const { return entry.textureViewID != 0 || mipCount > 0 || sliceCount > 0; }
		};
		Subresource srv;
		Subresource uav;
		Subresource rtv;
		Subresource dsv;
		Subresource* subresources_srv = nullptr;
		Subresource* subresources_uav = nullptr;
		Subresource* subresources_rtv = nullptr;
		Subresource* subresources_dsv = nullptr;
		
		SparseTextureProperties sparse_properties;
		
		void destroy_subresources()
		{
			
#ifndef USE_TEXTURE_VIEW_POOL
			if (srv.view != nullptr)
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(srv.view));
			if (uav.view != nullptr)
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(uav.view));
			for (size_t i = 0; i < arrlenu(subresources_srv); ++i)
			{
				auto& x = subresources_srv[i];
				if (x.view != nullptr)
					allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(x.view));
			}
			for (size_t i = 0; i < arrlenu(subresources_uav); ++i)
			{
				auto& x = subresources_uav[i];
				if (x.view != nullptr)
					allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr(x.view));
			}
#endif // USE_TEXTURE_VIEW_POOL
			
			if (srv.index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_res, srv.index);
			srv = {};
			
			if (uav.index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_res, uav.index);
			uav = {};
			
			if (rtv.index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_res, rtv.index);
			
			if (dsv.index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_res, dsv.index);
			
			for (size_t i = 0; i < arrlenu(subresources_srv); ++i)
			{
				auto& x = subresources_srv[i];
				if (x.index >= 0)
					allocationhandler->Retire(allocationhandler->destroyer_bindless_res, x.index);
			}
			arrfree(subresources_srv);
			for (size_t i = 0; i < arrlenu(subresources_uav); ++i)
			{
				auto& x = subresources_uav[i];
				if (x.index >= 0)
					allocationhandler->Retire(allocationhandler->destroyer_bindless_res, x.index);
			}
			arrfree(subresources_uav);
			
			// RTV and DSV can just be cleared, they don't have any gpu-referenced resources:
			arrfree(subresources_rtv);
			arrfree(subresources_dsv);
		}

		~Texture_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
				if (texture.get() != nullptr)
					allocationhandler->Retire(allocationhandler->destroyer_resources, std::move(texture));
				if (buffer.get() != nullptr)
					allocationhandler->Retire(allocationhandler->destroyer_resources, std::move(buffer));
				arrfree(mapped_subresources);
				mapped_subresources = nullptr;
				destroy_subresources();
				allocationhandler->destroylocker.unlock();
			}
	};
	struct Sampler_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		int index = -1;
		NS::SharedPtr<MTL::SamplerState> sampler;
		IRDescriptorTableEntry entry = {};

		~Sampler_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_samplers, std::move(sampler));
			if (index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_sam, index);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct QueryHeap_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		NS::SharedPtr<MTL::Buffer> buffer;
		NS::SharedPtr<MTL4::CounterHeap> counter_heap;

		~QueryHeap_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_resources, std::move(buffer));
			allocationhandler->Retire(allocationhandler->destroyer_counter_heaps, std::move(counter_heap));
			allocationhandler->destroylocker.unlock();
		}
	};
	struct Shader_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		NS::SharedPtr<MTL::Library> library;
		NS::SharedPtr<MTL::Function> function;
		NS::SharedPtr<MTL::ComputePipelineState> compute_pipeline;
		GraphicsDevice_Metal::ShaderAdditionalData additional_data;

		~Shader_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_libraries, std::move(library));
			allocationhandler->Retire(allocationhandler->destroyer_functions, std::move(function));
			allocationhandler->Retire(allocationhandler->destroyer_compute_pipelines, std::move(compute_pipeline));
			allocationhandler->destroylocker.unlock();
		}
	};
	struct PipelineState_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		NS::SharedPtr<MTL::RenderPipelineDescriptor> descriptor;
		NS::SharedPtr<MTL::MeshRenderPipelineDescriptor> ms_descriptor;
		NS::SharedPtr<MTL::RenderPipelineState> render_pipeline;
		NS::SharedPtr<MTL::DepthStencilState> depth_stencil_state;
		MTL::Size numthreads_as = {};
		MTL::Size numthreads_ms = {};
		bool needs_draw_params = false;
		IRGeometryEmulationPipelineDescriptor gs_desc = {};

		~PipelineState_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_render_pipelines, std::move(render_pipeline));
			allocationhandler->Retire(allocationhandler->destroyer_depth_stencil_states, std::move(depth_stencil_state));
			allocationhandler->destroylocker.unlock();
		}
	};
	struct BVH_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		NS::SharedPtr<MTL::AccelerationStructure> acceleration_structure;
		NS::SharedPtr<MTL::Buffer> scratch;
		NS::SharedPtr<MTL::Buffer> tlas_header_instancecontributions;
		int tlas_descriptor_index = -1;
		IRDescriptorTableEntry tlas_entry = {};
		MTL::ResourceID resourceid = {};

		~BVH_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_resources, std::move(acceleration_structure));
			allocationhandler->Retire(allocationhandler->destroyer_resources, std::move(scratch));
			if (tlas_header_instancecontributions.get() != nullptr)
				allocationhandler->Retire(allocationhandler->destroyer_resources, std::move(tlas_header_instancecontributions));
			if (tlas_descriptor_index >= 0)
				allocationhandler->Retire(allocationhandler->destroyer_bindless_res, tlas_descriptor_index);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct RTPipelineState_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;

		~RTPipelineState_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->destroylocker.unlock();
		}
	};
	struct SwapChain_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;
		ColorSpace colorSpace = ColorSpace::SRGB;
		CA::MetalLayer* layer = nullptr;
		NS::SharedPtr<CA::MetalDrawable> current_drawable;
		NS::SharedPtr<MTL::Texture> current_texture;

		~SwapChain_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->Retire(allocationhandler->destroyer_resources, current_texture);
			allocationhandler->Retire(allocationhandler->destroyer_drawables, current_drawable);
			allocationhandler->destroylocker.unlock();
		}
	};
	struct VideoDecoder_Metal
	{
		wi::allocator::shared_ptr<GraphicsDevice_Metal::AllocationHandler> allocationhandler;

		~VideoDecoder_Metal()
		{
			if (allocationhandler == nullptr)
				return;
			allocationhandler->destroylocker.lock();
			allocationhandler->destroylocker.unlock();
		}
	};

	template<typename T> struct MetalType;
	template<> struct MetalType<GPUBuffer> { using type = Buffer_Metal; };
	template<> struct MetalType<Texture> { using type = Texture_Metal; };
	template<> struct MetalType<Sampler> { using type = Sampler_Metal; };
	template<> struct MetalType<GPUQueryHeap> { using type = QueryHeap_Metal; };
	template<> struct MetalType<Shader> { using type = Shader_Metal; };
	template<> struct MetalType<RaytracingAccelerationStructure> { using type = BVH_Metal; };
	template<> struct MetalType<PipelineState> { using type = PipelineState_Metal; };
	template<> struct MetalType<RaytracingPipelineState> { using type = RTPipelineState_Metal; };
	template<> struct MetalType<SwapChain> { using type = SwapChain_Metal; };
	template<> struct MetalType<VideoDecoder> { using type = VideoDecoder_Metal; };


	template<typename T>
	typename MetalType<T>::type* to_internal(const T* param)
	{
		return static_cast<typename MetalType<T>::type*>(param->internal_state.get());
	}

	template<typename T>
	typename MetalType<T>::type* to_internal(const GPUResource* res)
	{
		return static_cast<typename MetalType<T>::type*>(res->internal_state.get());
	}
}
using namespace metal_internal;

	void GraphicsDevice_Metal::binder_flush(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		
		if (commandlist.dirty_root)
		{
			// root CBVs:
			for (uint32_t i = 0; i < SDL_arraysize(RootLayout::root_cbvs); ++i)
			{
				if (!wiGraphicsGPUResourceIsValid(&commandlist.binding_table.CBV[i]))
					continue;
				auto internal_state = to_internal(&commandlist.binding_table.CBV[i]);
				commandlist.root.root_cbvs[i] = internal_state->gpu_address + commandlist.binding_table.CBV_offset[i];
				SDL_assert(is_aligned(commandlist.root.root_cbvs[i], MTL::GPUAddress(256)));
			}
			
			if (commandlist.dirty_resource)
			{
				commandlist.dirty_resource = false;
				ResourceTable resource_table = {};
				GPUAllocation resource_table_allocation = AllocateGPU(sizeof(ResourceTable), cmd);
				auto resource_table_allocation_internal = to_internal(&resource_table_allocation.buffer);
				commandlist.root.resource_table_ptr = resource_table_allocation_internal->gpu_address + resource_table_allocation.offset;
				
				for (uint32_t i = SDL_arraysize(RootLayout::root_cbvs); i < SDL_arraysize(DescriptorBindingTable::CBV); ++i)
				{
					if (!wiGraphicsGPUResourceIsValid(&commandlist.binding_table.CBV[i]))
						continue;
					
					auto internal_state = to_internal(&commandlist.binding_table.CBV[i]);
					const uint64_t offset = commandlist.binding_table.CBV_offset[i];
					const MTL::GPUAddress gpu_address = internal_state->gpu_address + offset;
					const uint64_t metadata = commandlist.binding_table.CBV[i].desc.size;
					IRDescriptorTableSetBuffer(&resource_table.cbvs[i - SDL_arraysize(RootLayout::root_cbvs)], gpu_address, metadata);
				}
				for (uint32_t i = 0; i < SDL_arraysize(commandlist.binding_table.SRV); ++i)
				{
					if (!wiGraphicsGPUResourceIsValid(&commandlist.binding_table.SRV[i]))
						continue;
					
					if (wiGraphicsGPUResourceIsBuffer(&commandlist.binding_table.SRV[i]))
					{
						auto internal_state = to_internal<GPUBuffer>(&commandlist.binding_table.SRV[i]);
						const int subresource_index = commandlist.binding_table.SRV_index[i];
						const auto& subresource = subresource_index < 0 ? internal_state->srv : internal_state->subresources_srv[subresource_index];
						resource_table.srvs[i] = subresource.entry;
					}
					else if (wiGraphicsGPUResourceIsTexture(&commandlist.binding_table.SRV[i]))
					{
						auto internal_state = to_internal<Texture>(&commandlist.binding_table.SRV[i]);
						const int subresource_index = commandlist.binding_table.SRV_index[i];
						const auto& subresource = subresource_index < 0 ? internal_state->srv : internal_state->subresources_srv[subresource_index];
						resource_table.srvs[i] = subresource.entry;
					}
					else if (wiGraphicsGPUResourceIsAccelerationStructure(&commandlist.binding_table.SRV[i]))
					{
						auto internal_state = to_internal<RaytracingAccelerationStructure>(&commandlist.binding_table.SRV[i]);
						resource_table.srvs[i] = internal_state->tlas_entry;
					}
				}
				for (uint32_t i = 0; i < SDL_arraysize(commandlist.binding_table.UAV); ++i)
				{
					if (!wiGraphicsGPUResourceIsValid(&commandlist.binding_table.UAV[i]))
						continue;
					
					if (wiGraphicsGPUResourceIsBuffer(&commandlist.binding_table.UAV[i]))
					{
						auto internal_state = to_internal<GPUBuffer>(&commandlist.binding_table.UAV[i]);
						const int subresource_index = commandlist.binding_table.UAV_index[i];
						const auto& subresource = subresource_index < 0 ? internal_state->uav : internal_state->subresources_uav[subresource_index];
						resource_table.uavs[i] = subresource.entry;
					}
					else if (wiGraphicsGPUResourceIsTexture(&commandlist.binding_table.UAV[i]))
					{
						auto internal_state = to_internal<Texture>(&commandlist.binding_table.UAV[i]);
						const int subresource_index = commandlist.binding_table.UAV_index[i];
						const auto& subresource = subresource_index < 0 ? internal_state->uav : internal_state->subresources_uav[subresource_index];
						resource_table.uavs[i] = subresource.entry;
					}
				}
				
				std::memcpy(resource_table_allocation.data, &resource_table, sizeof(resource_table));
			}
			if (commandlist.dirty_sampler)
			{
				commandlist.dirty_sampler = false;
				SamplerTable sampler_table = {};
				sampler_table.static_samplers = static_sampler_descriptors;
				GPUAllocation sampler_table_allocation = AllocateGPU(sizeof(SamplerTable), cmd);
				auto sampler_table_allocation_internal = to_internal(&sampler_table_allocation.buffer);
				commandlist.root.sampler_table_ptr = sampler_table_allocation_internal->gpu_address + sampler_table_allocation.offset;
				
				for (uint32_t i = 0; i < SDL_arraysize(commandlist.binding_table.SAM); ++i)
				{
					if (!wiGraphicsSamplerIsValid(&commandlist.binding_table.SAM[i]))
						continue;
					auto internal_state = to_internal(&commandlist.binding_table.SAM[i]);
					sampler_table.samplers[i] = internal_state->entry;
				}
				
				std::memcpy(sampler_table_allocation.data, &sampler_table, sizeof(sampler_table));
			}
			
			auto alloc = AllocateGPU(sizeof(commandlist.root), cmd);
			std::memcpy(alloc.data, &commandlist.root, sizeof(commandlist.root));
			auto alloc_internal = to_internal(&alloc.buffer);
			commandlist.argument_table->setAddress(alloc_internal->gpu_address + alloc.offset, kIRArgumentBufferBindPoint);
		}
		if (commandlist.dirty_vb && commandlist.active_pso != nullptr && commandlist.active_pso->desc.il != nullptr)
		{
			const InputLayout& il = *commandlist.active_pso->desc.il;
			for (size_t i = 0; i < arrlenu(il.elements); ++i)
			{
				auto& element = il.elements[i];
				auto& vb = commandlist.vertex_buffers[element.input_slot];
				if (vb.gpu_address == 0)
					continue;
				commandlist.argument_table->setAddress(vb.gpu_address, vb.stride, kIRVertexBufferBindPoint + i);
			}
		}
		
		// Flushing argument table updates to encoder:
		if (commandlist.dirty_root || commandlist.dirty_vb || commandlist.dirty_drawargs)
		{
			commandlist.dirty_root = false;
			commandlist.dirty_vb = false;
			commandlist.dirty_drawargs = false;
			
			if (commandlist.render_encoder.get() != nullptr)
			{
				commandlist.render_encoder->setArgumentTable(commandlist.argument_table.get(), MTL::RenderStageVertex | MTL::RenderStageObject | MTL::RenderStageMesh | MTL::RenderStageFragment);
			}
			else if (commandlist.compute_encoder.get() != nullptr)
			{
				commandlist.compute_encoder->setArgumentTable(commandlist.argument_table.get());
			}
		}
	}

	void GraphicsDevice_Metal::barrier_flush(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.barriers == nullptr || arrlenu(commandlist.barriers) == 0)
			return;
		MTL4::VisibilityOptions visibility_options = MTL4::VisibilityOptionNone;
		for (size_t i = 0; i < arrlenu(commandlist.barriers); ++i)
		{
			const auto& x = commandlist.barriers[i];
			if (x.type == GPUBarrier::Type::ALIASING)
			{
				visibility_options |= MTL4::VisibilityOptionResourceAlias;
			}
		}
		if (commandlist.render_encoder.get() != nullptr)
		{
			commandlist.render_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, visibility_options);
			commandlist.render_encoder->barrierAfterEncoderStages(MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment, MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment, visibility_options);
		}
		else if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, visibility_options);
			commandlist.compute_encoder->barrierAfterEncoderStages(MTL::StageDispatch | MTL::StageBlit | MTL::StageAccelerationStructure, MTL::StageDispatch | MTL::StageBlit | MTL::StageAccelerationStructure, visibility_options);
		}
		arrsetlen(commandlist.barriers, 0);
	}

	void GraphicsDevice_Metal::clear_flush(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.texture_clears == nullptr || arrlenu(commandlist.texture_clears) == 0)
			return;
		
		if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->endEncoding();
			commandlist.compute_encoder.reset();
		}
		NS::SharedPtr<NS::AutoreleasePool> autorelease_pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
		constexpr size_t batching = 8; // batch up to 8 clears into one pass (probably more will not be supported by renderpass)
		size_t offset = 0;
		while (offset < arrlenu(commandlist.texture_clears))
		{
			NS::SharedPtr<MTL4::RenderPassDescriptor> descriptor = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
			NS::SharedPtr<MTL::RenderPassColorAttachmentDescriptor> color_attachment_descriptors[8];
			for (size_t i = 0; (i < batching) && ((offset + i) < arrlenu(commandlist.texture_clears)); ++i)
			{
				const size_t index = offset + i;
				NS::SharedPtr<MTL::RenderPassColorAttachmentDescriptor>& color_attachment_descriptor = color_attachment_descriptors[i];
				color_attachment_descriptor = NS::TransferPtr(MTL::RenderPassColorAttachmentDescriptor::alloc()->init());
				const uint32_t value = commandlist.texture_clears[index].value;
				color_attachment_descriptor->setTexture(commandlist.texture_clears[index].texture);
				color_attachment_descriptor->setClearColor(MTL::ClearColor::Make((value & 0xFF) / 255.0f, ((value >> 8u) & 0xFF) / 255.0f, ((value >> 16u) & 0xFF) / 255.0f, ((value >> 24u) & 0xFF) / 255.0f));
				color_attachment_descriptor->setLoadAction(MTL::LoadActionClear);
				color_attachment_descriptor->setStoreAction(MTL::StoreActionStore);
				descriptor->colorAttachments()->setObject(color_attachment_descriptor.get(), i);
			}
			MTL4::RenderCommandEncoder* encoder = commandlist.commandbuffer->renderCommandEncoder(descriptor.get());
			encoder->setLabel(NS::String::string("ClearUAV", NS::UTF8StringEncoding));
			encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone); // TODO: flickering issues in several places without this
			encoder->endEncoding();
			offset += batching;
		}
		for (size_t i = 0; i < arrlenu(commandlist.texture_clears); ++i)
		{
			commandlist.texture_clears[i].texture->release();
		}
		arrsetlen(commandlist.texture_clears, 0);
	}

	void GraphicsDevice_Metal::pso_validate(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (!commandlist.dirty_pso)
			return;
		
		SDL_assert(commandlist.render_encoder.get() != nullptr); // We must be inside renderpass at this point!
		
		PipelineHash pipeline_hash = commandlist.pipeline_hash;
		const PipelineState* pso = commandlist.active_pso;
		auto internal_state = to_internal(pso);
		
		if (internal_state->render_pipeline.get() == nullptr)
		{
			// Just in time PSO:
			MTL::RenderPipelineState* pipeline = nullptr;
			MTL::DepthStencilState* depth_stencil_state = nullptr;
			for (size_t i = 0; i < arrlenu(pipelines_global); ++i)
			{
				if (pipelines_global[i].key == pipeline_hash)
				{
					pipeline = pipelines_global[i].value.pipeline;
					depth_stencil_state = pipelines_global[i].value.depth_stencil_state;
					break;
				}
			}
			if (pipeline == nullptr)
			{
				// Doesn't yet exist in global just in time PSO map, but maybe exists on this thread temporary PSO array:
				for (size_t i = 0; i < arrlenu(commandlist.pipelines_worker); ++i)
				{
					auto& x = commandlist.pipelines_worker[i];
					if (pipeline_hash == x.key)
					{
						pipeline = x.value.pipeline;
						depth_stencil_state = x.value.depth_stencil_state;
						break;
					}
				}
			}
			if (pipeline == nullptr)
			{
				RenderPassInfo renderpass_info = GetRenderPassInfo(cmd);
					PipelineState newPSO;
					bool success = CreatePipelineState(&pso->desc, &newPSO, &renderpass_info);
					SDL_assert(success);
					SDL_assert(wiGraphicsPipelineStateIsValid(&newPSO));

					auto internal_new = to_internal(&newPSO);
					SDL_assert(internal_new->render_pipeline.get() != nullptr);
					SDL_assert(internal_new->depth_stencil_state.get() != nullptr);
				PipelineGlobalEntry just_in_time_pso = {};
				just_in_time_pso.key = pipeline_hash;
				just_in_time_pso.value.pipeline = internal_new->render_pipeline.get();
				just_in_time_pso.value.depth_stencil_state = internal_new->depth_stencil_state.get();
				just_in_time_pso.value.pipeline->retain();
				just_in_time_pso.value.depth_stencil_state->retain();
				arrput(commandlist.pipelines_worker, just_in_time_pso);
				pipeline = just_in_time_pso.value.pipeline;
				depth_stencil_state = just_in_time_pso.value.depth_stencil_state;
			}
				SDL_assert(pipeline != nullptr);
				SDL_assert(depth_stencil_state != nullptr);
			commandlist.render_encoder->setRenderPipelineState(pipeline);
			commandlist.render_encoder->setDepthStencilState(depth_stencil_state);
		}
		else
		{
			// Precompiled PSO:
			commandlist.render_encoder->setRenderPipelineState(internal_state->render_pipeline.get());
			commandlist.render_encoder->setDepthStencilState(internal_state->depth_stencil_state.get());
		}

		RasterizerState rs_default = {};
		InitRasterizerState(rs_default);
		const RasterizerState& rs_desc = pso->desc.rs != nullptr ? *pso->desc.rs : rs_default;
		MTL::CullMode cull_mode = {};
		switch (rs_desc.cull_mode)
		{
			case CullMode::BACK:
				cull_mode = MTL::CullModeBack;
				break;
			case CullMode::FRONT:
				cull_mode = MTL::CullModeFront;
				break;
			case CullMode::CULL_NONE:
				cull_mode = MTL::CullModeNone;
				break;
		}
		commandlist.render_encoder->setCullMode(cull_mode);
		commandlist.render_encoder->setDepthBias((float)rs_desc.depth_bias, rs_desc.slope_scaled_depth_bias, rs_desc.depth_bias_clamp);
		commandlist.render_encoder->setDepthClipMode(rs_desc.depth_clip_enable ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp);
		MTL::TriangleFillMode fill_mode = {};
		switch (rs_desc.fill_mode)
		{
			case FillMode::SOLID:
				fill_mode = MTL::TriangleFillModeFill;
				break;
			case FillMode::WIREFRAME:
				fill_mode = MTL::TriangleFillModeLines;
				break;
		}
		commandlist.render_encoder->setTriangleFillMode(fill_mode);
		commandlist.render_encoder->setFrontFacingWinding(rs_desc.front_counter_clockwise ? MTL::WindingCounterClockwise : MTL::WindingClockwise);
		
		switch (pso->desc.pt)
		{
			case PrimitiveTopology::TRIANGLELIST:
				commandlist.primitive_type = MTL::PrimitiveTypeTriangle;
				commandlist.ir_primitive_type = IRRuntimePrimitiveTypeTriangle;
				break;
			case PrimitiveTopology::TRIANGLESTRIP:
				commandlist.primitive_type = MTL::PrimitiveTypeTriangleStrip;
				commandlist.ir_primitive_type = IRRuntimePrimitiveTypeTriangleStrip;
				break;
			case PrimitiveTopology::LINELIST:
				commandlist.primitive_type = MTL::PrimitiveTypeLine;
				commandlist.ir_primitive_type = IRRuntimePrimitiveTypeLine;
				break;
			case PrimitiveTopology::LINESTRIP:
				commandlist.primitive_type = MTL::PrimitiveTypeLineStrip;
				commandlist.ir_primitive_type = IRRuntimePrimitiveTypeLineStrip;
				break;
			case PrimitiveTopology::POINTLIST:
				commandlist.primitive_type = MTL::PrimitiveTypePoint;
				commandlist.ir_primitive_type = IRRuntimePrimitiveTypePoint;
				break;
			case PrimitiveTopology::PATCHLIST:
				commandlist.primitive_type = MTL::PrimitiveTypeTriangle;
				commandlist.ir_primitive_type = IRRuntimePrimitiveType3ControlPointPatchlist;
				break;
			default:
				break;
		}
		
		commandlist.dirty_pso = false;
	}
	void GraphicsDevice_Metal::predraw(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.active_cs = nullptr;
		SDL_assert(commandlist.render_encoder.get() != nullptr);
		commandlist.active_renderpass_has_draws = true;
		pso_validate(cmd);
		binder_flush(cmd);
		
		if (commandlist.dirty_scissor && commandlist.scissor_count > 0)
		{
			commandlist.dirty_scissor = false;
			MTL::ScissorRect scissors[SDL_arraysize(commandlist.scissors)];
			std::memcpy(&scissors, commandlist.scissors, sizeof(commandlist.scissors));
			for (uint32_t i = 0; i < commandlist.scissor_count; ++i)
			{
				MTL::ScissorRect& scissor = scissors[i];
				scissor.x = SDL_clamp(scissor.x, NS::UInteger(0), NS::UInteger(commandlist.render_width));
				scissor.y = SDL_clamp(scissor.y, NS::UInteger(0), NS::UInteger(commandlist.render_height));
				scissor.width = SDL_clamp(scissor.width, NS::UInteger(0), NS::UInteger(commandlist.render_width) - scissor.x);
				scissor.height = SDL_clamp(scissor.height, NS::UInteger(0), NS::UInteger(commandlist.render_height) - scissor.y);
			}
			commandlist.render_encoder->setScissorRects(scissors, commandlist.scissor_count);
		}
		if (commandlist.dirty_viewport && commandlist.viewport_count > 0)
		{
			commandlist.dirty_viewport = false;
			commandlist.render_encoder->setViewports(commandlist.viewports, commandlist.viewport_count);
		}
	}
	void GraphicsDevice_Metal::predispatch(CommandList cmd)
	{
		clear_flush(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.compute_encoder.get() == nullptr)
		{
			NS::SharedPtr<NS::AutoreleasePool> autorelease_pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
			commandlist.compute_encoder = NS::TransferPtr(commandlist.commandbuffer->computeCommandEncoder()->retain());
		}
		commandlist.active_pso = nullptr;
		commandlist.dirty_vb = true;
		commandlist.dirty_root = true;
		commandlist.dirty_sampler = true;
		commandlist.dirty_resource = true;
		commandlist.dirty_scissor = true;
		commandlist.dirty_viewport = true;
		commandlist.dirty_cs = true;
		
		if (commandlist.dirty_cs && commandlist.active_cs != nullptr)
		{
			commandlist.dirty_cs = false;
			auto internal_state = to_internal(commandlist.active_cs);
			commandlist.compute_encoder->setComputePipelineState(internal_state->compute_pipeline.get());
		}
		
		binder_flush(cmd);
		barrier_flush(cmd);
	}
	void GraphicsDevice_Metal::precopy(CommandList cmd)
	{
		clear_flush(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.compute_encoder.get() == nullptr)
		{
			NS::SharedPtr<NS::AutoreleasePool> autorelease_pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
			commandlist.compute_encoder = NS::TransferPtr(commandlist.commandbuffer->computeCommandEncoder()->retain());
		}
		
		barrier_flush(cmd);
	}

	bool GraphicsDevice_Metal::EnsureDrawCountICBEncoder()
	{
		auto& state = drawcount_icb_encoder;
		if (state.initialized)
			return true;
		if (state.failed)
			return false;

		NS::SharedPtr<NS::String> source = NS::TransferPtr(NS::String::alloc()->init(METAL_ICB_DRAWCOUNT_KERNEL_SOURCE, NS::UTF8StringEncoding));
		NS::SharedPtr<MTL::CompileOptions> options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
		// draw_mesh_threadgroups() on MSL render_command requires at least Metal language 3.1.
		options->setLanguageVersion(MTL::LanguageVersion3_1);
		options->setFastMathEnabled(true);

		NS::Error* error = nullptr;
		state.library = NS::TransferPtr(device->newLibrary(source.get(), options.get(), &error));
		if (error != nullptr || state.library.get() == nullptr)
		{
			const char* message = "unknown";
			if (error != nullptr)
			{
				message = error->localizedDescription()->utf8String();
				error->release();
			}
			METAL_LOG_ERROR("[Wicked::Metal] Failed to compile internal ICB draw-count kernels: %s", message);
			state.failed = true;
			return false;
		}

		auto create_function = [&](const char* name, NS::SharedPtr<MTL::Function>& function_out) -> bool
		{
			NS::SharedPtr<NS::String> entry = NS::TransferPtr(NS::String::alloc()->init(name, NS::UTF8StringEncoding));
			NS::SharedPtr<MTL::FunctionConstantValues> constants = NS::TransferPtr(MTL::FunctionConstantValues::alloc()->init());
			NS::Error* function_error = nullptr;
			function_out = NS::TransferPtr(state.library->newFunction(entry.get(), constants.get(), &function_error));
			if (function_error != nullptr || function_out.get() == nullptr)
			{
				const char* message = "unknown";
				if (function_error != nullptr)
				{
					message = function_error->localizedDescription()->utf8String();
					function_error->release();
				}
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create internal ICB function '%s': %s", name, message);
				return false;
			}
			return true;
		};

		if (!create_function("wicked_encode_draw_indirect_count_icb", state.draw_function))
		{
			state.failed = true;
			return false;
		}
		if (!create_function("wicked_encode_draw_indexed_indirect_count_icb", state.draw_indexed_function))
		{
			state.failed = true;
			return false;
		}
		if (!create_function("wicked_encode_draw_mesh_indirect_count_icb", state.draw_mesh_function))
		{
			state.failed = true;
			return false;
		}

		auto create_pipeline = [&](MTL::Function* function, NS::SharedPtr<MTL::ComputePipelineState>& pipeline_out) -> bool
		{
			NS::SharedPtr<MTL::ComputePipelineDescriptor> descriptor = NS::TransferPtr(MTL::ComputePipelineDescriptor::alloc()->init());
			descriptor->setComputeFunction(function);
			descriptor->setSupportIndirectCommandBuffers(true);
			NS::Error* pipeline_error = nullptr;
			pipeline_out = NS::TransferPtr(device->newComputePipelineState(descriptor.get(), MTL::PipelineOptionNone, nullptr, &pipeline_error));
			if (pipeline_error != nullptr || pipeline_out.get() == nullptr)
			{
				const char* message = "unknown";
				if (pipeline_error != nullptr)
				{
					message = pipeline_error->localizedDescription()->utf8String();
					pipeline_error->release();
				}
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create internal ICB compute pipeline: %s", message);
				return false;
			}
			if (!pipeline_out->supportIndirectCommandBuffers())
			{
				METAL_LOG_ERROR("[Wicked::Metal] Internal ICB compute pipeline doesn't support indirect command buffers");
				return false;
			}
			return true;
		};

		if (!create_pipeline(state.draw_function.get(), state.draw_pipeline))
		{
			state.failed = true;
			return false;
		}
		if (!create_pipeline(state.draw_indexed_function.get(), state.draw_indexed_pipeline))
		{
			state.failed = true;
			return false;
		}
		if (!create_pipeline(state.draw_mesh_function.get(), state.draw_mesh_pipeline))
		{
			state.failed = true;
			return false;
		}

		{
			NS::SharedPtr<MTL::ArgumentEncoder> argument_encoder = NS::TransferPtr(state.draw_function->newArgumentEncoder(METAL_ICB_CONTAINER_BUFFER_INDEX));
			if (argument_encoder.get() == nullptr)
			{
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create internal draw ICB argument encoder");
				state.failed = true;
				return false;
			}
			state.draw_icb_argument_buffer_size = (uint32_t)argument_encoder->encodedLength();
		}
		{
			NS::SharedPtr<MTL::ArgumentEncoder> argument_encoder = NS::TransferPtr(state.draw_indexed_function->newArgumentEncoder(METAL_ICB_CONTAINER_BUFFER_INDEX));
			if (argument_encoder.get() == nullptr)
			{
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create internal indexed ICB argument encoder");
				state.failed = true;
				return false;
			}
			state.draw_indexed_icb_argument_buffer_size = (uint32_t)argument_encoder->encodedLength();
		}
		{
			NS::SharedPtr<MTL::ArgumentEncoder> argument_encoder = NS::TransferPtr(state.draw_mesh_function->newArgumentEncoder(METAL_ICB_CONTAINER_BUFFER_INDEX));
			if (argument_encoder.get() == nullptr)
			{
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create internal mesh ICB argument encoder");
				state.failed = true;
				return false;
			}
			state.draw_mesh_icb_argument_buffer_size = (uint32_t)argument_encoder->encodedLength();
		}

		state.initialized = true;
		return true;
	}

	bool GraphicsDevice_Metal::EnsureDrawCountICBResources(CommandList cmd, bool indexed, uint32_t max_count)
	{
		if (!EnsureDrawCountICBEncoder())
			return false;

		CommandList_Metal& commandlist = GetCommandList(cmd);
		CommandList_Metal::DrawCountICBState& icb_state = indexed ? commandlist.draw_indexed_count_icb : commandlist.draw_count_icb;
		const uint32_t required_argument_buffer_size = indexed ? drawcount_icb_encoder.draw_indexed_icb_argument_buffer_size : drawcount_icb_encoder.draw_icb_argument_buffer_size;
		const uint32_t required_capacity = std::max(1u, max_count);
		if (
			icb_state.icb.get() != nullptr &&
			icb_state.icb_argument_buffer.get() != nullptr &&
			icb_state.icb_execution_range_buffer.get() != nullptr &&
			icb_state.capacity >= required_capacity
			)
		{
			return true;
		}

		if (icb_state.icb.get() != nullptr || icb_state.icb_argument_buffer.get() != nullptr || icb_state.icb_execution_range_buffer.get() != nullptr)
		{
			std::scoped_lock lock(allocationhandler->destroylocker);
			if (icb_state.icb.get() != nullptr)
			{
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr((MTL::Resource*)icb_state.icb.get()->retain()));
			}
			if (icb_state.icb_argument_buffer.get() != nullptr)
			{
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr((MTL::Resource*)icb_state.icb_argument_buffer.get()->retain()));
			}
			if (icb_state.icb_execution_range_buffer.get() != nullptr)
			{
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr((MTL::Resource*)icb_state.icb_execution_range_buffer.get()->retain()));
			}
			icb_state.icb.reset();
			icb_state.icb_argument_buffer.reset();
			icb_state.icb_execution_range_buffer.reset();
			icb_state.capacity = 0;
		}

		NS::SharedPtr<MTL::IndirectCommandBufferDescriptor> descriptor = NS::TransferPtr(MTL::IndirectCommandBufferDescriptor::alloc()->init());
		descriptor->setCommandTypes(indexed ? MTL::IndirectCommandTypeDrawIndexed : MTL::IndirectCommandTypeDraw);
		descriptor->setInheritBuffers(true);
		descriptor->setInheritPipelineState(true);
		descriptor->setInheritCullMode(true);
		descriptor->setInheritDepthBias(true);
		descriptor->setInheritDepthClipMode(true);
		descriptor->setInheritDepthStencilState(true);
		descriptor->setInheritFrontFacingWinding(true);
		descriptor->setInheritTriangleFillMode(true);
		descriptor->setMaxVertexBufferBindCount(31);
		descriptor->setSupportDynamicAttributeStride(true);

		icb_state.icb = NS::TransferPtr(device->newIndirectCommandBuffer(descriptor.get(), required_capacity, MTL::ResourceStorageModePrivate));
		icb_state.icb_argument_buffer = NS::TransferPtr(device->newBuffer(required_argument_buffer_size, MTL::ResourceStorageModeShared));
		icb_state.icb_execution_range_buffer = NS::TransferPtr(device->newBuffer(sizeof(MetalICBExecutionRange), MTL::ResourceStorageModePrivate));
		if (icb_state.icb.get() == nullptr || icb_state.icb_argument_buffer.get() == nullptr || icb_state.icb_execution_range_buffer.get() == nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] Failed to allocate internal resources for indirect draw count ICB");
			icb_state.icb.reset();
			icb_state.icb_argument_buffer.reset();
			icb_state.icb_execution_range_buffer.reset();
			icb_state.capacity = 0;
			return false;
		}

		allocationhandler->make_resident(icb_state.icb.get());
		allocationhandler->make_resident(icb_state.icb_argument_buffer.get());
		allocationhandler->make_resident(icb_state.icb_execution_range_buffer.get());
		icb_state.capacity = required_capacity;
		return true;
	}

	bool GraphicsDevice_Metal::EnsureMeshCountICBResources(CommandList cmd, uint32_t max_count)
	{
		if (!EnsureDrawCountICBEncoder())
			return false;

		CommandList_Metal& commandlist = GetCommandList(cmd);
		CommandList_Metal::DrawCountICBState& icb_state = commandlist.draw_mesh_count_icb;
		const uint32_t required_argument_buffer_size = drawcount_icb_encoder.draw_mesh_icb_argument_buffer_size;
		const uint32_t required_capacity = std::max(1u, max_count);
		if (
			icb_state.icb.get() != nullptr &&
			icb_state.icb_argument_buffer.get() != nullptr &&
			icb_state.icb_execution_range_buffer.get() != nullptr &&
			icb_state.capacity >= required_capacity
			)
		{
			return true;
		}

		if (icb_state.icb.get() != nullptr || icb_state.icb_argument_buffer.get() != nullptr || icb_state.icb_execution_range_buffer.get() != nullptr)
		{
			std::scoped_lock lock(allocationhandler->destroylocker);
			if (icb_state.icb.get() != nullptr)
			{
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr((MTL::Resource*)icb_state.icb.get()->retain()));
			}
			if (icb_state.icb_argument_buffer.get() != nullptr)
			{
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr((MTL::Resource*)icb_state.icb_argument_buffer.get()->retain()));
			}
			if (icb_state.icb_execution_range_buffer.get() != nullptr)
			{
				allocationhandler->Retire(allocationhandler->destroyer_resources, NS::TransferPtr((MTL::Resource*)icb_state.icb_execution_range_buffer.get()->retain()));
			}
			icb_state.icb.reset();
			icb_state.icb_argument_buffer.reset();
			icb_state.icb_execution_range_buffer.reset();
			icb_state.capacity = 0;
		}

		NS::SharedPtr<MTL::IndirectCommandBufferDescriptor> descriptor = NS::TransferPtr(MTL::IndirectCommandBufferDescriptor::alloc()->init());
		descriptor->setCommandTypes(MTL::IndirectCommandTypeDrawMeshThreadgroups);
		descriptor->setInheritBuffers(true);
		descriptor->setInheritPipelineState(true);
		descriptor->setInheritCullMode(true);
		descriptor->setInheritDepthBias(true);
		descriptor->setInheritDepthClipMode(true);
		descriptor->setInheritDepthStencilState(true);
		descriptor->setInheritFrontFacingWinding(true);
		descriptor->setInheritTriangleFillMode(true);
		descriptor->setMaxObjectBufferBindCount(31);
		descriptor->setMaxMeshBufferBindCount(31);

		icb_state.icb = NS::TransferPtr(device->newIndirectCommandBuffer(descriptor.get(), required_capacity, MTL::ResourceStorageModePrivate));
		icb_state.icb_argument_buffer = NS::TransferPtr(device->newBuffer(required_argument_buffer_size, MTL::ResourceStorageModeShared));
		icb_state.icb_execution_range_buffer = NS::TransferPtr(device->newBuffer(sizeof(MetalICBExecutionRange), MTL::ResourceStorageModePrivate));
		if (icb_state.icb.get() == nullptr || icb_state.icb_argument_buffer.get() == nullptr || icb_state.icb_execution_range_buffer.get() == nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] Failed to allocate internal resources for indirect mesh draw count ICB");
			icb_state.icb.reset();
			icb_state.icb_argument_buffer.reset();
			icb_state.icb_execution_range_buffer.reset();
			icb_state.capacity = 0;
			return false;
		}

		allocationhandler->make_resident(icb_state.icb.get());
		allocationhandler->make_resident(icb_state.icb_argument_buffer.get());
		allocationhandler->make_resident(icb_state.icb_execution_range_buffer.get());
		icb_state.capacity = required_capacity;
		return true;
	}

	bool GraphicsDevice_Metal::EndRenderPassForIndirectEncoding(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.render_encoder.get() == nullptr)
			return true;

		if (commandlist.active_renderpass_occlusionqueries != nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] GPU-encoded indirect count draw is not supported while occlusion query heap is attached to the active render pass");
			SDL_assert(false);
			return false;
		}

		if (!commandlist.active_renderpass_is_swapchain)
		{
			for (size_t i = 0; i < arrlenu(commandlist.active_renderpass_images); ++i)
			{
				const RenderPassImage& image = commandlist.active_renderpass_images[i];
				if (
					(image.type == RenderPassImage::Type::RENDERTARGET || image.type == RenderPassImage::Type::DEPTH_STENCIL) &&
					image.storeop == RenderPassImage::StoreOp::STOREOP_DONTCARE
					)
				{
					METAL_LOG_ERROR("[Wicked::Metal] GPU-encoded indirect count draw requires STORE store-op for active render targets when splitting render pass");
					SDL_assert(false);
					return false;
				}
			}
		}

		commandlist.render_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone);
		commandlist.render_encoder->endEncoding();
		commandlist.render_encoder.reset();
		commandlist.render_encoder = nullptr;
		commandlist.dirty_pso = true;
		return true;
	}

	bool GraphicsDevice_Metal::ResumeRenderPassAfterIndirectEncoding(CommandList cmd, bool load_attachments)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		const bool had_draws_before_resume = commandlist.active_renderpass_has_draws;
		if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->endEncoding();
			commandlist.compute_encoder.reset();
			commandlist.compute_encoder = nullptr;
		}

		if (commandlist.active_renderpass_is_swapchain)
		{
			if (!wiGraphicsSwapChainIsValid(commandlist.active_renderpass_swapchain))
			{
				METAL_LOG_ERROR("[Wicked::Metal] Failed to resume swapchain render pass for indirect draw count");
				SDL_assert(false);
				return false;
			}

			auto internal_state = to_internal(commandlist.active_renderpass_swapchain);
			if (internal_state->current_drawable.get() == nullptr)
			{
				METAL_LOG_ERROR("[Wicked::Metal] Active swapchain drawable is missing while resuming render pass for indirect draw count");
				SDL_assert(false);
				return false;
			}

			NS::SharedPtr<MTL4::RenderPassDescriptor> descriptor = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
			NS::SharedPtr<MTL::RenderPassColorAttachmentDescriptor> color_attachment_descriptor = NS::TransferPtr(MTL::RenderPassColorAttachmentDescriptor::alloc()->init());
			CGSize size = internal_state->layer->drawableSize();
			descriptor->setRenderTargetWidth(size.width);
			descriptor->setRenderTargetHeight(size.height);
			descriptor->setDefaultRasterSampleCount(1);
			color_attachment_descriptor->setTexture(internal_state->current_drawable->texture());
			if (load_attachments)
			{
				color_attachment_descriptor->setLoadAction(MTL::LoadActionLoad);
			}
			else
			{
				color_attachment_descriptor->setLoadAction(MTL::LoadActionClear);
				color_attachment_descriptor->setClearColor(MTL::ClearColor::Make(
					commandlist.active_renderpass_swapchain->desc.clear_color[0],
					commandlist.active_renderpass_swapchain->desc.clear_color[1],
					commandlist.active_renderpass_swapchain->desc.clear_color[2],
					commandlist.active_renderpass_swapchain->desc.clear_color[3]));
			}
			color_attachment_descriptor->setStoreAction(MTL::StoreActionStore);
			descriptor->colorAttachments()->setObject(color_attachment_descriptor.get(), 0);
			commandlist.render_encoder = NS::TransferPtr(commandlist.commandbuffer->renderCommandEncoder(descriptor.get())->retain());
			commandlist.render_width = size.width;
			commandlist.render_height = size.height;
			commandlist.renderpass_info = wiGraphicsCreateRenderPassInfoFromSwapChainDesc(&commandlist.active_renderpass_swapchain->desc);
			commandlist.dirty_vb = true;
			commandlist.dirty_root = true;
			commandlist.dirty_sampler = true;
			commandlist.dirty_resource = true;
			commandlist.dirty_scissor = true;
			commandlist.dirty_viewport = true;
			commandlist.dirty_pso = true;
			commandlist.active_renderpass_has_draws = load_attachments ? had_draws_before_resume : false;
			barrier_flush(cmd);
			return true;
		}

		if (commandlist.active_renderpass_images == nullptr || arrlenu(commandlist.active_renderpass_images) == 0)
		{
			METAL_LOG_ERROR("[Wicked::Metal] Failed to resume render pass for indirect draw count because no active render pass attachments were cached");
			SDL_assert(false);
			return false;
		}

		RenderPassImage* resume_images = nullptr;
		arrsetlen(resume_images, arrlenu(commandlist.active_renderpass_images));
		std::memcpy(resume_images, commandlist.active_renderpass_images, sizeof(RenderPassImage) * arrlenu(commandlist.active_renderpass_images));
		if (load_attachments)
		{
			for (size_t i = 0; i < arrlenu(resume_images); ++i)
			{
				if (resume_images[i].type == RenderPassImage::Type::RENDERTARGET || resume_images[i].type == RenderPassImage::Type::DEPTH_STENCIL)
				{
					resume_images[i].loadop = RenderPassImage::LoadOp::LOAD;
				}
			}
		}
		RenderPassBegin(resume_images, (uint32_t)arrlenu(resume_images), commandlist.active_renderpass_occlusionqueries, cmd, RenderPassFlags::RENDER_PASS_FLAG_NONE);
		commandlist.active_renderpass_has_draws = load_attachments ? had_draws_before_resume : false;
		arrfree(resume_images);
		return true;
	}

	GraphicsDevice_Metal::GraphicsDevice_Metal(ValidationMode validationMode_, GPUPreference preference)
	{
		validationMode = validationMode_;
		const Uint64 timer_begin = SDL_GetPerformanceCounter();
#if defined(WICKED_MMGR_ENABLED) && WI_ENGINECONFIG_MMGR_METAL_TEST_LEAK
		if (g_metal_mmgr_intentional_leak == nullptr)
		{
			g_metal_mmgr_intentional_leak = mmgrAllocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_malloc, sizeof(void*), 256);
			SDL_LogWarn(
				SDL_LOG_CATEGORY_APPLICATION,
				"[Wicked::Metal] MMGR test leak is enabled. Intentional leak allocated at %p (size=%u)",
				g_metal_mmgr_intentional_leak,
				256u);
		}
#endif
		device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
		
		if (device.get() == nullptr)
		{
			METAL_LOG_ERROR("Metal graphics device creation failed, exiting!");
			wi::platform::Exit();
		}
		if (!device->supportsFamily(MTL::GPUFamilyMetal4))
		{
			METAL_LOG_ERROR("Metal 4 graphics is not supported by your system, exiting!");
			wi::platform::Exit();
		}
		
		adapterName = device->name()->cString(NS::UTF8StringEncoding);
		
		capabilities |= GraphicsDeviceCapability::SAMPLER_MINMAX;
		capabilities |= GraphicsDeviceCapability::ALIASING_GENERIC;
		capabilities |= GraphicsDeviceCapability::DEPTH_BOUNDS_TEST;
		capabilities |= GraphicsDeviceCapability::UAV_LOAD_FORMAT_COMMON;
		capabilities |= GraphicsDeviceCapability::UAV_LOAD_FORMAT_R11G11B10_FLOAT;
		capabilities |= GraphicsDeviceCapability::SPARSE_BUFFER;
		capabilities |= GraphicsDeviceCapability::SPARSE_TEXTURE2D;
		capabilities |= GraphicsDeviceCapability::SPARSE_TEXTURE3D;
		capabilities |= GraphicsDeviceCapability::SPARSE_NULL_MAPPING;
		capabilities |= GraphicsDeviceCapability::DEPTH_RESOLVE_MIN_MAX;
		capabilities |= GraphicsDeviceCapability::STENCIL_RESOLVE_MIN_MAX;
		capabilities |= GraphicsDeviceCapability::MESH_SHADER;
		capabilities |= GraphicsDeviceCapability::COPY_BETWEEN_DIFFERENT_IMAGE_ASPECTS_NOT_SUPPORTED;
		
		if (device->hasUnifiedMemory())
		{
			capabilities |= GraphicsDeviceCapability::CACHE_COHERENT_UMA;
		}
		if (device->supportsRaytracing())
		{
			capabilities |= GraphicsDeviceCapability::RAYTRACING;
			TOPLEVEL_ACCELERATION_STRUCTURE_INSTANCE_SIZE = sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor);
		}
		
		TIMESTAMP_FREQUENCY = device->queryTimestampFrequency();
		
		uploadqueue = NS::TransferPtr(device->newMTL4CommandQueue());
		allocationhandler = wi::allocator::make_shared_single<AllocationHandler>();
		
		argument_table_desc = NS::TransferPtr(MTL4::ArgumentTableDescriptor::alloc()->init());
		argument_table_desc->setInitializeBindings(false);
		argument_table_desc->setSupportAttributeStrides(true);
		argument_table_desc->setMaxBufferBindCount(31);
		argument_table_desc->setMaxTextureBindCount(0);
		argument_table_desc->setMaxSamplerStateBindCount(0);
		
		descriptor_heap_res = NS::TransferPtr(device->newBuffer(BINDLESS_RESOURCE_CAPACITY * sizeof(IRDescriptorTableEntry), MTL::ResourceStorageModeShared));
		descriptor_heap_res->setLabel(NS::TransferPtr(NS::String::alloc()->init("descriptor_heap_res", NS::UTF8StringEncoding)).get());
		
		const uint64_t real_bindless_sampler_capacity = std::min((uint64_t)BINDLESS_SAMPLER_CAPACITY, (uint64_t)device->maxArgumentBufferSamplerCount());
		descriptor_heap_sam = NS::TransferPtr(device->newBuffer(real_bindless_sampler_capacity * sizeof(IRDescriptorTableEntry), MTL::ResourceStorageModeShared));
		descriptor_heap_sam->setLabel(NS::TransferPtr(NS::String::alloc()->init("descriptor_heap_sam", NS::UTF8StringEncoding)).get());
		
		descriptor_heap_res_data = (IRDescriptorTableEntry*)descriptor_heap_res->contents();
		descriptor_heap_sam_data = (IRDescriptorTableEntry*)descriptor_heap_sam->contents();
		
		arrsetcap(allocationhandler->free_bindless_res, BINDLESS_RESOURCE_CAPACITY);
		arrsetlen(allocationhandler->free_bindless_res, 0);
		arrsetcap(allocationhandler->free_bindless_sam, real_bindless_sampler_capacity);
		arrsetlen(allocationhandler->free_bindless_sam, 0);
		for (int i = 0; i < real_bindless_sampler_capacity; ++i)
		{
			arrput(allocationhandler->free_bindless_sam, (int)real_bindless_sampler_capacity - i - 1);
		}
		for (int i = 0; i < BINDLESS_RESOURCE_CAPACITY; ++i)
		{
			arrput(allocationhandler->free_bindless_res, (int)BINDLESS_RESOURCE_CAPACITY - i - 1);
		}
		
		NS::SharedPtr<MTL::ResidencySetDescriptor> residency_set_descriptor = NS::TransferPtr(MTL::ResidencySetDescriptor::alloc()->init());
		residency_set_descriptor->setInitialCapacity(BINDLESS_RESOURCE_CAPACITY + real_bindless_sampler_capacity);
		NS::Error* error = nullptr;
		allocationhandler->residency_set = NS::TransferPtr(device->newResidencySet(residency_set_descriptor.get(), &error));
		if (error != nullptr)
		{
			NS::String* errDesc = error->localizedDescription();
			METAL_ASSERT_MSG(0, "%s", errDesc->utf8String());
			error->release();
		}
		uploadqueue->addResidencySet(allocationhandler->residency_set.get());
		allocationhandler->make_resident(descriptor_heap_res.get());
		allocationhandler->make_resident(descriptor_heap_sam.get());
		
#ifdef USE_TEXTURE_VIEW_POOL
		NS::SharedPtr<MTL::ResourceViewPoolDescriptor> view_pool_desc = NS::TransferPtr(MTL::ResourceViewPoolDescriptor::alloc()->init());
		view_pool_desc->setResourceViewCount(BINDLESS_RESOURCE_CAPACITY);
		texture_view_pool = NS::TransferPtr(device->newTextureViewPool(view_pool_desc.get(), &error));
		if (error != nullptr)
		{
			NS::String* errDesc = error->localizedDescription();
			METAL_ASSERT_MSG(0, "%s", errDesc->utf8String());
			error->release();
		}
#endif // USE_TEXTURE_VIEW_POOL
		
		// Static samplers workaround:
		NS::SharedPtr<MTL::SamplerDescriptor> sampler_descriptor = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
		sampler_descriptor->setLodMinClamp(0);
		sampler_descriptor->setLodMaxClamp(FLT_MAX);
		sampler_descriptor->setBorderColor(MTL::SamplerBorderColorTransparentBlack);
		sampler_descriptor->setMaxAnisotropy(1);
		sampler_descriptor->setReductionMode(MTL::SamplerReductionModeWeightedAverage);
		sampler_descriptor->setSupportArgumentBuffers(true);
		
		sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterLinear);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		static_samplers[0] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeRepeat);
		static_samplers[1] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		static_samplers[2] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		
		sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterNearest);
		sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterNearest);
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		static_samplers[3] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeRepeat);
		static_samplers[4] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		static_samplers[5] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		
		sampler_descriptor->setMaxAnisotropy(16);
		sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterLinear);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		static_samplers[6] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeRepeat);
		static_samplers[7] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeMirrorRepeat);
		static_samplers[8] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		
		sampler_descriptor->setMaxAnisotropy(1);
		sampler_descriptor->setLodMaxClamp(0);
		sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setCompareFunction(MTL::CompareFunctionGreaterEqual);
		static_samplers[9] = NS::TransferPtr(device->newSamplerState(sampler_descriptor.get()));
		
		for (uint32_t i = 0; i < SDL_arraysize(static_samplers); ++i)
		{
			IRDescriptorTableSetSampler(&static_sampler_descriptors.samplers[i], static_samplers[i].get(), 0);
		}
		
		queues[QUEUE_GRAPHICS].queue = NS::TransferPtr(device->newMTL4CommandQueue());
		queues[QUEUE_GRAPHICS].queue->addResidencySet(allocationhandler->residency_set.get());
		queues[QUEUE_COMPUTE].queue = NS::TransferPtr(device->newMTL4CommandQueue());
		queues[QUEUE_COMPUTE].queue->addResidencySet(allocationhandler->residency_set.get());
		queues[QUEUE_COPY].queue = NS::TransferPtr(device->newMTL4CommandQueue());
		queues[QUEUE_COPY].queue->addResidencySet(allocationhandler->residency_set.get());
		
		for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
		{
			submission_token_events[q] = NS::TransferPtr(device->newSharedEvent());
			submission_token_events[q]->setSignaledValue(0);
			allocationhandler->queue_timeline_events[q] = submission_token_events[q];
		}
		
		METAL_LOG("Created GraphicsDevice_Metal (%d ms)", (int)std::round(GetElapsedMilliseconds(timer_begin)));
	}
	GraphicsDevice_Metal::~GraphicsDevice_Metal()
	{
		WaitForGPU();
		retire_completed_uploads();
		ClearPipelineStateCache();

		for (size_t i = 0; i < arrlenu(commandlists); ++i)
		{
			CommandList_Metal* commandlist = commandlists[i];
			if (commandlist == nullptr)
				continue;
			if (commandlist->presents != nullptr)
			{
				for (size_t j = 0; j < arrlenu(commandlist->presents); ++j)
				{
					if (commandlist->presents[j] != nullptr)
						commandlist->presents[j]->release();
				}
				arrfree(commandlist->presents);
			}
			if (commandlist->texture_clears != nullptr)
			{
				for (size_t j = 0; j < arrlenu(commandlist->texture_clears); ++j)
				{
					if (commandlist->texture_clears[j].texture != nullptr)
						commandlist->texture_clears[j].texture->release();
				}
				arrfree(commandlist->texture_clears);
			}
			if (commandlist->pipelines_worker != nullptr)
			{
				for (size_t j = 0; j < arrlenu(commandlist->pipelines_worker); ++j)
				{
					if (commandlist->pipelines_worker[j].value.pipeline != nullptr)
						commandlist->pipelines_worker[j].value.pipeline->release();
					if (commandlist->pipelines_worker[j].value.depth_stencil_state != nullptr)
						commandlist->pipelines_worker[j].value.depth_stencil_state->release();
				}
				arrfree(commandlist->pipelines_worker);
			}
			arrfree(commandlist->wait_for_cmd_ids);
			arrfree(commandlist->barriers);
		}
		arrfree(commandlists);
		arrfree(open_commandlists);
		for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
		{
			arrfree(retired_contexts[q]);
		}
		for (auto& queue : queues)
		{
			arrfree(queue.submit_cmds);
		}
	}

	bool GraphicsDevice_Metal::CreateSwapChain(const SwapChainDesc* desc, wi::platform::window_type window, SwapChain* swapchain) const
	{
		auto internal_state = wiGraphicsSwapChainIsValid(swapchain) ? wi::allocator::shared_ptr<SwapChain_Metal>(swapchain->internal_state) : wi::allocator::make_shared<SwapChain_Metal>();
		internal_state->allocationhandler = allocationhandler;
		swapchain->internal_state = internal_state;
		swapchain->desc = *desc;
		
		swapchain->desc.buffer_count = SDL_clamp(desc->buffer_count, 2u, 3u);
		
		if (internal_state->layer == nullptr)
		{
			internal_state->layer = CA::MetalLayer::layer();
			internal_state->layer->setDevice(device.get());
			internal_state->layer->setMaximumDrawableCount(swapchain->desc.buffer_count);
			internal_state->layer->setFramebufferOnly(false); // GetBackBuffer() srv
			wi::apple::SetMetalLayerToWindow(window, internal_state->layer);
		}
		
		CGSize size = {(CGFloat)desc->width, (CGFloat)desc->height};
		internal_state->layer->setDrawableSize(size);
		internal_state->layer->setDisplaySyncEnabled(desc->vsync);
		internal_state->layer->setPixelFormat(_ConvertPixelFormat(desc->format));

		return internal_state->layer != nullptr;
	}
	bool GraphicsDevice_Metal::CreateBuffer2(const GPUBufferDesc* desc, const std::function<void(void*)>& init_callback, GPUBuffer* buffer, const GPUResource* alias, uint64_t alias_offset) const
	{
		auto internal_state = wi::allocator::make_shared<Buffer_Metal>();
		internal_state->allocationhandler = allocationhandler;
		buffer->internal_state = internal_state;
		buffer->type = GPUResource::Type::BUFFER;
		buffer->mapped_data = nullptr;
		buffer->mapped_size = 0;
		buffer->desc = *desc;
		
		const bool sparse = has_flag(desc->misc_flags, ResourceMiscFlag::SPARSE);
		const bool aliasing_storage = has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_BUFFER) || has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS) || has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_RT_DS);
		
		MTL::ResourceOptions resource_options = {};
		if (desc->usage == Usage::DEFAULT)
		{
			if (aliasing_storage)
			{
				// potentially used for sparse or other private requirement resource
				resource_options |= MTL::ResourceStorageModePrivate;
			}
			else if (CheckCapability(GraphicsDeviceCapability::CACHE_COHERENT_UMA))
			{
				resource_options |= MTL::ResourceStorageModeShared;
			}
			else
			{
				resource_options |= MTL::ResourceStorageModePrivate;
			}
		}
		else if (desc->usage == Usage::UPLOAD)
		{
			resource_options |= MTL::ResourceStorageModeShared;
			resource_options |= MTL::ResourceCPUCacheModeWriteCombined;
		}
		else if (desc->usage == Usage::READBACK)
		{
			resource_options |= MTL::ResourceStorageModeShared;
		}
		
		if (aliasing_storage)
		{
			// This is an aliasing storage:
			MTL::SizeAndAlign sizealign = device->heapBufferSizeAndAlign(desc->size, resource_options);
			NS::SharedPtr<MTL::HeapDescriptor> heap_desc = NS::TransferPtr(MTL::HeapDescriptor::alloc()->init());
			heap_desc->setResourceOptions(resource_options);
			heap_desc->setSize(sizealign.size);
			heap_desc->setType(MTL::HeapTypePlacement);
			heap_desc->setMaxCompatiblePlacementSparsePageSize(sparse_page_size);
			NS::SharedPtr<MTL::Heap> heap = NS::TransferPtr(device->newHeap(heap_desc.get()));
			internal_state->buffer = NS::TransferPtr(heap->newBuffer(desc->size, resource_options, 0));
			internal_state->buffer->makeAliasable();
		}
		else if (alias != nullptr)
		{
			// This is an aliasing view:
			if (wiGraphicsGPUResourceIsBuffer(alias))
			{
				auto alias_internal = to_internal<GPUBuffer>(alias);
				internal_state->buffer = NS::TransferPtr(alias_internal->buffer->heap()->newBuffer(desc->size, resource_options, alias_internal->buffer->heapOffset() + alias_offset));
			}
			else if (wiGraphicsGPUResourceIsTexture(alias))
			{
				auto alias_internal = to_internal<Texture>(alias);
				internal_state->buffer = NS::TransferPtr(alias_internal->texture->heap()->newBuffer(desc->size, resource_options, alias_internal->texture->heapOffset() + alias_offset));
			}
		}
		else if (sparse)
		{
			// This is a placement sparse buffer:
			internal_state->buffer = NS::TransferPtr(device->newBuffer(desc->size, resource_options, sparse_page_size));
		}
		else
		{
			// This is a standalone buffer:
			internal_state->buffer = NS::TransferPtr(device->newBuffer(desc->size, resource_options));
		}
		
		allocationhandler->make_resident(internal_state->buffer.get());
		internal_state->gpu_address = internal_state->buffer->gpuAddress();
		if ((resource_options & MTL::ResourceStorageModePrivate) == 0)
		{
			buffer->mapped_data = internal_state->buffer->contents();
			buffer->mapped_size = internal_state->buffer->allocatedSize();
		}
		
		if (init_callback)
		{
			if (buffer->mapped_data == nullptr)
			{
				std::vector<uint8_t> init_data(desc->size);
				init_callback(init_data.data());
				UploadDescInternal upload = {};
				upload.type = UploadDescInternal::Type::BUFFER;
				upload.src_data = init_data.data();
				upload.src_size = desc->size;
				upload.dst_buffer = buffer;
				upload.dst_offset = 0;
				UploadAsyncInternal(upload, true);
			}
			else
			{
				init_callback(buffer->mapped_data);
			}
		}
		
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
		
		return internal_state->buffer.get() != nullptr;
	}
	bool GraphicsDevice_Metal::CreateTexture(const TextureDesc* desc, const SubresourceData* initial_data, Texture* texture, const GPUResource* alias, uint64_t alias_offset) const
	{
		auto internal_state = wi::allocator::make_shared<Texture_Metal>();
		internal_state->allocationhandler = allocationhandler;
		texture->internal_state = internal_state;
		texture->type = GPUResource::Type::TEXTURE;
		texture->mapped_data = nullptr;
		texture->mapped_size = 0;
		texture->mapped_subresources = nullptr;
		texture->mapped_subresource_count = 0;
		texture->sparse_properties = nullptr;
		texture->desc = *desc;
		
		METAL_ASSERT_MSG(!IsFormatBlockCompressed(desc->format) || device->supportsBCTextureCompression(), "Block compressed textures are not supported by this device!");
		
		const bool sparse = has_flag(desc->misc_flags, ResourceMiscFlag::SPARSE);
		const bool aliasing_storage = has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_BUFFER) || has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS) || has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_RT_DS);

		texture->desc.mip_levels = GetMipCount(texture->desc);
		
		NS::SharedPtr<MTL::TextureDescriptor> descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
		descriptor->setWidth(desc->width);
		descriptor->setHeight(desc->height);
		descriptor->setDepth(desc->depth);
		descriptor->setArrayLength(desc->array_size);
		descriptor->setMipmapLevelCount(texture->desc.mip_levels);
		descriptor->setPixelFormat(_ConvertPixelFormat(desc->format));
		
		uint32_t sample_count = desc->sample_count;
		while (sample_count > 1 && !device->supportsTextureSampleCount(sample_count))
		{
			sample_count /= 2;
		}
		descriptor->setSampleCount(sample_count);
		texture->desc.sample_count = sample_count;
		
		switch (desc->type)
		{
			case TextureDesc::Type::TEXTURE_1D:
				//descriptor->setTextureType(desc->array_size > 1 ? MTL::TextureType1DArray : MTL::TextureType1D);
				descriptor->setTextureType(desc->array_size > 1 ? MTL::TextureType2DArray : MTL::TextureType2D); // NOTE: This seems to be broken! Real Texture1D type doesn't work in shaders, but creating Texture2D instead works! Issue FB21629558
				break;
			case TextureDesc::Type::TEXTURE_2D:
				if(desc->sample_count > 1)
				{
					descriptor->setTextureType(desc->array_size > 1 ? MTL::TextureType2DMultisampleArray : MTL::TextureType2DMultisample);
				}
				else
				{
					descriptor->setTextureType(desc->array_size > 1 ? MTL::TextureType2DArray : MTL::TextureType2D);
				}
				break;
			case TextureDesc::Type::TEXTURE_3D:
				descriptor->setTextureType(MTL::TextureType3D);
				break;
			default:
				break;
		}
		if (has_flag(desc->misc_flags, ResourceMiscFlag::TEXTURECUBE))
		{
			descriptor->setTextureType(desc->array_size > 6 ? MTL::TextureTypeCubeArray : MTL::TextureTypeCube);
			descriptor->setArrayLength(desc->array_size / 6);
		}
		
		MTL::ResourceOptions resource_options = {};
		if (has_flag(desc->misc_flags, ResourceMiscFlag::TRANSIENT_ATTACHMENT))
		{
			resource_options |= MTL::ResourceStorageModeMemoryless;
			descriptor->setStorageMode(MTL::StorageModeMemoryless);
		}
		if (desc->usage == Usage::DEFAULT && !CheckCapability(GraphicsDeviceCapability::CACHE_COHERENT_UMA))
		{
			// Discrete GPU path:
			resource_options |= MTL::ResourceStorageModePrivate;
			descriptor->setStorageMode(MTL::StorageModePrivate);
		}
		else if (
				 sparse ||
				 has_flag(desc->bind_flags, BindFlag::RENDER_TARGET) ||
				 has_flag(desc->bind_flags, BindFlag::DEPTH_STENCIL) ||
				 has_flag(desc->bind_flags, BindFlag::BIND_UNORDERED_ACCESS)
				 )
		{
			// optimized storage for render efficiency even on UMA GPU:
			resource_options |= MTL::ResourceStorageModePrivate;
			descriptor->setStorageMode(MTL::StorageModePrivate);
		}
		else if (desc->usage == Usage::UPLOAD)
		{
			resource_options |= MTL::ResourceStorageModeShared;
			resource_options |= MTL::ResourceOptionCPUCacheModeWriteCombined;
			descriptor->setStorageMode(MTL::StorageModeShared);
			descriptor->setTextureType(MTL::TextureTypeTextureBuffer);
		}
		else if (desc->usage == Usage::READBACK)
		{
			resource_options |= MTL::ResourceStorageModeShared;
			descriptor->setStorageMode(MTL::StorageModeShared);
			descriptor->setTextureType(MTL::TextureTypeTextureBuffer);
		}
		else
		{
			// CPU accessible or UMA GPU zero-copy optimized:
			resource_options |= MTL::ResourceStorageModeShared;
			descriptor->setStorageMode(MTL::StorageModeShared);
		}
		descriptor->setResourceOptions(resource_options);
		
		MTL::TextureUsage usage = {};
		if (has_flag(desc->bind_flags, BindFlag::RENDER_TARGET) || has_flag(desc->bind_flags, BindFlag::DEPTH_STENCIL))
		{
			usage |= MTL::TextureUsageRenderTarget;
		}
		if (has_flag(desc->bind_flags, BindFlag::BIND_UNORDERED_ACCESS))
		{
			usage |= MTL::TextureUsageShaderWrite;
			usage |= MTL::TextureUsageRenderTarget; // support for ClearUAV
			switch (descriptor->pixelFormat())
			{
				case MTL::PixelFormatR32Uint:
				case MTL::PixelFormatR32Sint:
				case MTL::PixelFormatRG32Uint:
					usage |= MTL::TextureUsageShaderAtomic;
					break;
				default:
					break;
			}
		}
		if (has_flag(desc->bind_flags, BindFlag::BIND_SHADER_RESOURCE))
		{
			usage |= MTL::TextureUsageShaderRead;
		}
		descriptor->setUsage(usage);
		
		descriptor->setAllowGPUOptimizedContents(true);
		
		if (sparse)
		{
			descriptor->setPlacementSparsePageSize(sparse_page_size);
			texture->sparse_page_size = (uint32_t)device->sparseTileSizeInBytes(sparse_page_size);
			texture->sparse_properties = &internal_state->sparse_properties;
			const MTL::Size sparse_size = device->sparseTileSize(descriptor->textureType(), descriptor->pixelFormat(), descriptor->sampleCount(), sparse_page_size);
			internal_state->sparse_properties.tile_width = (uint32_t)sparse_size.width;
			internal_state->sparse_properties.tile_height = (uint32_t)sparse_size.height;
			internal_state->sparse_properties.tile_depth = (uint32_t)sparse_size.depth;
			const MTL::SizeAndAlign sizealign = device->heapTextureSizeAndAlign(descriptor.get());
			internal_state->sparse_properties.total_tile_count = uint32_t(sizealign.size / (uint64_t)texture->sparse_page_size);
		}
		
		if (!has_flag(desc->bind_flags, BindFlag::BIND_UNORDERED_ACCESS) && !has_flag(desc->bind_flags, BindFlag::RENDER_TARGET))
		{
			MTL::TextureSwizzleChannels swizzle = MTL::TextureSwizzleChannels::Default();
			swizzle.red = _ConvertComponentSwizzle(desc->swizzle.r);
			swizzle.green = _ConvertComponentSwizzle(desc->swizzle.g);
			swizzle.blue = _ConvertComponentSwizzle(desc->swizzle.b);
			swizzle.alpha = _ConvertComponentSwizzle(desc->swizzle.a);
			descriptor->setSwizzle(swizzle);
		}
		
		if (aliasing_storage)
		{
			// This is an aliasing storage:
			MTL::SizeAndAlign sizealign = device->heapTextureSizeAndAlign(descriptor.get());
			NS::SharedPtr<MTL::HeapDescriptor> heap_desc = NS::TransferPtr(MTL::HeapDescriptor::alloc()->init());
			heap_desc->setResourceOptions(resource_options);
			heap_desc->setSize(sizealign.size);
			heap_desc->setType(MTL::HeapTypePlacement);
			heap_desc->setMaxCompatiblePlacementSparsePageSize(sparse_page_size);
			NS::SharedPtr<MTL::Heap> heap = NS::TransferPtr(device->newHeap(heap_desc.get()));
			internal_state->texture = NS::TransferPtr(heap->newTexture(descriptor.get(), 0));
			internal_state->texture->makeAliasable();
		}
		else if (alias != nullptr)
		{
			// This is an aliasing view:
			if (wiGraphicsGPUResourceIsBuffer(alias))
			{
				auto alias_internal = to_internal<GPUBuffer>(alias);
				internal_state->texture = NS::TransferPtr(alias_internal->buffer->heap()->newTexture(descriptor.get(), alias_internal->buffer->heapOffset() + alias_offset));
			}
			else if (wiGraphicsGPUResourceIsTexture(alias))
			{
				auto alias_internal = to_internal<Texture>(alias);
				internal_state->texture = NS::TransferPtr(alias_internal->texture->heap()->newTexture(descriptor.get(), alias_internal->texture->heapOffset() + alias_offset));
			}
		}
		else
		{
			if (desc->usage == Usage::READBACK || desc->usage == Usage::UPLOAD)
			{
				// Note: we are creating a buffer instead of linear image because linear image cannot have mips
				//	With a buffer, we can tightly pack mips linearly into a buffer so it won't have that limitation
				const size_t buffersize = ComputeTextureMemorySizeInBytes(*desc);
				internal_state->buffer = NS::TransferPtr(device->newBuffer(buffersize, resource_options));
				allocationhandler->make_resident(internal_state->buffer.get());
				
				texture->mapped_data = internal_state->buffer->contents();
				texture->mapped_size = buffersize;
				
				const uint32_t bytes_per_block = GetFormatStride(desc->format);
				const uint32_t pixels_per_block = GetFormatBlockSize(desc->format);
				const uint32_t mips = GetMipCount(*desc);
				arrsetlen(internal_state->mapped_subresources, GetTextureSubresourceCount(*desc)); // stb_ds array: mapped texture subresource layout metadata.
				size_t subresource_index = 0;
				size_t subresource_data_offset = 0;
				for (uint32_t layer = 0; layer < desc->array_size; ++layer)
				{
					for (uint32_t mip = 0; mip < mips; ++mip)
					{
						const uint32_t mip_width = std::max(1u, desc->width >> mip);
						const uint32_t mip_height = std::max(1u, desc->height >> mip);
						const uint32_t mip_depth = std::max(1u, desc->depth >> mip);
						const uint32_t num_blocks_x = (mip_width + pixels_per_block - 1) / pixels_per_block;
						const uint32_t num_blocks_y = (mip_height + pixels_per_block - 1) / pixels_per_block;
						SubresourceData& subresource_data = internal_state->mapped_subresources[subresource_index++];
						subresource_data.data_ptr = (uint8_t*)texture->mapped_data + subresource_data_offset;
						subresource_data.row_pitch = num_blocks_x * bytes_per_block;
						subresource_data.row_pitch = align(subresource_data.row_pitch, 1u);
						subresource_data.slice_pitch = subresource_data.row_pitch * num_blocks_y;
						subresource_data_offset += subresource_data.slice_pitch * mip_depth;
					}
				}
				texture->mapped_subresources = internal_state->mapped_subresources;
				texture->mapped_subresource_count = (uint32_t)arrlenu(internal_state->mapped_subresources);
				SDL_assert(texture->mapped_subresources != nullptr);
			}
			else
			{
				internal_state->texture = NS::TransferPtr(device->newTexture(descriptor.get()));
			}
		}
		
		if (internal_state->texture.get() != nullptr)
		{
			allocationhandler->make_resident(internal_state->texture.get());
		}
		
		SDL_assert(internal_state->texture->isSparse() == sparse);
		
		if (initial_data != nullptr)
		{
			if (internal_state->buffer.get() != nullptr)
			{
				// readback or upload, linear memory:
				const uint32_t data_stride = GetFormatStride(desc->format);
				uint32_t initDataIdx = 0;
				uint64_t src_offset = 0;
				uint8_t* upload_data = (uint8_t*)internal_state->buffer->contents();
				for (uint32_t slice = 0; slice < desc->array_size; ++slice)
				{
					uint32_t width = desc->width;
					uint32_t height = desc->height;
					uint32_t depth = desc->depth;
					for (uint32_t mip = 0; mip < texture->desc.mip_levels; ++mip)
					{
						const SubresourceData& subresourceData = initial_data[initDataIdx++];
						const uint32_t block_size = GetFormatBlockSize(desc->format);
						const uint32_t num_blocks_x = std::max(1u, width / block_size);
						const uint32_t num_blocks_y = std::max(1u, height / block_size);
						const uint64_t datasize = data_stride * num_blocks_x * num_blocks_y * depth;
						std::memcpy(upload_data + src_offset, subresourceData.data_ptr, datasize);
						depth = std::max(1u, depth / 2);
						src_offset += datasize;
						width = std::max(block_size, width / 2);
						height = std::max(block_size, height / 2);
					}
				}
			}
			else if (descriptor->storageMode() == MTL::StorageModePrivate)
			{
				UploadDescInternal upload = {};
				upload.type = UploadDescInternal::Type::TEXTURE;
				upload.dst_texture = texture;
				upload.subresources = initial_data;
				upload.subresource_count = GetTextureSubresourceCount(texture->desc);
				UploadAsyncInternal(upload, true);
			}
			else
			{
				uint32_t initDataIdx = 0;
				for (uint32_t slice = 0; slice < desc->array_size; ++slice)
				{
					uint32_t width = desc->width;
					uint32_t height = desc->height;
					uint32_t depth = desc->depth;
					for (uint32_t mip = 0; mip < texture->desc.mip_levels; ++mip)
					{
						const SubresourceData& subresourceData = initial_data[initDataIdx++];
						const uint32_t block_size = GetFormatBlockSize(desc->format);
						const uint32_t num_blocks_x = std::max(1u, width / block_size);
						const uint32_t num_blocks_y = std::max(1u, height / block_size);
						const uint64_t datasize = GetFormatStride(desc->format) * num_blocks_x * num_blocks_y * depth;
						MTL::Region region = {};
						region.size.width = width;
						region.size.height = height;
						region.size.depth = depth;
						internal_state->texture->replaceRegion(region, mip, slice, subresourceData.data_ptr, subresourceData.row_pitch, datasize);
						depth = std::max(1u, depth / 2);
						width = std::max(block_size, width / 2);
						height = std::max(block_size, height / 2);
					}
				}
			}
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
		
		return internal_state->texture.get() != nullptr || internal_state->buffer.get() != nullptr;
	}
	bool GraphicsDevice_Metal::CreateShader(ShaderStage stage, const void* shadercode, size_t shadercode_size, Shader* shader, const char* entrypoint) const
	{
		auto internal_state = wi::allocator::make_shared<Shader_Metal>();
		internal_state->allocationhandler = allocationhandler;
		shader->internal_state = internal_state;
		
		// Offline reflection gathered data that's required to bring HLSL shaders to Metal is stored tightly after shadercode data:
		if (
			stage == ShaderStage::VS ||
			stage == ShaderStage::GS ||
			stage == ShaderStage::CS ||
			stage == ShaderStage::MS ||
			stage == ShaderStage::AS
			)
		{
			shadercode_size -= sizeof(internal_state->additional_data);
			std::memcpy(&internal_state->additional_data, (uint8_t*)shadercode + shadercode_size, sizeof(internal_state->additional_data));
		}
		
		dispatch_data_t bytecodeData = dispatch_data_create(shadercode, shadercode_size, dispatch_get_main_queue(), nullptr);
		NS::Error* error = nullptr;
		internal_state->library = NS::TransferPtr(device->newLibrary(bytecodeData, &error));
		if(error != nullptr)
		{
			NS::String* errDesc = error->localizedDescription();
			METAL_ASSERT_MSG(0, "%s", errDesc->utf8String());
			error->release();
		}
			SDL_assert(internal_state->library.get() != nullptr);
		
		NS::SharedPtr<NS::String> entry = NS::TransferPtr(NS::String::alloc()->init(entrypoint, NS::UTF8StringEncoding));
		NS::SharedPtr<MTL::FunctionConstantValues> constants = NS::TransferPtr(MTL::FunctionConstantValues::alloc()->init());
		
		if (stage == ShaderStage::HS || stage == ShaderStage::DS || stage == ShaderStage::GS)
		{
			// These will be used only by emulated pipeline creation, not native shader functions:
			return internal_state->library.get() != nullptr;
		}
		
		error = nullptr;
		internal_state->function = NS::TransferPtr(internal_state->library->newFunction(entry.get(), constants.get(), &error));
			if(error != nullptr)
			{
				NS::String* errDesc = error->localizedDescription();
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", errDesc->utf8String());
				error->release();
			}
			SDL_assert(internal_state->function.get() != nullptr);
		
			if (stage == ShaderStage::CS)
			{
				error = nullptr;
				internal_state->compute_pipeline = NS::TransferPtr(device->newComputePipelineState(internal_state->function.get(), &error));
				if (error != nullptr)
				{
					NS::String* errDesc = error->localizedDescription();
					SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", errDesc->utf8String());
					SDL_assert(0);
					error->release();
				}
			
			return internal_state->compute_pipeline.get() != nullptr;
		}

		return internal_state->function.get() != nullptr;
	}
	bool GraphicsDevice_Metal::CreateSampler(const SamplerDesc* desc, Sampler* sampler) const
	{
		auto internal_state = wi::allocator::make_shared<Sampler_Metal>();
		internal_state->allocationhandler = allocationhandler;
		sampler->internal_state = internal_state;
		sampler->desc = *desc;
		
		NS::SharedPtr<MTL::SamplerDescriptor> descriptor = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
		descriptor->setSupportArgumentBuffers(true);
		MTL::SamplerMipFilter mip_filter = {};
		switch (desc->filter)
		{
			case Filter::MIN_MAG_MIP_POINT:
			case Filter::MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MIN_MAG_LINEAR_MIP_POINT:
			case Filter::COMPARISON_MIN_MAG_MIP_POINT:
			case Filter::COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::COMPARISON_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::COMPARISON_MIN_MAG_LINEAR_MIP_POINT:
			case Filter::MINIMUM_MIN_MAG_MIP_POINT:
			case Filter::MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MINIMUM_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MINIMUM_MIN_MAG_LINEAR_MIP_POINT:
			case Filter::MAXIMUM_MIN_MAG_MIP_POINT:
			case Filter::MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MAXIMUM_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MAXIMUM_MIN_MAG_LINEAR_MIP_POINT:
				mip_filter = MTL::SamplerMipFilterNearest;
				break;
			default:
				mip_filter = MTL::SamplerMipFilterLinear;
				break;
		}
		descriptor->setMipFilter(mip_filter);
		MTL::SamplerMinMagFilter min_filter = {};
		switch (desc->filter)
		{
			case Filter::MIN_MAG_MIP_POINT:
			case Filter::MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MIN_POINT_MAG_MIP_LINEAR:
			case Filter::COMPARISON_MIN_MAG_MIP_POINT:
			case Filter::COMPARISON_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::COMPARISON_MIN_POINT_MAG_MIP_LINEAR:
			case Filter::MINIMUM_MIN_MAG_MIP_POINT:
			case Filter::MINIMUM_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MINIMUM_MIN_POINT_MAG_MIP_LINEAR:
			case Filter::MAXIMUM_MIN_MAG_MIP_POINT:
			case Filter::MAXIMUM_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:
			case Filter::MAXIMUM_MIN_POINT_MAG_MIP_LINEAR:
				min_filter = MTL::SamplerMinMagFilterNearest;
				break;
			default:
				min_filter = MTL::SamplerMinMagFilterLinear;
				break;
		}
		descriptor->setMinFilter(min_filter);
		MTL::SamplerMinMagFilter mag_filter = {};
		switch (desc->filter)
		{
			case Filter::MIN_MAG_MIP_POINT:
			case Filter::MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MIN_LINEAR_MAG_POINT_MIP_LINEAR:
			case Filter::COMPARISON_MIN_MAG_MIP_POINT:
			case Filter::COMPARISON_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::COMPARISON_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
			case Filter::MINIMUM_MIN_MAG_MIP_POINT:
			case Filter::MINIMUM_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MINIMUM_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
			case Filter::MAXIMUM_MIN_MAG_MIP_POINT:
			case Filter::MAXIMUM_MIN_MAG_POINT_MIP_LINEAR:
			case Filter::MAXIMUM_MIN_LINEAR_MAG_MIP_POINT:
			case Filter::MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
				mag_filter = MTL::SamplerMinMagFilterNearest;
				break;
			default:
				mag_filter = MTL::SamplerMinMagFilterLinear;
				break;
		}
		descriptor->setMagFilter(mag_filter);
		MTL::SamplerReductionMode reduction_mode = {};
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
				reduction_mode = MTL::SamplerReductionModeMinimum;
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
				reduction_mode = MTL::SamplerReductionModeMaximum;
				break;
			default:
				reduction_mode = MTL::SamplerReductionModeWeightedAverage;
				break;
		}
		descriptor->setReductionMode(reduction_mode);
		descriptor->setCompareFunction(_ConvertCompareFunction(desc->comparison_func));
		descriptor->setLodMinClamp(desc->min_lod);
		descriptor->setLodMaxClamp(desc->max_lod);
		descriptor->setMaxAnisotropy(SDL_clamp(desc->max_anisotropy, 1u, 16u));
		descriptor->setLodBias(desc->mip_lod_bias);
		MTL::SamplerBorderColor border_color = {};
		switch (desc->border_color)
		{
			case SamplerBorderColor::TRANSPARENT_BLACK:
				border_color = MTL::SamplerBorderColorTransparentBlack;
				break;
			case SamplerBorderColor::OPAQUE_BLACK:
				border_color = MTL::SamplerBorderColorOpaqueBlack;
				break;
			case SamplerBorderColor::OPAQUE_WHITE:
				border_color = MTL::SamplerBorderColorOpaqueWhite;
				break;
		}
		descriptor->setBorderColor(border_color);
		descriptor->setSAddressMode(_ConvertAddressMode(desc->address_u));
		descriptor->setTAddressMode(_ConvertAddressMode(desc->address_v));
		descriptor->setRAddressMode(_ConvertAddressMode(desc->address_w));
		internal_state->sampler = NS::TransferPtr(device->newSamplerState(descriptor.get()));
		internal_state->index = allocationhandler->allocate_sampler_index();
		internal_state->entry = create_entry(internal_state->sampler.get());
		std::memcpy(descriptor_heap_sam_data + internal_state->index, &internal_state->entry, sizeof(internal_state->entry));

		return internal_state->sampler.get() != nullptr;
	}
	bool GraphicsDevice_Metal::CreateQueryHeap(const GPUQueryHeapDesc* desc, GPUQueryHeap* queryheap) const
	{
		auto internal_state = wi::allocator::make_shared<QueryHeap_Metal>();
		internal_state->allocationhandler = allocationhandler;
		queryheap->internal_state = internal_state;
		queryheap->desc = *desc;
		
		switch (desc->type)
		{
			case GpuQueryType::OCCLUSION:
			case GpuQueryType::OCCLUSION_BINARY:
				internal_state->buffer = NS::TransferPtr(device->newBuffer(desc->query_count * sizeof(uint64_t), MTL::ResourceStorageModePrivate));
				break;
				
			case GpuQueryType::TIMESTAMP:
			{
				NS::SharedPtr<MTL4::CounterHeapDescriptor> descriptor = NS::TransferPtr(MTL4::CounterHeapDescriptor::alloc()->init());
				descriptor->setType(MTL4::CounterHeapTypeTimestamp);
				descriptor->setCount(desc->query_count);
				NS::Error* error = nullptr;
				internal_state->counter_heap = NS::TransferPtr(device->newCounterHeap(descriptor.get(), &error));
				if (error != nullptr)
				{
					NS::String* errDesc = error->localizedDescription();
					SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", errDesc->utf8String());
					SDL_assert(0);
					error->release();
				}
				internal_state->buffer = NS::TransferPtr(device->newBuffer(desc->query_count * sizeof(uint64_t), MTL::ResourceStorageModeShared));
			}
			break;
				
			default:
				break;
		}
		
		return internal_state->buffer.get() != nullptr;
	}
	bool GraphicsDevice_Metal::CreatePipelineState(const PipelineStateDesc* desc, PipelineState* pso, const RenderPassInfo* renderpass_info) const
	{
		auto internal_state = wi::allocator::make_shared<PipelineState_Metal>();
		internal_state->allocationhandler = allocationhandler;
		pso->internal_state = internal_state;
		pso->desc = *desc;

		DepthStencilState dss_default = {};
		BlendState bs_default = {};
		InitDepthStencilState(dss_default);
		InitBlendState(bs_default);
		const DepthStencilState& dss_desc = pso->desc.dss != nullptr ? *pso->desc.dss : dss_default;
		const BlendState& bs_desc = pso->desc.bs != nullptr ? *pso->desc.bs : bs_default;
		
		if (desc->vs != nullptr)
		{
			internal_state->descriptor = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
			auto shader_internal = to_internal(desc->vs);
			internal_state->descriptor->setVertexFunction(shader_internal->function.get());
			internal_state->needs_draw_params = shader_internal->additional_data.needs_draw_params;
			internal_state->gs_desc.vertexLibrary = shader_internal->library.get();
			internal_state->gs_desc.vertexFunctionName = "main";
			internal_state->gs_desc.stageInLibrary = shader_internal->library.get();
			internal_state->gs_desc.pipelineConfig.gsVertexSizeInBytes = shader_internal->additional_data.vertex_output_size_in_bytes;
		}
		if (desc->ds != nullptr)
		{
			return false; // TODO
		}
		if (desc->hs != nullptr)
		{
			return false; // TODO
		}
		if (desc->gs != nullptr)
		{
			internal_state->ms_descriptor = NS::TransferPtr(MTL::MeshRenderPipelineDescriptor::alloc()->init());
			auto shader_internal = to_internal(desc->gs);
			internal_state->gs_desc.basePipelineDescriptor = internal_state->ms_descriptor.get();
			internal_state->gs_desc.geometryLibrary = shader_internal->library.get();
			internal_state->gs_desc.geometryFunctionName = "main";
			internal_state->gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup = shader_internal->additional_data.max_input_primitives_per_mesh_threadgroup;
		}
		if (desc->ms != nullptr)
		{
			internal_state->ms_descriptor = NS::TransferPtr(MTL::MeshRenderPipelineDescriptor::alloc()->init());
			auto shader_internal = to_internal(desc->ms);
			internal_state->ms_descriptor->setMeshFunction(shader_internal->function.get());
			internal_state->numthreads_ms = shader_internal->additional_data.numthreads;
		}
		if (desc->as != nullptr)
		{
			auto shader_internal = to_internal(desc->as);
			internal_state->ms_descriptor->setObjectFunction(shader_internal->function.get());
			internal_state->numthreads_as = shader_internal->additional_data.numthreads;
		}
		if (desc->ps != nullptr)
		{
			auto shader_internal = to_internal(desc->ps);
			if (internal_state->descriptor.get() != nullptr)
				internal_state->descriptor->setFragmentFunction(shader_internal->function.get());
			else
				internal_state->ms_descriptor->setFragmentFunction(shader_internal->function.get());
			internal_state->gs_desc.fragmentLibrary = shader_internal->library.get();
			internal_state->gs_desc.fragmentFunctionName = "main";
		}
		
		NS::SharedPtr<MTL::VertexDescriptor> vertex_descriptor;
		if (desc->il != nullptr && internal_state->descriptor.get() != nullptr)
		{
			vertex_descriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
			uint64_t input_slot_strides[32] = {};
			for (size_t i = 0; i < arrlenu(desc->il->elements); ++i)
			{
				const InputLayout::Element& element = desc->il->elements[i];
				input_slot_strides[element.input_slot] += GetFormatStride(element.format);
			}
			uint64_t offset = 0;
			for (size_t i = 0; i < arrlenu(desc->il->elements); ++i)
			{
				const InputLayout::Element& element = desc->il->elements[i];
				const uint64_t element_stride = GetFormatStride(element.format);
				const uint64_t input_slot_stride = input_slot_strides[element.input_slot];
				MTL::VertexBufferLayoutDescriptor* layout = vertex_descriptor->layouts()->object(kIRVertexBufferBindPoint + i);
				layout->setStride(input_slot_stride);
				layout->setStepFunction(element.input_slot_class == InputClassification::PER_VERTEX_DATA ? MTL::VertexStepFunctionPerVertex : MTL::VertexStepFunctionPerInstance);
				layout->setStepRate(1);
				MTL::VertexAttributeDescriptor* attribute = vertex_descriptor->attributes()->object(kIRStageInAttributeStartIndex + i);
				attribute->setFormat(_ConvertVertexFormat(element.format));
				attribute->setOffset(element.aligned_byte_offset == InputLayout::APPEND_ALIGNED_ELEMENT ? offset : element.aligned_byte_offset);
				attribute->setBufferIndex(kIRVertexBufferBindPoint + i);
				if (element.aligned_byte_offset != InputLayout::APPEND_ALIGNED_ELEMENT)
				{
					offset = element.aligned_byte_offset;
				}
				offset += element_stride;
			}
			internal_state->descriptor->setVertexDescriptor(vertex_descriptor.get());
		}
		
		if (internal_state->descriptor.get() != nullptr)
		{
			switch (desc->pt)
			{
				case PrimitiveTopology::TRIANGLELIST:
				case PrimitiveTopology::TRIANGLESTRIP:
				case PrimitiveTopology::PATCHLIST:
					internal_state->descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
					break;
				case PrimitiveTopology::LINELIST:
				case PrimitiveTopology::LINESTRIP:
					internal_state->descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassLine);
					break;
				case PrimitiveTopology::POINTLIST:
					internal_state->descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassPoint);
					break;
				default:
					break;
			}
		}

		if (internal_state->descriptor.get() != nullptr)
		{
			internal_state->descriptor->setSupportIndirectCommandBuffers(true);
		}
		if (internal_state->ms_descriptor.get() != nullptr)
		{
			internal_state->ms_descriptor->setSupportIndirectCommandBuffers(true);
		}
		
		if (renderpass_info != nullptr)
		{
			// When renderpass_info is provided, it will be a completely precompiled pipeline state only useable by that renderpass type:
			NS::SharedPtr<MTL::RenderPipelineColorAttachmentDescriptor> attachments[8] = {
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
				NS::TransferPtr(MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init()),
			};
			if (internal_state->descriptor.get() != nullptr)
				internal_state->descriptor->setAlphaToCoverageEnabled(bs_desc.alpha_to_coverage_enable);
			else
				internal_state->ms_descriptor->setAlphaToCoverageEnabled(bs_desc.alpha_to_coverage_enable);
			for (uint32_t i = 0; i < renderpass_info->rt_count; ++i)
			{
				MTL::RenderPipelineColorAttachmentDescriptor& attachment = *attachments[i].get();
				attachment.setPixelFormat(_ConvertPixelFormat(renderpass_info->rt_formats[i]));
				
				const BlendState::RenderTargetBlendState& bs_rt = bs_desc.render_target[i];
				MTL::ColorWriteMask color_write_mask = {};
				if (has_flag(bs_rt.render_target_write_mask, ColorWrite::ENABLE_RED))
				{
					color_write_mask |= MTL::ColorWriteMaskRed;
				}
				if (has_flag(bs_rt.render_target_write_mask, ColorWrite::ENABLE_GREEN))
				{
					color_write_mask |= MTL::ColorWriteMaskGreen;
				}
				if (has_flag(bs_rt.render_target_write_mask, ColorWrite::ENABLE_BLUE))
				{
					color_write_mask |= MTL::ColorWriteMaskBlue;
				}
				if (has_flag(bs_rt.render_target_write_mask, ColorWrite::ENABLE_ALPHA))
				{
					color_write_mask |= MTL::ColorWriteMaskAlpha;
				}
				attachment.setWriteMask(color_write_mask);
				attachment.setBlendingEnabled(bs_rt.blend_enable);
				attachment.setRgbBlendOperation(_ConvertBlendOp(bs_rt.blend_op));
				attachment.setAlphaBlendOperation(_ConvertBlendOp(bs_rt.blend_op_alpha));
				attachment.setSourceRGBBlendFactor(_ConvertBlendFactor(bs_rt.src_blend));
				attachment.setSourceAlphaBlendFactor(_ConvertBlendFactor(bs_rt.src_blend_alpha));
				attachment.setDestinationRGBBlendFactor(_ConvertBlendFactor(bs_rt.dest_blend));
				attachment.setDestinationAlphaBlendFactor(_ConvertBlendFactor(bs_rt.dest_blend_alpha));
				if (internal_state->descriptor.get() != nullptr)
					internal_state->descriptor->colorAttachments()->setObject(&attachment, i);
				else
					internal_state->ms_descriptor->colorAttachments()->setObject(&attachment, i);
			}
			if (internal_state->descriptor.get() != nullptr)
			{
				internal_state->descriptor->setDepthAttachmentPixelFormat(_ConvertPixelFormat(renderpass_info->ds_format));
				if (IsFormatStencilSupport(renderpass_info->ds_format))
				{
					internal_state->descriptor->setStencilAttachmentPixelFormat(_ConvertPixelFormat(renderpass_info->ds_format));
				}
			}
			else
			{
				internal_state->ms_descriptor->setDepthAttachmentPixelFormat(_ConvertPixelFormat(renderpass_info->ds_format));
				if (IsFormatStencilSupport(renderpass_info->ds_format))
				{
					internal_state->ms_descriptor->setStencilAttachmentPixelFormat(_ConvertPixelFormat(renderpass_info->ds_format));
				}
			}
			
			uint32_t sample_count = renderpass_info->sample_count;
			while (sample_count > 1 && !device->supportsTextureSampleCount(sample_count))
			{
				sample_count /= 2;
			}
			if (internal_state->descriptor.get() != nullptr)
				internal_state->descriptor->setRasterSampleCount(sample_count);
			else
				internal_state->ms_descriptor->setRasterSampleCount(sample_count);
			
			NS::SharedPtr<MTL::DepthStencilDescriptor> depth_stencil_desc = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
			if (dss_desc.depth_enable && renderpass_info->ds_format != Format::UNKNOWN)
			{
				depth_stencil_desc->setDepthCompareFunction(_ConvertCompareFunction(dss_desc.depth_func));
				depth_stencil_desc->setDepthWriteEnabled(dss_desc.depth_write_mask == DepthWriteMask::ALL);
			}
			NS::SharedPtr<MTL::StencilDescriptor> stencil_front;
			NS::SharedPtr<MTL::StencilDescriptor> stencil_back;
			if (dss_desc.stencil_enable && IsFormatStencilSupport(renderpass_info->ds_format))
			{
				stencil_front = NS::TransferPtr(MTL::StencilDescriptor::alloc()->init());
				stencil_back = NS::TransferPtr(MTL::StencilDescriptor::alloc()->init());
				stencil_front->setReadMask(dss_desc.stencil_read_mask);
				stencil_front->setWriteMask(dss_desc.stencil_write_mask);
				stencil_front->setStencilCompareFunction(_ConvertCompareFunction(dss_desc.front_face.stencil_func));
				stencil_front->setStencilFailureOperation(_ConvertStencilOperation(dss_desc.front_face.stencil_fail_op));
				stencil_front->setDepthFailureOperation(_ConvertStencilOperation(dss_desc.front_face.stencil_depth_fail_op));
				stencil_front->setDepthStencilPassOperation(_ConvertStencilOperation(dss_desc.front_face.stencil_pass_op));
				stencil_back->setReadMask(dss_desc.stencil_read_mask);
				stencil_back->setWriteMask(dss_desc.stencil_write_mask);
				stencil_back->setStencilCompareFunction(_ConvertCompareFunction(dss_desc.back_face.stencil_func));
				stencil_back->setStencilFailureOperation(_ConvertStencilOperation(dss_desc.back_face.stencil_fail_op));
				stencil_back->setDepthFailureOperation(_ConvertStencilOperation(dss_desc.back_face.stencil_depth_fail_op));
				stencil_back->setDepthStencilPassOperation(_ConvertStencilOperation(dss_desc.back_face.stencil_pass_op));
				depth_stencil_desc->setFrontFaceStencil(stencil_front.get());
				depth_stencil_desc->setBackFaceStencil(stencil_back.get());
			}
			internal_state->depth_stencil_state = NS::TransferPtr(device->newDepthStencilState(depth_stencil_desc.get()));
			
			MTL::AutoreleasedRenderPipelineReflection* reflection = nullptr;
			NS::Error* error = nullptr;
			if (internal_state->gs_desc.basePipelineDescriptor != nullptr)
				internal_state->render_pipeline = NS::TransferPtr(IRRuntimeNewGeometryEmulationPipeline(device.get(), &internal_state->gs_desc, &error));
			else if (internal_state->descriptor.get() != nullptr)
				internal_state->render_pipeline = NS::TransferPtr(device->newRenderPipelineState(internal_state->descriptor.get(), &error));
			else
				internal_state->render_pipeline = NS::TransferPtr(device->newRenderPipelineState(internal_state->ms_descriptor.get(), MTL::PipelineOptionNone, reflection, &error));
			if (error != nullptr)
			{
				NS::String* errDesc = error->localizedDescription();
				METAL_ASSERT_MSG(0, "%s", errDesc->utf8String());
				error->release();
			}
			
			return internal_state->render_pipeline.get() != nullptr;
		}
		
		// If we get here, this pipeline state is not complete, but it will be reuseable by different render passes (and compiled just in time at runtime)
		return true;
	}

	static NS::SharedPtr<MTL4::AccelerationStructureDescriptor> mtl_acceleration_structure_descriptor(const RaytracingAccelerationStructureDesc* desc)
	{
		NS::SharedPtr<MTL4::AccelerationStructureDescriptor> descriptor;
		
		NS::SharedPtr<NS::Array> object_array;
			MTL4::AccelerationStructureGeometryDescriptor** geometry_descs = nullptr; // stb_ds array: transient geometry descriptor list.
		
		if (desc->type == RaytracingAccelerationStructureDesc::Type::TOPLEVEL)
		{
			NS::SharedPtr<MTL4::InstanceAccelerationStructureDescriptor> instance_descriptor = NS::TransferPtr(MTL4::InstanceAccelerationStructureDescriptor::alloc()->init());
			instance_descriptor->setInstanceCount(desc->top_level.count);
			auto buffer_internal = to_internal(&desc->top_level.instance_buffer);
			instance_descriptor->setInstanceDescriptorBuffer({buffer_internal->gpu_address + desc->top_level.offset, buffer_internal->buffer->length()});
			instance_descriptor->setInstanceDescriptorStride(sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor));
			instance_descriptor->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
			instance_descriptor->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutRowMajor);
			descriptor = instance_descriptor;
		}
		else
		{
			auto primitive_descriptor = NS::TransferPtr(MTL4::PrimitiveAccelerationStructureDescriptor::alloc()->init());
			
			for (size_t geometry_index = 0; geometry_index < arrlenu(desc->bottom_level.geometries); ++geometry_index)
			{
				auto& x = desc->bottom_level.geometries[geometry_index];
				if (x.type == RaytracingAccelerationStructureDesc::BottomLevel::Geometry::Type::TRIANGLES)
				{
					auto* geo = MTL4::AccelerationStructureTriangleGeometryDescriptor::alloc()->init();
					arrput(geometry_descs, geo);
					geo->setAllowDuplicateIntersectionFunctionInvocation((x.flags & RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_NO_DUPLICATE_ANYHIT_INVOCATION) == 0);
					geo->setOpaque(x.flags & RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_OPAQUE);
					auto ib_internal = to_internal(&x.triangles.index_buffer);
					auto vb_internal = to_internal(&x.triangles.vertex_buffer);
					geo->setIndexType(x.triangles.index_format == IndexBufferFormat::UINT32 ? MTL::IndexTypeUInt32 : MTL::IndexTypeUInt16);
					const uint64_t index_byteoffset = x.triangles.index_offset * GetIndexStride(x.triangles.index_format);
					geo->setIndexBuffer({ib_internal->gpu_address + index_byteoffset, ib_internal->buffer->length()});
					geo->setVertexFormat(_ConvertAttributeFormat(x.triangles.vertex_format));
					geo->setVertexBuffer({vb_internal->gpu_address + x.triangles.vertex_byte_offset, vb_internal->buffer->length()});
					geo->setVertexStride(x.triangles.vertex_stride);
					geo->setTriangleCount(x.triangles.index_count / 3);
					if (wiGraphicsGPUResourceIsValid(&x.triangles.transform_3x4_buffer))
					{
						auto transform_internal = to_internal(&x.triangles.transform_3x4_buffer);
						geo->setTransformationMatrixBuffer({transform_internal->gpu_address + x.triangles.transform_3x4_buffer_offset, transform_internal->buffer->length()});
						geo->setTransformationMatrixLayout(MTL::MatrixLayoutRowMajor);
					}
				}
				else
				{
					auto* geo = MTL4::AccelerationStructureBoundingBoxGeometryDescriptor::alloc()->init();
					arrput(geometry_descs, geo);
					geo->setOpaque(x.flags & RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_OPAQUE);
					geo->setBoundingBoxCount(x.aabbs.count);
					auto buffer_internal = to_internal(&x.aabbs.aabb_buffer);
					geo->setBoundingBoxBuffer({buffer_internal->gpu_address + x.aabbs.offset, buffer_internal->buffer->length()});
					geo->setBoundingBoxStride(x.aabbs.stride);
				}
			}
			
			object_array = NS::TransferPtr(NS::Array::array((NS::Object**)geometry_descs, arrlenu(geometry_descs))->retain());
			primitive_descriptor->setGeometryDescriptors(object_array.get());
			for (size_t i = 0; i < arrlenu(geometry_descs); ++i)
			{
				geometry_descs[i]->release();
			}
			arrfree(geometry_descs);
			descriptor = primitive_descriptor;
		}
		
		MTL::AccelerationStructureUsage usage = MTL::AccelerationStructureUsageNone;
		if (desc->flags & RaytracingAccelerationStructureDesc::FLAG_ALLOW_UPDATE)
		{
			usage |= MTL::AccelerationStructureUsageRefit;
		}
		if (desc->flags & RaytracingAccelerationStructureDesc::FLAG_PREFER_FAST_BUILD)
		{
			usage |= MTL::AccelerationStructureUsagePreferFastBuild;
		}
		if (desc->flags & RaytracingAccelerationStructureDesc::FLAG_PREFER_FAST_TRACE)
		{
			usage |= MTL::AccelerationStructureUsagePreferFastIntersection;
		}
		if (desc->flags & RaytracingAccelerationStructureDesc::FLAG_MINIMIZE_MEMORY)
		{
			usage |= MTL::AccelerationStructureUsageMinimizeMemory;
		}
		descriptor->setUsage(usage);
		
		return descriptor;
	}
	bool GraphicsDevice_Metal::CreateRaytracingAccelerationStructure(const RaytracingAccelerationStructureDesc* desc, RaytracingAccelerationStructure* bvh) const
	{
		auto internal_state = wi::allocator::make_shared<BVH_Metal>();
		internal_state->allocationhandler = allocationhandler;
		bvh->internal_state = internal_state;
		bvh->type = GPUResource::Type::RAYTRACING_ACCELERATION_STRUCTURE;
		::wi::CloneRaytracingAccelerationStructureDesc(bvh->desc, *desc);
		bvh->size = 0;
		
		NS::SharedPtr<MTL4::AccelerationStructureDescriptor> descriptor = mtl_acceleration_structure_descriptor(desc);
		if (descriptor.get() == nullptr)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Metal acceleration structure descriptor creation failed");
			SDL_assert(descriptor.get() != nullptr);
			return false;
		}
		
		const MTL::AccelerationStructureSizes size = device->accelerationStructureSizes(descriptor.get());
		internal_state->acceleration_structure = NS::TransferPtr(device->newAccelerationStructure(size.accelerationStructureSize));
		internal_state->scratch = NS::TransferPtr(device->newBuffer(std::max(size.buildScratchBufferSize, size.refitScratchBufferSize), MTL::ResourceStorageModePrivate));

		allocationhandler->make_resident(internal_state->acceleration_structure.get());
		allocationhandler->make_resident(internal_state->scratch.get());
		
		internal_state->resourceid = internal_state->acceleration_structure->gpuResourceID();
		
		if (desc->type == RaytracingAccelerationStructureDesc::Type::TOPLEVEL)
		{
			internal_state->tlas_header_instancecontributions = NS::TransferPtr(device->newBuffer(sizeof(IRRaytracingAccelerationStructureGPUHeader) + sizeof(uint32_t) * desc->top_level.count, MTL::ResourceStorageModeShared));
			allocationhandler->make_resident(internal_state->tlas_header_instancecontributions.get());
				uint32_t* instance_contributions = nullptr; // stb_ds array: TLAS instance contribution table.
			arrsetlen(instance_contributions, desc->top_level.count);
			std::memset(instance_contributions, 0, sizeof(uint32_t) * desc->top_level.count);
			IRRaytracingAccelerationStructureGPUHeader* header = (IRRaytracingAccelerationStructureGPUHeader*)internal_state->tlas_header_instancecontributions->contents();
			uint8_t* instancecontributions_gpudata = (uint8_t*)header + sizeof(IRRaytracingAccelerationStructureGPUHeader);
			MTL::GPUAddress header_gpuaddress = internal_state->tlas_header_instancecontributions->gpuAddress();
			MTL::GPUAddress instancecontributions_gpuaddress = header_gpuaddress + sizeof(IRRaytracingAccelerationStructureGPUHeader);
			IRRaytracingSetAccelerationStructure((uint8_t*)header, internal_state->resourceid, instancecontributions_gpudata, instancecontributions_gpuaddress, instance_contributions, desc->top_level.count);
			IRDescriptorTableSetAccelerationStructure(&internal_state->tlas_entry, header_gpuaddress);
			internal_state->tlas_descriptor_index = allocationhandler->allocate_resource_index();
			std::memcpy(descriptor_heap_res_data + internal_state->tlas_descriptor_index, &internal_state->tlas_entry, sizeof(internal_state->tlas_entry));
			arrfree(instance_contributions);
		}
		
		return internal_state->acceleration_structure.get() != nullptr;
	}
	bool GraphicsDevice_Metal::CreateRaytracingPipelineState(const RaytracingPipelineStateDesc* desc, RaytracingPipelineState* rtpso) const
	{
		// TODO
		return false;
	}
	bool GraphicsDevice_Metal::CreateVideoDecoder(const VideoDesc* desc, VideoDecoder* video_decoder) const
	{
		// TODO
		return false;
	}

	int GraphicsDevice_Metal::CreateSubresource(Texture* texture, SubresourceType type, uint32_t firstSlice, uint32_t sliceCount, uint32_t firstMip, uint32_t mipCount, const Format* format_change, const ImageAspect* aspect, const Swizzle* swizzle, float min_lod_clamp) const
	{
		auto internal_state = to_internal(texture);

		Format format = wiGraphicsTextureGetDesc(texture)->format;
		if (format_change != nullptr)
		{
			format = *format_change;
		}
		
		const MTL::PixelFormat pixelformat = _ConvertPixelFormat(format);
		SDL_assert(pixelformat != MTL::PixelFormatInvalid);
		
		sliceCount = std::min(sliceCount, texture->desc.array_size - firstSlice);
		mipCount = std::min(mipCount, texture->desc.mip_levels - firstMip);
		
		MTL::TextureType texture_type = internal_state->texture->textureType();
		if (type != SubresourceType::SRV &&
			(texture_type == MTL::TextureTypeCube || texture_type == MTL::TextureTypeCubeArray)
			)
		{
			texture_type = MTL::TextureType2DArray;
		}
		
		NS::SharedPtr<MTL::TextureViewDescriptor> view_desc;
		
		switch (type) {
			case SubresourceType::SRV:
			case SubresourceType::UAV:
				view_desc = NS::TransferPtr(MTL::TextureViewDescriptor::alloc()->init());
				view_desc->setLevelRange({firstMip, mipCount});
				view_desc->setSliceRange({firstSlice, sliceCount});
				view_desc->setPixelFormat(pixelformat);
				view_desc->setTextureType(texture_type);
				break;
			default:
				break;
		};
		
		switch (type) {
			case SubresourceType::SRV:
				{
					MTL::TextureSwizzleChannels mtlswizzle = MTL::TextureSwizzleChannels::Default();
					Swizzle requested_swizzle = texture->desc.swizzle;
					if (swizzle != nullptr)
					{
						requested_swizzle = *swizzle;
					}
					mtlswizzle.red = _ConvertComponentSwizzle(requested_swizzle.r);
					mtlswizzle.green = _ConvertComponentSwizzle(requested_swizzle.g);
					mtlswizzle.blue = _ConvertComponentSwizzle(requested_swizzle.b);
					mtlswizzle.alpha = _ConvertComponentSwizzle(requested_swizzle.a);
					view_desc->setSwizzle(mtlswizzle);
					if (!internal_state->srv.IsValid())
					{
						auto& subresource = internal_state->srv;
						subresource.index = allocationhandler->allocate_resource_index();
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = texture_view_pool->setTextureView(internal_state->texture.get(), view_desc.get(), subresource.index);
						subresource.entry = create_entry(resourceID, min_lod_clamp);
#else
						subresource.view = internal_state->texture->newTextureView(view_desc.get());
						subresource.entry = create_entry(subresource.view);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return -1;
					}
					else
					{
						auto& subresource = arrput(internal_state->subresources_srv, {});
						subresource.index = allocationhandler->allocate_resource_index();
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = texture_view_pool->setTextureView(internal_state->texture.get(), view_desc.get(), subresource.index);
						subresource.entry = create_entry(resourceID, min_lod_clamp);
#else
						subresource.view = internal_state->texture->newTextureView(view_desc.get());
						subresource.entry = create_entry(subresource.view);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return (int)arrlenu(internal_state->subresources_srv) - 1;
					}
				}
				break;
				
			case SubresourceType::UAV:
				{
					if (!internal_state->uav.IsValid())
					{
						auto& subresource = internal_state->uav;
						subresource.index = allocationhandler->allocate_resource_index();
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = texture_view_pool->setTextureView(internal_state->texture.get(), view_desc.get(), subresource.index);
						subresource.entry = create_entry(resourceID, min_lod_clamp);
#else
						subresource.view = internal_state->texture->newTextureView(view_desc.get());
						subresource.entry = create_entry(subresource.view);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return -1;
					}
					else
					{
						auto& subresource = arrput(internal_state->subresources_uav, {});
						subresource.index = allocationhandler->allocate_resource_index();
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = texture_view_pool->setTextureView(internal_state->texture.get(), view_desc.get(), subresource.index);
						subresource.entry = create_entry(resourceID, min_lod_clamp);
#else
						subresource.view = internal_state->texture->newTextureView(view_desc.get());
						subresource.entry = create_entry(subresource.view);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return (int)arrlenu(internal_state->subresources_uav) - 1;
					}
				}
				break;
				
			case SubresourceType::RTV:
				{
					if (!internal_state->rtv.IsValid())
					{
						auto& subresource = internal_state->rtv;
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return -1;
					}
					else
					{
						auto& subresource = arrput(internal_state->subresources_rtv, {});
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return (int)arrlenu(internal_state->subresources_rtv) - 1;
					}
				}
				break;
				
			case SubresourceType::DSV:
				{
					if (!internal_state->dsv.IsValid())
					{
						auto& subresource = internal_state->dsv;
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return -1;
					}
					else
					{
						auto& subresource = arrput(internal_state->subresources_dsv, {});
						subresource.firstMip = firstMip;
						subresource.mipCount = mipCount;
						subresource.firstSlice = firstSlice;
						subresource.sliceCount = sliceCount;
						return (int)arrlenu(internal_state->subresources_dsv) - 1;
					}
				}
				break;
				
			default:
				break;
		}

		return -1;
	}
	int GraphicsDevice_Metal::CreateSubresource(GPUBuffer* buffer, SubresourceType type, uint64_t offset, uint64_t size, const Format* format_change, const uint32_t* structuredbuffer_stride_change) const
	{
		auto internal_state = to_internal(buffer);
		NS::SharedPtr<MTL::TextureDescriptor> texture_descriptor;
		
		size = std::min(size, buffer->desc.size - offset);
		
		Format format = wiGraphicsGPUBufferGetDesc(buffer)->format;
		if (format_change != nullptr)
		{
			format = *format_change;
		}
		
		const bool structured = has_flag(buffer->desc.misc_flags, ResourceMiscFlag::BUFFER_STRUCTURED) || (structuredbuffer_stride_change != nullptr);
		
		const MTL::PixelFormat pixelformat = _ConvertPixelFormat(format);
		if (pixelformat != MTL::PixelFormatInvalid)
		{
			texture_descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
			texture_descriptor->setPixelFormat(pixelformat);
			texture_descriptor->setTextureType(MTL::TextureTypeTextureBuffer);
			texture_descriptor->setResourceOptions(internal_state->buffer->resourceOptions());
			texture_descriptor->setWidth(size / GetFormatStride(format));
			MTL::TextureUsage texture_usage = MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView;
			if (type == SubresourceType::UAV)
			{
				texture_usage |= MTL::TextureUsageShaderWrite;
				switch (texture_descriptor->pixelFormat())
				{
					case MTL::PixelFormatR32Uint:
					case MTL::PixelFormatR32Sint:
					case MTL::PixelFormatRG32Uint:
						texture_usage |= MTL::TextureUsageShaderAtomic;
						break;
					default:
						break;
				}
			}
			texture_descriptor->setUsage(texture_usage);
		}
		
		switch (type) {
			case SubresourceType::SRV:
				{
					if (!internal_state->srv.IsValid())
					{
						auto& subresource = internal_state->srv;
						subresource.index = allocationhandler->allocate_resource_index();
						subresource.offset = offset;
						subresource.size = size;
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = {};
						if (texture_descriptor.get() != nullptr)
						{
							resourceID = texture_view_pool->setTextureViewFromBuffer(internal_state->buffer.get(), texture_descriptor.get(), offset, size, subresource.index);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, resourceID, format, structured);
#else
						if (texture_descriptor.get() != nullptr)
						{
							subresource.view = internal_state->buffer->newTexture(texture_descriptor.get(), offset, size);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, subresource.view, format, structured);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						return -1;
					}
					else
					{
						auto& subresource = arrput(internal_state->subresources_srv, {});
						subresource.index = allocationhandler->allocate_resource_index();
						subresource.offset = offset;
						subresource.size = size;
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = {};
						if (texture_descriptor.get() != nullptr)
						{
							resourceID = texture_view_pool->setTextureViewFromBuffer(internal_state->buffer.get(), texture_descriptor.get(), offset, size, subresource.index);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, resourceID, format, structured);
#else
						if (texture_descriptor.get() != nullptr)
						{
							subresource.view = internal_state->buffer->newTexture(texture_descriptor.get(), offset, size);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, subresource.view, format, structured);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						return (int)arrlenu(internal_state->subresources_srv) - 1;
					}
				}
				break;
			case SubresourceType::UAV:
				{
					if (!internal_state->uav.IsValid())
					{
						auto& subresource = internal_state->uav;
						subresource.index = allocationhandler->allocate_resource_index();
						subresource.offset = offset;
						subresource.size = size;
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = {};
						if (texture_descriptor.get() != nullptr)
						{
							resourceID = texture_view_pool->setTextureViewFromBuffer(internal_state->buffer.get(), texture_descriptor.get(), offset, size, subresource.index);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, resourceID, format, structured);
#else
						if (texture_descriptor.get() != nullptr)
						{
							subresource.view = internal_state->buffer->newTexture(texture_descriptor.get(), offset, size);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, subresource.view, format, structured);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						return -1;
					}
					else
					{
						auto& subresource = arrput(internal_state->subresources_uav, {});
						subresource.index = allocationhandler->allocate_resource_index();
						subresource.offset = offset;
						subresource.size = size;
#ifdef USE_TEXTURE_VIEW_POOL
						MTL::ResourceID resourceID = {};
						if (texture_descriptor.get() != nullptr)
						{
							resourceID = texture_view_pool->setTextureViewFromBuffer(internal_state->buffer.get(), texture_descriptor.get(), offset, size, subresource.index);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, resourceID, format, structured);
#else
						if (texture_descriptor.get() != nullptr)
						{
							subresource.view = internal_state->buffer->newTexture(texture_descriptor.get(), offset, size);
						}
						subresource.entry = create_entry(internal_state->buffer.get(), size, offset, subresource.view, format, structured);
#endif // USE_TEXTURE_VIEW_POOL
						std::memcpy(descriptor_heap_res_data + subresource.index, &subresource.entry, sizeof(subresource.entry));
						return (int)arrlenu(internal_state->subresources_uav) - 1;
					}
				}
				break;
			default:
				break;
		}
		
		return -1;
	}

	void GraphicsDevice_Metal::DeleteSubresources(GPUResource* resource)
	{
		std::scoped_lock lck(allocationhandler->destroylocker);
		if (wiGraphicsGPUResourceIsTexture(resource))
		{
			auto internal_state = to_internal<Texture>(resource);
			internal_state->destroy_subresources();
		}
		else if (wiGraphicsGPUResourceIsBuffer(resource))
		{
			auto internal_state = to_internal<GPUBuffer>(resource);
			internal_state->destroy_subresources();
		}
	}

	int GraphicsDevice_Metal::GetDescriptorIndex(const GPUResource* resource, SubresourceType type, int subresource) const
	{
		if (!wiGraphicsGPUResourceIsValid(resource))
			return -1;

		if (wiGraphicsGPUResourceIsTexture(resource))
		{
			auto internal_state = to_internal<Texture>(resource);
			switch (type)
			{
				case SubresourceType::SRV:
					return subresource < 0 ? internal_state->srv.index : internal_state->subresources_srv[subresource].index;
				case SubresourceType::UAV:
					return subresource < 0 ? internal_state->uav.index : internal_state->subresources_uav[subresource].index;
				default:
					break;
			}
		}
		else if (wiGraphicsGPUResourceIsBuffer(resource))
		{
			auto internal_state = to_internal<GPUBuffer>(resource);
			switch (type)
			{
				case SubresourceType::SRV:
					return subresource < 0 ? internal_state->srv.index : internal_state->subresources_srv[subresource].index;
				case SubresourceType::UAV:
					return subresource < 0 ? internal_state->uav.index : internal_state->subresources_uav[subresource].index;
				default:
					break;
			}
		}
		else if (wiGraphicsGPUResourceIsAccelerationStructure(resource))
		{
			auto internal_state = to_internal<RaytracingAccelerationStructure>(resource);
			return internal_state->tlas_descriptor_index;
		}
		return -1;
	}
	int GraphicsDevice_Metal::GetDescriptorIndex(const Sampler* sampler) const
	{
		if (!wiGraphicsSamplerIsValid(sampler))
			return -1;
		
		auto internal_state = to_internal(sampler);
		return internal_state->index;
	}

	void GraphicsDevice_Metal::WriteShadingRateValue(ShadingRate rate, void* dest) const
	{
		// TODO
	}
	void GraphicsDevice_Metal::WriteTopLevelAccelerationStructureInstance(const RaytracingAccelerationStructureDesc::TopLevel::Instance* instance, void* dest) const
	{
		MTL::IndirectAccelerationStructureInstanceDescriptor descriptor = {};
		if (instance != nullptr)
		{
			descriptor.options = MTL::AccelerationStructureInstanceOptionNone;
			if (instance->flags & RaytracingAccelerationStructureDesc::TopLevel::Instance::FLAG_TRIANGLE_CULL_DISABLE)
			{
				descriptor.options |= MTL::AccelerationStructureInstanceOptionDisableTriangleCulling;
			}
			if (instance->flags & RaytracingAccelerationStructureDesc::TopLevel::Instance::FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE)
			{
				descriptor.options |= MTL::AccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise;
			}
			if (instance->flags & RaytracingAccelerationStructureDesc::TopLevel::Instance::FLAG_FORCE_OPAQUE)
			{
				descriptor.options |= MTL::AccelerationStructureInstanceOptionOpaque;
			}
			if (instance->flags & RaytracingAccelerationStructureDesc::TopLevel::Instance::FLAG_FORCE_NON_OPAQUE)
			{
				descriptor.options |= MTL::AccelerationStructureInstanceOptionNonOpaque;
			}
			descriptor.mask = instance->instance_mask;
			descriptor.userID = instance->instance_id;
			descriptor.intersectionFunctionTableOffset = 0;
			auto blas_internal = to_internal<RaytracingAccelerationStructure>(instance->bottom_level);
			descriptor.accelerationStructureID = blas_internal->resourceid;
			std::memcpy(&descriptor.transformationMatrix, instance->transform, sizeof(descriptor.transformationMatrix)); // MatrixLayoutRowMajor instance setup!
		}
		std::memcpy(dest, &descriptor, sizeof(descriptor)); // force memcpy into potentially write combined cache memory
	}
	void GraphicsDevice_Metal::WriteShaderIdentifier(const RaytracingPipelineState* rtpso, uint32_t group_index, void* dest) const
	{
		// TODO
	}

	void GraphicsDevice_Metal::SetName(GPUResource* pResource, const char* name) const
	{
		if (!wiGraphicsGPUResourceIsValid(pResource))
			return;
		NS::SharedPtr<NS::String> str = NS::TransferPtr(NS::String::alloc()->init(name, NS::UTF8StringEncoding));
		if (wiGraphicsGPUResourceIsTexture(pResource))
		{
			auto internal_state = to_internal<Texture>(pResource);
			internal_state->texture->setLabel(str.get());
		}
		else if (wiGraphicsGPUResourceIsBuffer(pResource))
		{
			auto internal_state = to_internal<GPUBuffer>(pResource);
			internal_state->buffer->setLabel(str.get());
		}
		else if (wiGraphicsGPUResourceIsAccelerationStructure(pResource))
		{
			auto internal_state = to_internal<RaytracingAccelerationStructure>(pResource);
			internal_state->acceleration_structure->setLabel(str.get());
		}
	}
	void GraphicsDevice_Metal::SetName(Shader* shader, const char* name) const
	{
		if (!wiGraphicsShaderIsValid(shader))
			return;
		NS::SharedPtr<NS::String> str = NS::TransferPtr(NS::String::alloc()->init(name, NS::UTF8StringEncoding));
		auto internal_state = to_internal(shader);
		internal_state->library->setLabel(str.get());
	}

	CommandList GraphicsDevice_Metal::BeginCommandList(QUEUE_TYPE queue)
	{
		cmd_locker.lock();
		CommandList_Metal* commandlist_ptr = nullptr;
		for (size_t i = 0; i < arrlenu(retired_contexts[queue]); ++i)
		{
			RetiredCommandContext retired = retired_contexts[queue][i];
			if (retired.context != nullptr && IsQueuePointComplete(retired.retire_after))
			{
				commandlist_ptr = retired.context;
				retired_contexts[queue][i] = arrlast(retired_contexts[queue]);
				arrpop(retired_contexts[queue]);
				break;
			}
		}
		if (commandlist_ptr == nullptr)
		{
			commandlist_ptr = cmd_allocator.allocate();
			arrput(commandlists, commandlist_ptr);
			commandlist_ptr->commandbuffer = NS::TransferPtr(device->newCommandBuffer());
			for (auto& x : commandlist_ptr->commandallocators)
			{
				x = NS::TransferPtr(device->newCommandAllocator());
			}
			NS::Error* error = nullptr;
			commandlist_ptr->argument_table = NS::TransferPtr(device->newArgumentTable(argument_table_desc.get(), &error));
			if (error != nullptr)
			{
				NS::String* errDesc = error->localizedDescription();
				METAL_ASSERT_MSG(0, "%s", errDesc->utf8String());
				error->release();
			}
		}
		CommandList cmd;
		cmd.internal_state = commandlist_ptr;
		cmd_locker.unlock();

		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.reset(GetBufferIndex());
		commandlist.queue = queue;
		commandlist.id = (uint32_t)arrlenu(open_commandlists);
		arrput(open_commandlists, commandlist_ptr);
		commandlist.commandallocators[GetBufferIndex()]->reset();
		commandlist.commandbuffer->beginCommandBuffer(commandlist.commandallocators[GetBufferIndex()].get());
		commandlist.argument_table->setAddress(descriptor_heap_res->gpuAddress(), kIRDescriptorHeapBindPoint);
		commandlist.argument_table->setAddress(descriptor_heap_sam->gpuAddress(), kIRSamplerHeapBindPoint);
		
		return cmd;
	}
	SubmissionToken GraphicsDevice_Metal::allocate_submission_token(QUEUE_TYPE queue) const
	{
		if (queue >= QUEUE_COUNT)
			queue = QUEUE_COPY;
		std::scoped_lock lock(submission_token_locker);
		SubmissionToken token = {};
		token.Merge(QueueSyncPoint{ queue, ++submission_token_values[queue] });
		return token;
	}

	void GraphicsDevice_Metal::retire_completed_uploads() const
	{
		std::scoped_lock upload_lock(upload_locker);
		bool residency_dirty = false;
		for (size_t i = 0; i < inflight_uploads.size();)
		{
			if (!IsUploadComplete(inflight_uploads[i].ticket))
			{
				++i;
				continue;
			}

			if (inflight_uploads[i].staging_buffer.get() != nullptr)
			{
				std::scoped_lock residency_lock(allocationhandler->destroylocker);
				allocationhandler->residency_set->removeAllocation(inflight_uploads[i].staging_buffer.get());
				residency_dirty = true;
			}
			inflight_uploads.erase(inflight_uploads.begin() + i);
		}
		if (residency_dirty)
		{
			std::scoped_lock residency_lock(allocationhandler->destroylocker);
			allocationhandler->residency_set->commit();
		}
	}

	SubmissionToken GraphicsDevice_Metal::consume_pending_implicit_uploads() const
	{
		std::scoped_lock lock(upload_locker);
		SubmissionToken result = pending_implicit_uploads;
		pending_implicit_uploads = {};
		return result;
	}

	UploadTicket GraphicsDevice_Metal::UploadAsyncInternal(const UploadDescInternal& upload, bool implicit_dependency) const
	{
		retire_completed_uploads();

		UploadTicket ticket = {};
		if (upload.src_data == nullptr || upload.src_size == 0)
			return ticket;

		// Metal uploads use the dedicated upload queue, which maps to copy queue token stream.
		const QUEUE_TYPE token_queue = QUEUE_COPY;

		switch (upload.type)
		{
		case UploadDescInternal::Type::BUFFER:
		{
			if (upload.dst_buffer == nullptr)
				return ticket;
			auto internal_state = to_internal(upload.dst_buffer);
			if (internal_state == nullptr || internal_state->buffer.get() == nullptr)
				return ticket;

			if (upload.dst_buffer->mapped_data != nullptr)
			{
				std::memcpy((uint8_t*)upload.dst_buffer->mapped_data + upload.dst_offset, upload.src_data, (size_t)upload.src_size);
				return ticket;
			}

			NS::SharedPtr<NS::AutoreleasePool> autorelease_pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
			NS::SharedPtr<MTL::Buffer> staging = NS::TransferPtr(device->newBuffer(upload.src_size, MTL::ResourceStorageModeShared | MTL::ResourceOptionCPUCacheModeWriteCombined));
			if (staging.get() == nullptr)
				return ticket;
			std::memcpy(staging->contents(), upload.src_data, (size_t)upload.src_size);

			NS::SharedPtr<MTL4::CommandAllocator> commandallocator = NS::TransferPtr(device->newCommandAllocator());
			NS::SharedPtr<MTL4::CommandBuffer> commandbuffer = NS::TransferPtr(device->newCommandBuffer());
			commandbuffer->beginCommandBuffer(commandallocator.get());

			{
				std::scoped_lock residency_lock(allocationhandler->destroylocker);
				allocationhandler->residency_set->addAllocation(staging.get());
				allocationhandler->residency_set->commit();
			}

			MTL4::ComputeCommandEncoder* encoder = commandbuffer->computeCommandEncoder();
			encoder->copyFromBuffer(staging.get(), 0, internal_state->buffer.get(), upload.dst_offset, upload.src_size);
			encoder->endEncoding();
			commandbuffer->endCommandBuffer();

			ticket.token = allocate_submission_token(token_queue);
			ticket.completion = ticket.token.Get(token_queue);
			MTL4::CommandBuffer* cmds[] = { commandbuffer.get() };
			uploadqueue->commit(cmds, SDL_arraysize(cmds));
			if (ticket.completion.IsValid() && submission_token_events[token_queue].get() != nullptr)
			{
				uploadqueue->signalEvent(submission_token_events[token_queue].get(), ticket.completion.value);
			}

			UploadJob job = {};
			job.ticket = ticket;
			job.staging_buffer = staging;
			job.commandbuffer = commandbuffer;
			job.commandallocator = commandallocator;

			{
				std::scoped_lock lock(upload_locker);
				inflight_uploads.emplace_back(std::move(job));
					if (implicit_dependency)
					{
						pending_implicit_uploads.Merge(ticket.token);
					}
				}
				return ticket;
		}
		case UploadDescInternal::Type::TEXTURE:
		{
			if (upload.dst_texture == nullptr || upload.subresources == nullptr)
				return ticket;
			auto internal_state = to_internal(upload.dst_texture);
			if (internal_state == nullptr)
				return ticket;

			const uint32_t subresource_count = upload.subresource_count == 0 ? GetTextureSubresourceCount(upload.dst_texture->desc) : upload.subresource_count;
			if (subresource_count == 0)
				return ticket;

			if (internal_state->buffer.get() != nullptr)
			{
				// Linear readback/upload textures are CPU-visible and don't require GPU copy.
				const uint32_t data_stride = GetFormatStride(upload.dst_texture->desc.format);
				uint64_t src_offset = 0;
				for (uint32_t i = 0; i < subresource_count; ++i)
				{
					const SubresourceData& subresource = upload.subresources[i];
					const uint64_t datasize = std::min<uint64_t>(subresource.slice_pitch, data_stride * subresource.row_pitch);
					std::memcpy((uint8_t*)upload.dst_texture->mapped_subresources[i].data_ptr, subresource.data_ptr, (size_t)datasize);
					src_offset += datasize;
				}
				(void)src_offset;
				return ticket;
			}

			if (internal_state->texture.get() == nullptr || internal_state->texture->storageMode() != MTL::StorageModePrivate)
			{
				// Shared texture: direct CPU upload path.
				uint32_t initDataIdx = 0;
				for (uint32_t slice = 0; slice < upload.dst_texture->desc.array_size; ++slice)
				{
					uint32_t width = upload.dst_texture->desc.width;
					uint32_t height = upload.dst_texture->desc.height;
					uint32_t depth = upload.dst_texture->desc.depth;
					for (uint32_t mip = 0; mip < upload.dst_texture->desc.mip_levels && initDataIdx < subresource_count; ++mip)
					{
						const SubresourceData& subresourceData = upload.subresources[initDataIdx++];
						const uint32_t block_size = GetFormatBlockSize(upload.dst_texture->desc.format);
						const uint32_t num_blocks_x = std::max(1u, width / block_size);
						const uint32_t num_blocks_y = std::max(1u, height / block_size);
						const uint64_t datasize = GetFormatStride(upload.dst_texture->desc.format) * num_blocks_x * num_blocks_y * depth;
						MTL::Region region = {};
						region.size.width = width;
						region.size.height = height;
						region.size.depth = depth;
						internal_state->texture->replaceRegion(region, mip, slice, subresourceData.data_ptr, subresourceData.row_pitch, datasize);
						depth = std::max(1u, depth / 2);
						width = std::max(block_size, width / 2);
						height = std::max(block_size, height / 2);
					}
				}
				return ticket;
			}

			NS::SharedPtr<NS::AutoreleasePool> autorelease_pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
			NS::SharedPtr<MTL::Buffer> staging = NS::TransferPtr(device->newBuffer(internal_state->texture->allocatedSize(), MTL::ResourceStorageModeShared | MTL::ResourceOptionCPUCacheModeWriteCombined));
			if (staging.get() == nullptr)
				return ticket;

			uint8_t* upload_data = (uint8_t*)staging->contents();
			uint32_t initDataIdx = 0;
			uint64_t src_offset = 0;
			for (uint32_t slice = 0; slice < upload.dst_texture->desc.array_size; ++slice)
			{
				uint32_t width = upload.dst_texture->desc.width;
				uint32_t height = upload.dst_texture->desc.height;
				uint32_t depth = upload.dst_texture->desc.depth;
				for (uint32_t mip = 0; mip < upload.dst_texture->desc.mip_levels && initDataIdx < subresource_count; ++mip)
				{
					const SubresourceData& subresourceData = upload.subresources[initDataIdx++];
					const uint32_t block_size = GetFormatBlockSize(upload.dst_texture->desc.format);
					const uint32_t num_blocks_x = std::max(1u, width / block_size);
					const uint32_t num_blocks_y = std::max(1u, height / block_size);
					const uint64_t datasize = GetFormatStride(upload.dst_texture->desc.format) * num_blocks_x * num_blocks_y * depth;
					std::memcpy(upload_data + src_offset, subresourceData.data_ptr, (size_t)datasize);
					src_offset += datasize;
					width = std::max(block_size, width / 2);
					height = std::max(block_size, height / 2);
					depth = std::max(1u, depth / 2);
				}
			}

			NS::SharedPtr<MTL4::CommandAllocator> commandallocator = NS::TransferPtr(device->newCommandAllocator());
			NS::SharedPtr<MTL4::CommandBuffer> commandbuffer = NS::TransferPtr(device->newCommandBuffer());
			commandbuffer->beginCommandBuffer(commandallocator.get());
			{
				std::scoped_lock residency_lock(allocationhandler->destroylocker);
				allocationhandler->residency_set->addAllocation(staging.get());
				allocationhandler->residency_set->commit();
			}
			MTL4::ComputeCommandEncoder* encoder = commandbuffer->computeCommandEncoder();

			initDataIdx = 0;
			src_offset = 0;
			for (uint32_t slice = 0; slice < upload.dst_texture->desc.array_size; ++slice)
			{
				uint32_t width = upload.dst_texture->desc.width;
				uint32_t height = upload.dst_texture->desc.height;
				uint32_t depth = upload.dst_texture->desc.depth;
				for (uint32_t mip = 0; mip < upload.dst_texture->desc.mip_levels && initDataIdx < subresource_count; ++mip)
				{
					const SubresourceData& subresourceData = upload.subresources[initDataIdx++];
					MTL::Origin origin = {};
					MTL::Size size = {};
					size.width = width;
					size.height = height;
					size.depth = depth;
					encoder->copyFromBuffer(staging.get(), src_offset, subresourceData.row_pitch, subresourceData.slice_pitch, size, internal_state->texture.get(), slice, mip, origin);

					const uint32_t block_size = GetFormatBlockSize(upload.dst_texture->desc.format);
					const uint32_t num_blocks_x = std::max(1u, width / block_size);
					const uint32_t num_blocks_y = std::max(1u, height / block_size);
					const uint64_t datasize = GetFormatStride(upload.dst_texture->desc.format) * num_blocks_x * num_blocks_y * depth;
					src_offset += datasize;
					width = std::max(block_size, width / 2);
					height = std::max(block_size, height / 2);
					depth = std::max(1u, depth / 2);
				}
			}

			encoder->endEncoding();
			commandbuffer->endCommandBuffer();

			ticket.token = allocate_submission_token(token_queue);
			ticket.completion = ticket.token.Get(token_queue);
			MTL4::CommandBuffer* cmds[] = { commandbuffer.get() };
			uploadqueue->commit(cmds, SDL_arraysize(cmds));
			if (ticket.completion.IsValid() && submission_token_events[token_queue].get() != nullptr)
			{
				uploadqueue->signalEvent(submission_token_events[token_queue].get(), ticket.completion.value);
			}

			UploadJob job = {};
			job.ticket = ticket;
			job.staging_buffer = staging;
			job.commandbuffer = commandbuffer;
			job.commandallocator = commandallocator;
			{
				std::scoped_lock lock(upload_locker);
				inflight_uploads.emplace_back(std::move(job));
					if (implicit_dependency)
					{
						pending_implicit_uploads.Merge(ticket.token);
					}
				}
			return ticket;
		}
		default:
			break;
		}

		return ticket;
	}

	SubmissionToken GraphicsDevice_Metal::SubmitCommandListsInternal(const SubmitDesc& desc)
	{
		QueueSubmissionStats stats = {};
		stats.frameIndex = FRAMECOUNT;
		const bool legacy_implicit_frame_sync = (std::getenv("FW_WICKED_LEGACY_IMPLICIT_FRAME_SYNC") != nullptr);
		stats.frameSyncMode = legacy_implicit_frame_sync ? QueueFrameSyncMode::FullFrameSync : QueueFrameSyncMode::NoFrameSync;
		stats.fullFrameSyncActive = legacy_implicit_frame_sync;

		uint64_t dependency_max[QUEUE_COUNT] = {};
		auto merge_dependency = [&](QueueSyncPoint point) {
			if (!point.IsValid() || point.queue >= QUEUE_COUNT)
				return;
#ifndef NDEBUG
			if (point.value > submission_token_values[point.queue])
			{
				METAL_LOG_ERROR(
					"[Metal][Submit] Ignoring dependency on future queue point: queue=%s value=%llu submitted=%llu",
					GetQueueTypeName(point.queue),
					(unsigned long long)point.value,
					(unsigned long long)submission_token_values[point.queue]
				);
				SDL_assert(false && "SubmitCommandListsEx received dependency on a future queue point");
				return;
			}
#endif
			dependency_max[point.queue] = std::max(dependency_max[point.queue], point.value);
		};

		allocationhandler->destroylocker.lock();
		allocationhandler->residency_set->commit();
		allocationhandler->destroylocker.unlock();
		retire_completed_uploads();

		const SubmissionToken external_dependencies = consume_pending_implicit_uploads();
		for (uint32_t i = 0; i < desc.queue_dependency_count; ++i)
		{
			merge_dependency(desc.queue_dependencies[i].point);
		}
		for (uint32_t i = 0; i < desc.submission_dependency_count; ++i)
		{
			const SubmissionToken& token = desc.submission_dependencies[i];
			for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
			{
				if ((token.queue_mask & (1u << q)) == 0 || token.values[q] == 0)
					continue;
				merge_dependency(QueueSyncPoint{ (QUEUE_TYPE)q, token.values[q] });
			}
		}
		for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
		{
			if ((external_dependencies.queue_mask & (1u << q)) == 0)
				continue;
			const uint64_t value = external_dependencies.values[q];
			if (value > 0)
			{
				merge_dependency(QueueSyncPoint{ (QUEUE_TYPE)q, value });
			}
		}

		CommandList_Metal** submit_commandlists = nullptr;
		cmd_locker.lock();
		if (desc.command_lists != nullptr && desc.command_list_count > 0)
		{
			for (uint32_t i = 0; i < desc.command_list_count; ++i)
			{
				const CommandList cmd = desc.command_lists[i];
				if (!wiGraphicsCommandListIsValid(cmd))
					continue;
				arrput(submit_commandlists, (CommandList_Metal*)cmd.internal_state);
			}
		}
		else
		{
			for (size_t i = 0; i < arrlenu(open_commandlists); ++i)
			{
				arrput(submit_commandlists, open_commandlists[i]);
			}
		}
		cmd_locker.unlock();

		const uint32_t cmd_last = (uint32_t)arrlenu(submit_commandlists);
		stats.commandListCount = cmd_last;
		if (cmd_last == 0)
		{
			arrfree(submit_commandlists);
			std::scoped_lock stats_lock(submission_stats_mutex);
			last_submission_stats = stats;
			return {};
		}

		auto find_submit_id = [&](uint32_t original_id) -> uint32_t {
			for (uint32_t i = 0; i < cmd_last; ++i)
			{
				if (submit_commandlists[i]->id == original_id)
					return i;
			}
			return std::numeric_limits<uint32_t>::max();
		};
		auto remap_wait_ids = [&](uint32_t* ids, const char* source_name, uint32_t consumer_id) {
			const uint32_t total = (uint32_t)arrlenu(ids);
			uint32_t valid = 0;
			for (uint32_t i = 0; i < total; ++i)
			{
				const uint32_t mapped = find_submit_id(ids[i]);
				if (mapped != std::numeric_limits<uint32_t>::max())
				{
					ids[valid++] = mapped;
					continue;
				}
#ifndef NDEBUG
				METAL_LOG_ERROR(
					"[Metal][Submit] Invalid %s entry: consumer cmd=%u waits on cmd=%u that is not in this submit batch",
					source_name,
					consumer_id,
					ids[i]
				);
				SDL_assert(false && "WaitCommandList dependency is not part of this SubmitCommandListsEx call");
#endif
			}
			arrsetlen(ids, valid);
		};

		bool active_queue[QUEUE_COUNT] = {};
		for (uint32_t cmd = 0; cmd < cmd_last; ++cmd)
		{
			auto& commandlist = *submit_commandlists[cmd];
			commandlist.id = cmd;
			remap_wait_ids(commandlist.wait_for_cmd_ids, "wait_for_cmd_ids", commandlist.id);
			active_queue[commandlist.queue] = true;
			stats.queues[commandlist.queue].commandLists++;
		}

		for (uint32_t consumer = 0; consumer < QUEUE_COUNT; ++consumer)
		{
			if (!active_queue[consumer] || queues[consumer].queue.get() == nullptr)
				continue;
			for (uint32_t producer = 0; producer < QUEUE_COUNT; ++producer)
			{
				if (producer == consumer)
					continue;
				const uint64_t value = dependency_max[producer];
				if (value == 0 || submission_token_events[producer].get() == nullptr)
					continue;
				queues[consumer].queue->wait(submission_token_events[producer].get(), value);
				stats.queues[consumer].dependencyWaits++;
				stats.dependencyEdges++;
			}
		}

		SubmissionToken tokens = {};
		bool queue_submitted[QUEUE_COUNT] = {};
		uint64_t queue_last_signaled[QUEUE_COUNT] = {};
		uint32_t* pending_commandlists[QUEUE_COUNT] = {};
		QueueSyncPoint* command_signal_points = nullptr;
		arrsetlen(command_signal_points, cmd_last);
		for (uint32_t i = 0; i < cmd_last; ++i)
		{
			command_signal_points[i] = {};
		}
		auto flush_queue_packet = [&](QUEUE_TYPE queue_type) {
			const uint32_t queue_index = (uint32_t)queue_type;
			CommandQueue& queue = queues[queue_index];
			if (queue.queue.get() == nullptr || queue.submit_cmds == nullptr || arrlenu(queue.submit_cmds) == 0)
				return;

			queue.submit();
			stats.queues[queue_index].packetsSubmitted++;
			queue_submitted[queue_index] = true;
			if (submission_token_events[queue_index].get() == nullptr)
			{
				arrsetlen(pending_commandlists[queue_index], 0);
				return;
			}

			const SubmissionToken token = allocate_submission_token((QUEUE_TYPE)queue_index);
			const QueueSyncPoint point = token.Get((QUEUE_TYPE)queue_index);
			queue.queue->signalEvent(submission_token_events[queue_index].get(), point.value);
			queue_last_signaled[queue_index] = point.value;
			{
				std::scoped_lock lock(allocationhandler->destroylocker);
				allocationhandler->submitted_queue_values[queue_index] = point.value;
			}

			for (uint32_t i = 0; i < arrlenu(pending_commandlists[queue_index]); ++i)
			{
				const uint32_t cmd_index = pending_commandlists[queue_index][i];
				if (cmd_index < cmd_last)
				{
					command_signal_points[cmd_index] = point;
				}
			}
			arrsetlen(pending_commandlists[queue_index], 0);
		};

		// Presents wait:
		for (uint32_t cmd = 0; cmd < cmd_last; ++cmd)
		{
			auto& commandlist = *submit_commandlists[cmd];
			for (size_t i = 0; i < arrlenu(commandlist.presents); ++i)
			{
				auto* x = commandlist.presents[i];
				CommandQueue& queue = queues[commandlist.queue];
				queue.queue->wait(x);
			}
		}

		// Submit work and resolve local command-list dependencies:
		for (uint32_t cmd = 0; cmd < cmd_last; ++cmd)
		{
			auto& commandlist = *submit_commandlists[cmd];
			if (commandlist.barriers != nullptr && arrlenu(commandlist.barriers) > 0)
			{
				if (commandlist.compute_encoder.get() == nullptr)
				{
					commandlist.compute_encoder = NS::TransferPtr(commandlist.commandbuffer->computeCommandEncoder()->retain());
				}
				MTL4::VisibilityOptions visibility_options = MTL4::VisibilityOptionNone;
				for (size_t i = 0; i < arrlenu(commandlist.barriers); ++i)
				{
					const auto& x = commandlist.barriers[i];
					if (x.type == GPUBarrier::Type::ALIASING)
					{
						visibility_options |= MTL4::VisibilityOptionResourceAlias;
					}
				}
				commandlist.compute_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, visibility_options);
				arrsetlen(commandlist.barriers, 0);
			}
			if (commandlist.compute_encoder.get() != nullptr)
			{
				commandlist.compute_encoder->endEncoding();
				commandlist.compute_encoder.reset();
				commandlist.compute_encoder = nullptr;
			}
			commandlist.commandbuffer->endCommandBuffer();

			uint64_t local_wait_value[QUEUE_COUNT] = {};
			bool has_local_cross_queue_wait = false;
			for (uint32_t i = 0; i < arrlenu(commandlist.wait_for_cmd_ids); ++i)
			{
				const uint32_t wait_id = commandlist.wait_for_cmd_ids[i];
				if (wait_id >= cmd_last)
				{
#ifndef NDEBUG
					METAL_LOG_ERROR(
						"[Metal][Submit] Invalid local dependency: consumer cmd=%u waits on cmd=%u (batch size=%u)",
						cmd,
						wait_id,
						cmd_last
					);
					SDL_assert(false && "WaitCommandList dependency ID is out of range");
#endif
					continue;
				}
				if (wait_id >= cmd)
				{
#ifndef NDEBUG
					METAL_LOG_ERROR(
						"[Metal][Submit] Invalid local dependency order: consumer cmd=%u waits on future cmd=%u",
						cmd,
						wait_id
					);
					SDL_assert(false && "WaitCommandList dependency must reference an earlier command list in submit order");
#endif
					continue;
				}
				const auto& producer = *submit_commandlists[wait_id];
				if (producer.queue == commandlist.queue)
					continue;

				QueueSyncPoint producer_point = command_signal_points[wait_id];
				if (!producer_point.IsValid())
				{
					flush_queue_packet(producer.queue);
					producer_point = command_signal_points[wait_id];
				}
				if (!producer_point.IsValid())
				{
#ifndef NDEBUG
					METAL_LOG_ERROR(
						"[Metal][Submit] Failed to resolve producer queue point: producer cmd=%u queue=%s",
						wait_id,
						GetQueueTypeName(producer.queue)
					);
					SDL_assert(false && "Unable to resolve producer queue timeline point");
#endif
					continue;
				}

				local_wait_value[producer_point.queue] = std::max(local_wait_value[producer_point.queue], producer_point.value);
				has_local_cross_queue_wait = true;
			}

			if (has_local_cross_queue_wait)
			{
				CommandQueue& queue = queues[commandlist.queue];
				// waits apply to work recorded after this point, so flush queue packet first
				flush_queue_packet(commandlist.queue);
				for (uint32_t producer_queue = 0; producer_queue < QUEUE_COUNT; ++producer_queue)
				{
					if (producer_queue == (uint32_t)commandlist.queue)
						continue;
					const uint64_t value = local_wait_value[producer_queue];
					if (value == 0 || submission_token_events[producer_queue].get() == nullptr)
						continue;
					queue.queue->wait(submission_token_events[producer_queue].get(), value);
					stats.dependencyEdges++;
					stats.queues[commandlist.queue].dependencyWaits++;
				}
			}
			arrput(queues[commandlist.queue].submit_cmds, commandlist.commandbuffer.get());
			arrput(pending_commandlists[commandlist.queue], cmd);

			// Worker pipelines are merged in to global:
			for (size_t i = 0; i < arrlenu(commandlist.pipelines_worker); ++i)
			{
				auto& x = commandlist.pipelines_worker[i];
				PipelineGlobalEntry* found = nullptr;
				for (size_t j = 0; j < arrlenu(pipelines_global); ++j)
				{
					if (pipelines_global[j].key == x.key)
					{
						found = pipelines_global + j;
						break;
					}
				}
				if (found == nullptr)
				{
					arrput(pipelines_global, x);
				}
				else
				{
					allocationhandler->destroylocker.lock();
					allocationhandler->Retire(allocationhandler->destroyer_render_pipelines, NS::TransferPtr(x.value.pipeline));
					allocationhandler->Retire(allocationhandler->destroyer_depth_stencil_states, NS::TransferPtr(x.value.depth_stencil_state));
					allocationhandler->destroylocker.unlock();
				}
			}
			arrsetlen(commandlist.pipelines_worker, 0);
			arrsetlen(commandlist.wait_for_cmd_ids, 0);
		}

		// Mark completion of queued packets:
		for (int q = 0; q < QUEUE_COUNT; ++q)
		{
			flush_queue_packet((QUEUE_TYPE)q);
			if (queue_last_signaled[q] != 0)
			{
				tokens.Merge(QueueSyncPoint{ (QUEUE_TYPE)q, queue_last_signaled[q] });
			}
		}

		if (legacy_implicit_frame_sync)
		{
			for (int queue1 = 0; queue1 < QUEUE_COUNT; ++queue1)
			{
				if (!active_queue[queue1] || queues[queue1].queue.get() == nullptr)
					continue;
				for (int queue2 = 0; queue2 < QUEUE_COUNT; ++queue2)
				{
					if (queue1 == queue2 || !active_queue[queue2] || queues[queue2].queue.get() == nullptr)
						continue;
					const uint64_t value = queue_last_signaled[queue2];
					if (value == 0 || submission_token_events[queue2].get() == nullptr)
						continue;
					queues[queue1].queue->wait(submission_token_events[queue2].get(), value);
					stats.queues[queue1].fullSyncWaitsInjected++;
				}
			}
		}

		// Presents submit:
		for (uint32_t cmd = 0; cmd < cmd_last; ++cmd)
		{
			auto& commandlist = *submit_commandlists[cmd];
			for (size_t i = 0; i < arrlenu(commandlist.presents); ++i)
			{
				auto* x = commandlist.presents[i];
				CommandQueue& queue = queues[commandlist.queue];
				queue.queue->signalDrawable(x);
				x->present();
				x->release();
			}
			arrsetlen(commandlist.presents, 0);
		}

		cmd_locker.lock();
		for (uint32_t cmd = 0; cmd < cmd_last; ++cmd)
		{
			CommandList_Metal* submitted = submit_commandlists[cmd];
			if (desc.command_lists != nullptr && desc.command_list_count > 0)
			{
				for (size_t i = 0; i < arrlenu(open_commandlists); ++i)
				{
					if (open_commandlists[i] != submitted)
						continue;
					open_commandlists[i] = arrlast(open_commandlists);
					arrpop(open_commandlists);
					break;
				}
			}
			RetiredCommandContext retired = {};
			retired.context = submitted;
			retired.retire_after = tokens.Get(submitted->queue);
			arrput(retired_contexts[submitted->queue], retired);
		}
		if (desc.command_lists == nullptr || desc.command_list_count == 0)
		{
			arrsetlen(open_commandlists, 0);
		}

		if (arrlenu(open_commandlists) > 0)
		{
			for (size_t i = 0; i < arrlenu(open_commandlists); ++i)
			{
				for (uint32_t j = 0; j < arrlenu(open_commandlists[i]->wait_for_cmd_ids); ++j)
				{
					uint32_t old_id = open_commandlists[i]->wait_for_cmd_ids[j];
					uint32_t new_id = std::numeric_limits<uint32_t>::max();
					for (uint32_t k = 0; k < arrlenu(open_commandlists); ++k)
					{
						if (open_commandlists[k]->id == old_id)
						{
							new_id = k;
							break;
						}
					}
					if (new_id == std::numeric_limits<uint32_t>::max())
					{
#ifndef NDEBUG
						METAL_LOG_ERROR(
							"[Metal][Submit] Dropping stale dependency from open command list: cmd=%u waits on removed cmd=%u",
							open_commandlists[i]->id,
							old_id
						);
						SDL_assert(false && "Open command list contains dependency to command list outside current open set");
#endif
						open_commandlists[i]->wait_for_cmd_ids[j] = new_id;
					}
					else
					{
						open_commandlists[i]->wait_for_cmd_ids[j] = new_id;
					}
				}
				uint32_t keep = 0;
				for (uint32_t j = 0; j < arrlenu(open_commandlists[i]->wait_for_cmd_ids); ++j)
				{
					if (open_commandlists[i]->wait_for_cmd_ids[j] != std::numeric_limits<uint32_t>::max())
					{
						open_commandlists[i]->wait_for_cmd_ids[keep++] = open_commandlists[i]->wait_for_cmd_ids[j];
					}
				}
				arrsetlen(open_commandlists[i]->wait_for_cmd_ids, keep);
			}
		}

		for (size_t i = 0; i < arrlenu(open_commandlists); ++i)
		{
			open_commandlists[i]->id = (uint32_t)i;
		}
		cmd_locker.unlock();

		// From here, we begin a new frame, this affects GetBufferIndex()!
		FRAMECOUNT++;

		if (desc.throttle_cpu || legacy_implicit_frame_sync)
		{
			const uint64_t default_budget = legacy_implicit_frame_sync ? 1u : 2u;
			const uint64_t budget = desc.max_inflight_per_queue > 0 ? desc.max_inflight_per_queue : default_budget;
			for (int queue = 0; queue < QUEUE_COUNT; ++queue)
			{
				if (!queue_submitted[queue] || submission_token_events[queue].get() == nullptr)
					continue;
				if (submission_token_values[queue] <= budget)
					continue;

				const uint64_t wait_until = submission_token_values[queue] - budget;
				if (submission_token_events[queue]->signaledValue() < wait_until)
				{
					submission_token_events[queue]->waitUntilSignaledValue(wait_until, ~0ull);
					stats.queues[queue].cpuFenceWaits++;
				}
			}
		}

		allocationhandler->Update();
		retire_completed_uploads();
		arrfree(submit_commandlists);
		for (uint32_t i = 0; i < QUEUE_COUNT; ++i)
		{
			arrfree(pending_commandlists[i]);
		}
		arrfree(command_signal_points);
		std::scoped_lock stats_lock(submission_stats_mutex);
		last_submission_stats = stats;
		return tokens;
	}

	SubmissionToken GraphicsDevice_Metal::SubmitCommandListsEx(const SubmitDesc& desc)
	{
		return SubmitCommandListsInternal(desc);
	}

	bool GraphicsDevice_Metal::IsQueuePointComplete(QueueSyncPoint point) const
	{
		if (!point.IsValid())
			return true;
		if (point.queue >= QUEUE_COUNT || submission_token_events[point.queue].get() == nullptr)
			return false;
		return submission_token_events[point.queue]->signaledValue() >= point.value;
	}

	void GraphicsDevice_Metal::WaitQueuePoint(QueueSyncPoint point) const
	{
		if (!point.IsValid() || point.queue >= QUEUE_COUNT || submission_token_events[point.queue].get() == nullptr)
			return;
		WaitForSharedEventWithAssert(submission_token_events[point.queue].get(), point.value, "WaitQueuePoint");
	}

	QueueSyncPoint GraphicsDevice_Metal::GetLastSubmittedQueuePoint(QUEUE_TYPE queue) const
	{
		if (queue >= QUEUE_COUNT)
			return {};
		std::scoped_lock lock(submission_token_locker);
		const uint64_t value = submission_token_values[queue];
		if (value == 0)
			return {};
		return QueueSyncPoint{ queue, value };
	}

	QueueSyncPoint GraphicsDevice_Metal::GetLastCompletedQueuePoint(QUEUE_TYPE queue) const
	{
		if (queue >= QUEUE_COUNT || submission_token_events[queue].get() == nullptr)
			return {};
		const uint64_t value = submission_token_events[queue]->signaledValue();
		if (value == 0)
			return {};
		return QueueSyncPoint{ queue, value };
	}

	bool GraphicsDevice_Metal::GetQueueSubmissionStats(QueueSubmissionStats& out) const
	{
		std::scoped_lock lock(submission_stats_mutex);
		out = last_submission_stats;
		return out.commandListCount > 0 || out.dependencyEdges > 0;
	}

	UploadTicket GraphicsDevice_Metal::EnqueueBufferUpload(const BufferUploadDesc& upload)
	{
		UploadDescInternal internal = {};
		internal.type = UploadDescInternal::Type::BUFFER;
		internal.src_data = upload.data;
		internal.src_size = upload.size;
		internal.dst_buffer = upload.dst;
		internal.dst_offset = upload.dst_offset;
		UploadTicket ticket = UploadAsyncInternal(internal, false);
		if (upload.block_until_complete && ticket.IsValid())
		{
			WaitUpload(ticket);
		}
		return ticket;
	}

	UploadTicket GraphicsDevice_Metal::EnqueueTextureUpload(const TextureUploadDesc& upload)
	{
		UploadDescInternal internal = {};
		internal.type = UploadDescInternal::Type::TEXTURE;
		internal.dst_texture = upload.dst;
		internal.subresources = upload.subresources;
		internal.subresource_count = upload.subresource_count;
		UploadTicket ticket = UploadAsyncInternal(internal, false);
		if (upload.block_until_complete && ticket.IsValid())
		{
			WaitUpload(ticket);
		}
		return ticket;
	}

	bool GraphicsDevice_Metal::IsUploadComplete(const UploadTicket& ticket) const
	{
		const bool complete = IsQueuePointComplete(ticket.completion);
		if (complete)
		{
			retire_completed_uploads();
		}
		return complete;
	}

	void GraphicsDevice_Metal::WaitUpload(const UploadTicket& ticket) const
	{
		if (!ticket.completion.IsValid() || ticket.completion.queue >= QUEUE_COUNT || submission_token_events[ticket.completion.queue].get() == nullptr)
			return;
		WaitForSharedEventWithAssert(submission_token_events[ticket.completion.queue].get(), ticket.completion.value, "WaitUpload");
		retire_completed_uploads();
	}

	void GraphicsDevice_Metal::WaitForGPU() const
	{
		NS::SharedPtr<MTL::SharedEvent> event = NS::TransferPtr(device->newSharedEvent());
		for (auto& queue : queues)
		{
			if (queue.queue.get() == nullptr)
				continue;
			event->setSignaledValue(0);
			queue.queue->signalEvent(event.get(), 1);
			WaitForSharedEventWithAssert(event.get(), 1, "WaitForGPU queue drain");
		}
		if (submission_token_events[QUEUE_COPY].get() != nullptr)
		{
			uint64_t latest_upload_token = 0;
			{
				std::scoped_lock lock(submission_token_locker);
				latest_upload_token = submission_token_values[QUEUE_COPY];
			}
			if (latest_upload_token > 0)
			{
				WaitForSharedEventWithAssert(submission_token_events[QUEUE_COPY].get(), latest_upload_token, "WaitForGPU upload queue drain");
			}
		}
		retire_completed_uploads();
	}
	void GraphicsDevice_Metal::ClearPipelineStateCache()
	{
		std::scoped_lock locker(allocationhandler->destroylocker);
		for (size_t i = 0; i < arrlenu(pipelines_global); ++i)
		{
			allocationhandler->Retire(allocationhandler->destroyer_render_pipelines, NS::TransferPtr(pipelines_global[i].value.pipeline));
			allocationhandler->Retire(allocationhandler->destroyer_depth_stencil_states, NS::TransferPtr(pipelines_global[i].value.depth_stencil_state));
		}
		arrfree(pipelines_global);
	}

	Texture GraphicsDevice_Metal::GetBackBuffer(const SwapChain* swapchain) const
	{
		auto swapchain_internal = to_internal(swapchain);
		Texture result;
		result.type = GPUResource::Type::TEXTURE;
		auto texture_internal = wi::allocator::make_shared<Texture_Metal>();
		texture_internal->texture = swapchain_internal->current_texture;
		texture_internal->srv.index = allocationhandler->allocate_resource_index();
		texture_internal->srv.entry = create_entry(texture_internal->texture.get());
		std::memcpy(descriptor_heap_res_data + texture_internal->srv.index, &texture_internal->srv.entry, sizeof(texture_internal->srv.entry));
		result.internal_state = texture_internal;
		result.desc.width = swapchain->desc.width;
		result.desc.height = swapchain->desc.height;
		result.desc.bind_flags = BindFlag::BIND_SHADER_RESOURCE | BindFlag::RENDER_TARGET;
		result.desc.format = swapchain->desc.format;
		return result;
	}
	ColorSpace GraphicsDevice_Metal::GetSwapChainColorSpace(const SwapChain* swapchain) const
	{
		auto internal_state = to_internal(swapchain);
		return internal_state->colorSpace;
	}
	bool GraphicsDevice_Metal::IsSwapChainSupportsHDR(const SwapChain* swapchain) const
	{
		// TODO
		return false;
	}

	void GraphicsDevice_Metal::SparseUpdate(QUEUE_TYPE queue, const SparseUpdateCommand* commands, uint32_t command_count)
	{
		MTL4::CommandQueue* commandqueue = queues[queue].queue.get();
		for (uint32_t i = 0; i < command_count; ++i)
		{
			const SparseUpdateCommand& command = commands[i];
			auto tilepool_internal = to_internal<GPUBuffer>(command.tile_pool);
			MTL::Heap* heap = tilepool_internal->buffer->heap();
			SDL_assert(heap != nullptr);
			
			if (wiGraphicsGPUResourceIsTexture(command.sparse_resource))
			{
				auto sparse_internal = to_internal<Texture>(command.sparse_resource);
			// stb_ds array: sparse texture mapping operations are collected explicitly and freed after submission.
			MTL4::UpdateSparseTextureMappingOperation* operations = nullptr;
				for (uint32_t j = 0; j < command.num_resource_regions; ++j)
				{
					const SparseResourceCoordinate& coordinate = command.coordinates[j];
					const SparseRegionSize& size = command.sizes[j];
					auto& op = arrput(operations, {});
					op.mode = command.range_flags[j] == TileRangeFlags::Null ? MTL::SparseTextureMappingModeUnmap : MTL::SparseTextureMappingModeMap;
					op.heapOffset = command.range_start_offsets[j];
					op.textureLevel = coordinate.mip;
					op.textureSlice = coordinate.slice;
					op.textureRegion.origin.x = coordinate.x;
					op.textureRegion.origin.y = coordinate.y;
					op.textureRegion.origin.z = coordinate.z;
					op.textureRegion.size.width = size.width;
					op.textureRegion.size.height = size.height;
					op.textureRegion.size.depth = size.depth;
				}
				commandqueue->updateTextureMappings(sparse_internal->texture.get(), heap, operations, arrlenu(operations));
				arrfree(operations);
			}
			else if (wiGraphicsGPUResourceIsBuffer(command.sparse_resource))
			{
				auto sparse_internal = to_internal<GPUBuffer>(command.sparse_resource);
				// stb_ds array: sparse buffer mapping operations are collected explicitly and freed after submission.
				MTL4::UpdateSparseBufferMappingOperation* operations = nullptr;
				for (uint32_t j = 0; j < command.num_resource_regions; ++j)
				{
					const SparseResourceCoordinate& coordinate = command.coordinates[j];
					const SparseRegionSize& size = command.sizes[j];
					MTL4::UpdateSparseBufferMappingOperation op = {
						command.range_flags[j] == TileRangeFlags::Null ? MTL::SparseTextureMappingModeUnmap : MTL::SparseTextureMappingModeMap,
						{coordinate.x, size.width},
						command.range_start_offsets[j]
					};
					arrput(operations, op);
				}
				commandqueue->updateBufferMappings(sparse_internal->buffer.get(), heap, operations, arrlenu(operations));
				arrfree(operations);
			}
		}
		
#if 0
		NS::SharedPtr<MTL::SharedEvent> event = NS::TransferPtr(device->newSharedEvent());
		event->setSignaledValue(0);
		commandqueue->signalEvent(event.get(), 1);
		event->waitUntilSignaledValue(1, ~0ull);
#endif
	}

	void GraphicsDevice_Metal::WaitCommandList(CommandList cmd, CommandList wait_for)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		CommandList_Metal& commandlist_wait_for = GetCommandList(wait_for);
		SDL_assert(commandlist_wait_for.id < commandlist.id); // can't wait for future command list!
		bool exists = false;
		for (uint32_t i = 0; i < arrlenu(commandlist.wait_for_cmd_ids); ++i)
		{
			if (commandlist.wait_for_cmd_ids[i] == commandlist_wait_for.id)
			{
				exists = true;
				break;
			}
		}
		if (!exists)
		{
			arrput(commandlist.wait_for_cmd_ids, commandlist_wait_for.id);
		}
	}
	void GraphicsDevice_Metal::RenderPassBegin(const SwapChain* swapchain, CommandList cmd)
	{
		clear_flush(cmd);
		NS::SharedPtr<NS::AutoreleasePool> autorelease_pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->endEncoding();
			commandlist.compute_encoder.reset();
			commandlist.compute_encoder = nullptr;
		}
		
		auto internal_state = to_internal(swapchain);
		
		CA::MetalDrawable* drawable = internal_state->layer->nextDrawable();
		while (drawable == nullptr)
		{
			drawable = internal_state->layer->nextDrawable();
		}
		internal_state->current_drawable = NS::TransferPtr(drawable->retain());
		internal_state->current_texture = NS::TransferPtr(drawable->texture()->retain());
		
		arrput(commandlist.presents, internal_state->current_drawable.get()->retain());
		
		NS::SharedPtr<MTL4::RenderPassDescriptor> descriptor = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
		NS::SharedPtr<MTL::RenderPassColorAttachmentDescriptor> color_attachment_descriptor = NS::TransferPtr(MTL::RenderPassColorAttachmentDescriptor::alloc()->init());
		
		CGSize size = internal_state->layer->drawableSize();
		descriptor->setRenderTargetWidth(size.width);
		descriptor->setRenderTargetHeight(size.height);
		descriptor->setDefaultRasterSampleCount(1);
		
		color_attachment_descriptor->setTexture(internal_state->current_drawable->texture());
		color_attachment_descriptor->setClearColor(MTL::ClearColor::Make(swapchain->desc.clear_color[0], swapchain->desc.clear_color[1], swapchain->desc.clear_color[2], swapchain->desc.clear_color[3]));
		color_attachment_descriptor->setLoadAction(MTL::LoadActionClear);
		color_attachment_descriptor->setStoreAction(MTL::StoreActionStore);
		descriptor->colorAttachments()->setObject(color_attachment_descriptor.get(), 0);
		
		commandlist.render_encoder = NS::TransferPtr(commandlist.commandbuffer->renderCommandEncoder(descriptor.get())->retain());
		commandlist.dirty_vb = true;
		commandlist.dirty_root = true;
		commandlist.dirty_sampler = true;
		commandlist.dirty_resource = true;
		commandlist.dirty_scissor = true;
		commandlist.dirty_viewport = true;
		commandlist.dirty_pso = true;
		
		commandlist.render_width = size.width;
		commandlist.render_height = size.height;
		
		commandlist.renderpass_info = wiGraphicsCreateRenderPassInfoFromSwapChainDesc(&swapchain->desc);
		commandlist.active_renderpass_is_swapchain = true;
		commandlist.active_renderpass_swapchain = swapchain;
		commandlist.active_renderpass_occlusionqueries = nullptr;
		commandlist.active_renderpass_has_draws = false;
		arrsetlen(commandlist.active_renderpass_images, 0);
		
		barrier_flush(cmd);
	}
	void GraphicsDevice_Metal::RenderPassBegin(const RenderPassImage* images, uint32_t image_count, const GPUQueryHeap* occlusionqueries, CommandList cmd, RenderPassFlags flags)
	{
		clear_flush(cmd);
		NS::SharedPtr<NS::AutoreleasePool> autorelease_pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->endEncoding();
			commandlist.compute_encoder.reset();
			commandlist.compute_encoder = nullptr;
		}
		
		NS::SharedPtr<MTL4::RenderPassDescriptor> descriptor = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
		NS::SharedPtr<MTL::RenderPassColorAttachmentDescriptor> color_attachment_descriptors[8];
		NS::SharedPtr<MTL::RenderPassDepthAttachmentDescriptor> depth_attachment_descriptor;
		NS::SharedPtr<MTL::RenderPassStencilAttachmentDescriptor> stencil_attachment_descriptor;
		
		if (image_count > 0)
		{
			descriptor->setRenderTargetWidth(images[0].texture->desc.width);
			descriptor->setRenderTargetHeight(images[0].texture->desc.height);
			descriptor->setRenderTargetArrayLength(images[0].texture->desc.array_size);
			descriptor->setDefaultRasterSampleCount(images[0].texture->desc.sample_count);
			commandlist.render_width = images[0].texture->desc.width;
			commandlist.render_height = images[0].texture->desc.height;
		}
		else
		{
			descriptor->setRenderTargetWidth(commandlist.viewports[0].width);
			descriptor->setRenderTargetHeight(commandlist.viewports[0].height);
			descriptor->setDefaultRasterSampleCount(1);
		}
		
		uint32_t color_attachment_index = 0;
		uint32_t resolve_index = 0;
		for (uint32_t i = 0; i < image_count; ++i)
		{
			const RenderPassImage& image = images[i];
			auto internal_state = to_internal(image.texture);
			
			MTL::LoadAction load_action;
			switch (image.loadop) {
				case RenderPassImage::LoadOp::LOADOP_DONTCARE:
					load_action = MTL::LoadActionDontCare;
					break;
				case RenderPassImage::LoadOp::LOAD:
					load_action = MTL::LoadActionLoad;
					break;
				case RenderPassImage::LoadOp::CLEAR:
					load_action = MTL::LoadActionClear;
					break;
				default:
					break;
			}
			MTL::StoreAction store_action;
			switch (image.storeop) {
			  case RenderPassImage::StoreOp::STOREOP_DONTCARE:
				  store_action = MTL::StoreActionDontCare;
				  break;
			  case RenderPassImage::StoreOp::STORE:
				  store_action = MTL::StoreActionStore;
				  break;
			  default:
				  break;
			}
			
			switch (image.type) {
				case RenderPassImage::Type::RENDERTARGET:
				{
					Texture_Metal::Subresource& subresource = image.subresource < 0 ? internal_state->rtv : internal_state->subresources_rtv[image.subresource];
					auto& color_attachment_descriptor = color_attachment_descriptors[color_attachment_index];
					color_attachment_descriptor = NS::TransferPtr(MTL::RenderPassColorAttachmentDescriptor::alloc()->init());
					color_attachment_descriptor->init();
					color_attachment_descriptor->setTexture(internal_state->texture.get());
					color_attachment_descriptor->setLevel(subresource.firstMip);
					color_attachment_descriptor->setSlice(subresource.firstSlice);
					color_attachment_descriptor->setClearColor(MTL::ClearColor::Make(image.texture->desc.clear.color[0], image.texture->desc.clear.color[1], image.texture->desc.clear.color[2], image.texture->desc.clear.color[3]));
					color_attachment_descriptor->setLoadAction(load_action);
					color_attachment_descriptor->setStoreAction(store_action);
					color_attachment_descriptor->setResolveTexture(nullptr);
					color_attachment_descriptor->setResolveLevel(0);
					color_attachment_descriptor->setResolveSlice(0);
					descriptor->colorAttachments()->setObject(color_attachment_descriptor.get(), color_attachment_index);
					color_attachment_index++;
					break;
				}
				case RenderPassImage::Type::DEPTH_STENCIL:
				{
					Texture_Metal::Subresource& subresource = image.subresource < 0 ? internal_state->dsv : internal_state->subresources_dsv[image.subresource];
					depth_attachment_descriptor = NS::TransferPtr(MTL::RenderPassDepthAttachmentDescriptor::alloc()->init());
					depth_attachment_descriptor->setTexture(internal_state->texture.get());
					depth_attachment_descriptor->setLevel(subresource.firstMip);
					depth_attachment_descriptor->setSlice(subresource.firstSlice);
					depth_attachment_descriptor->setClearDepth(image.texture->desc.clear.depth_stencil.depth);
					depth_attachment_descriptor->setLoadAction(load_action);
					depth_attachment_descriptor->setStoreAction(store_action);
					depth_attachment_descriptor->setResolveTexture(nullptr);
					descriptor->setDepthAttachment(depth_attachment_descriptor.get());
					if (IsFormatStencilSupport(image.texture->desc.format))
					{
						stencil_attachment_descriptor = NS::TransferPtr(MTL::RenderPassStencilAttachmentDescriptor::alloc()->init());
						stencil_attachment_descriptor->setTexture(internal_state->texture.get());
						stencil_attachment_descriptor->setClearStencil(image.texture->desc.clear.depth_stencil.stencil);
						stencil_attachment_descriptor->setLoadAction(load_action);
						stencil_attachment_descriptor->setStoreAction(store_action);
						descriptor->setStencilAttachment(stencil_attachment_descriptor.get());
					}
					break;
				}
				case RenderPassImage::Type::RESOLVE:
				{
					Texture_Metal::Subresource& subresource = image.subresource < 0 ? internal_state->rtv : internal_state->subresources_rtv[image.subresource];
					auto& color_attachment_descriptor = color_attachment_descriptors[resolve_index];
					color_attachment_descriptor->setResolveTexture(internal_state->texture.get());
					color_attachment_descriptor->setResolveLevel(subresource.firstMip);
					color_attachment_descriptor->setResolveSlice(subresource.firstSlice);
					color_attachment_descriptor->setStoreAction(color_attachment_descriptor->storeAction() == MTL::StoreActionStore ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionMultisampleResolve);
					descriptor->colorAttachments()->setObject(color_attachment_descriptor.get(), resolve_index);
					resolve_index++;
					break;
				}
				case RenderPassImage::Type::RESOLVE_DEPTH:
				{
					Texture_Metal::Subresource& subresource = image.subresource < 0 ? internal_state->dsv : internal_state->subresources_dsv[image.subresource];
					depth_attachment_descriptor->setResolveTexture(internal_state->texture.get());
					depth_attachment_descriptor->setResolveLevel(subresource.firstMip);
					depth_attachment_descriptor->setResolveSlice(subresource.firstSlice);
					depth_attachment_descriptor->setStoreAction(depth_attachment_descriptor->storeAction() == MTL::StoreActionStore ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionMultisampleResolve);
					descriptor->setDepthAttachment(depth_attachment_descriptor.get());
					break;
				}
				default:
					SDL_assert(0);
					break;
			}
		}
		
		if (wiGraphicsGPUQueryHeapIsValid(occlusionqueries))
		{
			auto occlusionquery_internal = to_internal(occlusionqueries);
			descriptor->setVisibilityResultBuffer(occlusionquery_internal->buffer.get());
			descriptor->setVisibilityResultType(MTL::VisibilityResultTypeReset);
		}
		
		MTL4::RenderEncoderOptions options = MTL4::RenderEncoderOptionNone;
		if (has_flag(flags, RenderPassFlags::SUSPENDING))
		{
			options |= MTL4::RenderEncoderOptionSuspending;
		}
		if (has_flag(flags, RenderPassFlags::RESUMING))
		{
			options |= MTL4::RenderEncoderOptionResuming;
		}
		commandlist.render_encoder = NS::TransferPtr(commandlist.commandbuffer->renderCommandEncoder(descriptor.get(), options)->retain());
		if (wiGraphicsGPUQueryHeapIsValid(occlusionqueries))
		{
			commandlist.render_encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
		}
		commandlist.dirty_vb = true;
		commandlist.dirty_root = true;
		commandlist.dirty_sampler = true;
		commandlist.dirty_resource = true;
		commandlist.dirty_scissor = true;
		commandlist.dirty_viewport = true;
		commandlist.dirty_pso = true;
		
		commandlist.renderpass_info = wiGraphicsCreateRenderPassInfoFromImages(images, image_count);
		commandlist.active_renderpass_is_swapchain = false;
		commandlist.active_renderpass_swapchain = nullptr;
		commandlist.active_renderpass_occlusionqueries = occlusionqueries;
		commandlist.active_renderpass_has_draws = false;
		arrsetlen(commandlist.active_renderpass_images, image_count);
		if (image_count > 0)
		{
			std::memcpy(commandlist.active_renderpass_images, images, image_count * sizeof(RenderPassImage));
		}
		
		barrier_flush(cmd);
	}
	void GraphicsDevice_Metal::RenderPassEnd(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		SDL_assert(commandlist.render_encoder.get() != nullptr);
		
		commandlist.render_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone);
		commandlist.render_encoder->endEncoding();
		commandlist.render_encoder.reset();
		commandlist.dirty_pso = true;
		
		commandlist.render_width = 0;
		commandlist.render_height = 0;

		commandlist.renderpass_info = {};
		commandlist.render_encoder = nullptr;
		commandlist.active_renderpass_is_swapchain = false;
		commandlist.active_renderpass_swapchain = nullptr;
		commandlist.active_renderpass_occlusionqueries = nullptr;
		commandlist.active_renderpass_has_draws = false;
		arrsetlen(commandlist.active_renderpass_images, 0);
	}
	void GraphicsDevice_Metal::BindScissorRects(uint32_t numRects, const Rect* rects, CommandList cmd)
	{
		SDL_assert(rects != nullptr);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		for (uint32_t i = 0; i < numRects; ++i)
		{
			commandlist.scissors[i].x = uint32_t(rects[i].left);
			commandlist.scissors[i].y = uint32_t(rects[i].top);
			commandlist.scissors[i].width = uint32_t(rects[i].right - rects[i].left);
			commandlist.scissors[i].height = uint32_t(rects[i].bottom - rects[i].top);
		}
		commandlist.scissor_count = numRects;
		commandlist.dirty_scissor = true;
	}
	void GraphicsDevice_Metal::BindViewports(uint32_t NumViewports, const Viewport* pViewports, CommandList cmd)
	{
		SDL_assert(pViewports != nullptr);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		for (uint32_t i = 0; i < NumViewports; ++i)
		{
			commandlist.viewports[i].originX = pViewports[i].top_left_x;
			commandlist.viewports[i].originY = pViewports[i].top_left_y;
			commandlist.viewports[i].width = pViewports[i].width;
			commandlist.viewports[i].height = pViewports[i].height;
			commandlist.viewports[i].znear = pViewports[i].min_depth;
			commandlist.viewports[i].zfar = pViewports[i].max_depth;
		}
		commandlist.viewport_count = NumViewports;
		commandlist.dirty_viewport = true;
	}
	void GraphicsDevice_Metal::BindResource(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_SRV_COUNT);
		if (commandlist.binding_table.SRV[slot].internal_state == resource->internal_state && commandlist.binding_table.SRV_index[slot] == subresource)
			return;
		commandlist.dirty_root = true;
		commandlist.dirty_resource = true;
		commandlist.binding_table.SRV[slot] = *resource;
		commandlist.binding_table.SRV_index[slot] = subresource;
	}
	void GraphicsDevice_Metal::BindResources(const GPUResource *const* resources, uint32_t slot, uint32_t count, CommandList cmd)
	{
		if (resources != nullptr)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				BindResource(resources[i], slot + i, cmd, -1);
			}
		}
	}
	void GraphicsDevice_Metal::BindUAV(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_UAV_COUNT);
		if (commandlist.binding_table.UAV[slot].internal_state == resource->internal_state && commandlist.binding_table.UAV_index[slot] == subresource)
			return;
		commandlist.dirty_root = true;
		commandlist.dirty_resource = true;
		commandlist.binding_table.UAV[slot] = *resource;
		commandlist.binding_table.UAV_index[slot] = subresource;
	}
	void GraphicsDevice_Metal::BindUAVs(const GPUResource *const* resources, uint32_t slot, uint32_t count, CommandList cmd)
	{
		if (resources != nullptr)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				BindUAV(resources[i], slot + i, cmd, -1);
			}
		}
	}
	void GraphicsDevice_Metal::BindSampler(const Sampler* sampler, uint32_t slot, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_SAMPLER_COUNT);
		if (commandlist.binding_table.SAM[slot].internal_state == sampler->internal_state)
			return;
		commandlist.dirty_root = true;
		commandlist.dirty_sampler = true;
		commandlist.binding_table.SAM[slot] = *sampler;
	}
	void GraphicsDevice_Metal::BindConstantBuffer(const GPUBuffer* buffer, uint32_t slot, CommandList cmd, uint64_t offset)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		SDL_assert(slot < DESCRIPTORBINDER_CBV_COUNT);
		if (commandlist.binding_table.CBV[slot].internal_state == buffer->internal_state && commandlist.binding_table.CBV_offset[slot] == offset)
			return;
		commandlist.dirty_root = true;
		if (slot >= SDL_arraysize(RootLayout::root_cbvs))
		{
			commandlist.dirty_resource = true;
		}
		commandlist.binding_table.CBV[slot] = *buffer;
		commandlist.binding_table.CBV_offset[slot] = offset;
	}
	void GraphicsDevice_Metal::BindVertexBuffers(const GPUBuffer *const* vertexBuffers, uint32_t slot, uint32_t count, const uint32_t* strides, const uint64_t* offsets, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.dirty_vb = true;
		for (uint32_t i = 0; i < count; ++i)
		{
			auto& vb = commandlist.vertex_buffers[slot + i];
			if (!wiGraphicsGPUResourceIsValid(vertexBuffers[i]))
				continue;
			auto internal_state = to_internal(vertexBuffers[i]);
			const uint64_t offset = offsets == nullptr ? 0 : offsets[i];
			vb.gpu_address = internal_state->gpu_address + offset;
			vb.stride = strides[i];
		}
	}
	void GraphicsDevice_Metal::BindIndexBuffer(const GPUBuffer* indexBuffer, const IndexBufferFormat format, uint64_t offset, CommandList cmd)
	{
		if (!wiGraphicsGPUResourceIsValid(indexBuffer))
			return;
		auto internal_state = to_internal(indexBuffer);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.index_buffer.bufferAddress = internal_state->gpu_address + offset;
		commandlist.index_buffer.length = internal_state->buffer->length();
		commandlist.index_type = format == IndexBufferFormat::UINT32 ? MTL::IndexTypeUInt32 : MTL::IndexTypeUInt16;
	}
	void GraphicsDevice_Metal::BindStencilRef(uint32_t value, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.render_encoder->setStencilReferenceValue(value);
	}
	void GraphicsDevice_Metal::BindBlendFactor(float r, float g, float b, float a, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.render_encoder->setBlendColor(r, g, b, a);
	}
	void GraphicsDevice_Metal::BindShadingRate(ShadingRate rate, CommandList cmd)
	{
	}
	void GraphicsDevice_Metal::BindPipelineState(const PipelineState* pso, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		auto internal_state = to_internal(pso);
		
		if (internal_state->render_pipeline.get() == nullptr)
		{
			// Just in time pso:
			PipelineHash pipeline_hash;
			pipeline_hash.pso = pso;
			pipeline_hash.renderpass_hash = commandlist.renderpass_info.get_hash();
			if (!(commandlist.pipeline_hash == pipeline_hash))
			{
				commandlist.dirty_pso = true;
				commandlist.pipeline_hash = pipeline_hash;
			}
		}
		else
		{
			// Precompiled pso:
			if (commandlist.active_pso != pso)
			{
				commandlist.active_pso = pso;
				commandlist.dirty_pso = true;
				commandlist.pipeline_hash = {};
			}
		}
		commandlist.active_pso = pso;
		commandlist.drawargs_required = internal_state->needs_draw_params;
		commandlist.numthreads_as = internal_state->numthreads_as;
		commandlist.numthreads_ms = internal_state->numthreads_ms;
		commandlist.gs_desc = internal_state->gs_desc;
	}
	void GraphicsDevice_Metal::BindComputeShader(const Shader* cs, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.dirty_cs = commandlist.active_cs != cs;
		commandlist.active_cs = cs;
	}
	void GraphicsDevice_Metal::BindDepthBounds(float min_bounds, float max_bounds, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.render_encoder->setDepthTestBounds(min_bounds, max_bounds);
	}

	void GraphicsDevice_Metal::Draw(uint32_t vertexCount, uint32_t startVertexLocation, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		
		if (commandlist.gs_desc.basePipelineDescriptor != nullptr)
		{
			// IRRuntimeDrawPrimitivesGeometryEmulation Metal4 port:
			IRRuntimeDrawInfo drawInfo = IRRuntimeCalculateDrawInfoForGSEmulation(commandlist.ir_primitive_type,
																				  (indextype_t)-1,
																				  commandlist.gs_desc.pipelineConfig.gsVertexSizeInBytes,
																				  commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
																				  1);
			drawInfo.indexType = kIRNonIndexedDraw;
			
			mtlsize_t objectThreadgroupCount = IRRuntimeCalculateObjectTgCountForTessellationAndGeometryEmulation(vertexCount,
																												  drawInfo.objectThreadgroupVertexStride,
																												  commandlist.ir_primitive_type,
																												  1);
			
			uint32_t objectThreadgroupSize,meshThreadgroupSize;
			IRRuntimeCalculateThreadgroupSizeForGeometry(commandlist.ir_primitive_type,
														 commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
														 drawInfo.objectThreadgroupVertexStride,
														 &objectThreadgroupSize,
														 &meshThreadgroupSize);
			
			IRRuntimeDrawArgument da = {
				.vertexCountPerInstance = vertexCount,
				.instanceCount = 1,
				.startVertexLocation = startVertexLocation,
				.startInstanceLocation = 0
			};
			IRRuntimeDrawParams dp = { .draw = da };
			
			auto alloc = AllocateGPU(sizeof(drawInfo), cmd);
			std::memcpy(alloc.data, &drawInfo, sizeof(drawInfo));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
			alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			
			predraw(cmd);
			commandlist.render_encoder->drawMeshThreadgroups(objectThreadgroupCount, MTL::Size::Make(objectThreadgroupSize, 1, 1), MTL::Size::Make(meshThreadgroupSize, 1, 1));
			return;
		}
		else if (commandlist.drawargs_required)
		{
			commandlist.dirty_drawargs = true;
			IRRuntimeDrawArgument da = {
				.vertexCountPerInstance = vertexCount,
				.instanceCount = 1,
				.startVertexLocation = startVertexLocation,
				.startInstanceLocation = 0
			};
			IRRuntimeDrawParams dp = { .draw = da };
			auto alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			alloc = AllocateGPU(sizeof(uint16_t), cmd);
			std::memcpy(alloc.data, &kIRNonIndexedDraw, sizeof(uint16_t));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}
		
		predraw(cmd);
		commandlist.render_encoder->drawPrimitives(commandlist.primitive_type, startVertexLocation, vertexCount);
	}
	void GraphicsDevice_Metal::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		const uint64_t index_stride = commandlist.index_type == MTL::IndexTypeUInt32 ? sizeof(uint32_t) : sizeof(uint16_t);
		const uint64_t indexBufferOffset = startIndexLocation * index_stride;
		
		if (commandlist.gs_desc.basePipelineDescriptor != nullptr)
		{
			// IRRuntimeDrawIndexedPrimitivesGeometryEmulation Metal4 port:
			IRRuntimeDrawInfo drawInfo = IRRuntimeCalculateDrawInfoForGSEmulation(commandlist.ir_primitive_type,
																				  commandlist.index_type,
																				  commandlist.gs_desc.pipelineConfig.gsVertexSizeInBytes,
																				  commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
																				  1);
			
			mtlsize_t objectThreadgroupCount = IRRuntimeCalculateObjectTgCountForTessellationAndGeometryEmulation(indexCount,
																												  drawInfo.objectThreadgroupVertexStride,
																												  commandlist.ir_primitive_type,
																												  1);
			
			uint32_t objectThreadgroupSize,meshThreadgroupSize;
			IRRuntimeCalculateThreadgroupSizeForGeometry(commandlist.ir_primitive_type,
														 commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
														 drawInfo.objectThreadgroupVertexStride,
														 &objectThreadgroupSize,
														 &meshThreadgroupSize);
			
			IRRuntimeDrawIndexedArgument da = {
				.indexCountPerInstance = indexCount,
				.instanceCount = 1,
				.startIndexLocation = startIndexLocation,
				.baseVertexLocation = baseVertexLocation,
				.startInstanceLocation = 0
			};
			IRRuntimeDrawParams dp = { .drawIndexed = da };
			
			drawInfo.indexBuffer = commandlist.index_buffer.bufferAddress + indexBufferOffset;
			
			auto alloc = AllocateGPU(sizeof(drawInfo), cmd);
			std::memcpy(alloc.data, &drawInfo, sizeof(drawInfo));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
			alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			
			predraw(cmd);
			commandlist.render_encoder->drawMeshThreadgroups(objectThreadgroupCount, MTL::Size::Make(objectThreadgroupSize, 1, 1), MTL::Size::Make(meshThreadgroupSize, 1, 1));
			return;
		}
		else if (commandlist.drawargs_required)
		{
			commandlist.dirty_drawargs = true;
			IRRuntimeDrawIndexedArgument da = {
				.indexCountPerInstance = indexCount,
				.instanceCount = 1,
				.startIndexLocation = startIndexLocation,
				.baseVertexLocation = baseVertexLocation,
				.startInstanceLocation = 0
			};
			IRRuntimeDrawParams dp = { .drawIndexed = da };
			auto alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			alloc = AllocateGPU(sizeof(uint16_t), cmd);
			uint16_t irindextype = IRMetalIndexToIRIndex(commandlist.index_type);
			std::memcpy(alloc.data, &irindextype, sizeof(uint16_t));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}
		
		predraw(cmd);
		commandlist.render_encoder->drawIndexedPrimitives(commandlist.primitive_type, indexCount, commandlist.index_type, commandlist.index_buffer.bufferAddress + indexBufferOffset, commandlist.index_buffer.length);
	}
	void GraphicsDevice_Metal::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		
		if (commandlist.gs_desc.basePipelineDescriptor != nullptr)
		{
			// IRRuntimeDrawPrimitivesGeometryEmulation Metal4 port:
			IRRuntimeDrawInfo drawInfo = IRRuntimeCalculateDrawInfoForGSEmulation(commandlist.ir_primitive_type,
																				  (indextype_t)-1,
																				  commandlist.gs_desc.pipelineConfig.gsVertexSizeInBytes,
																				  commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
																				  instanceCount);
			drawInfo.indexType = kIRNonIndexedDraw;
			
			mtlsize_t objectThreadgroupCount = IRRuntimeCalculateObjectTgCountForTessellationAndGeometryEmulation(vertexCount,
																												  drawInfo.objectThreadgroupVertexStride,
																												  commandlist.ir_primitive_type,
																												  instanceCount);
			
			uint32_t objectThreadgroupSize,meshThreadgroupSize;
			IRRuntimeCalculateThreadgroupSizeForGeometry(commandlist.ir_primitive_type,
														 commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
														 drawInfo.objectThreadgroupVertexStride,
														 &objectThreadgroupSize,
														 &meshThreadgroupSize);
			
			IRRuntimeDrawArgument da = {
				.vertexCountPerInstance = vertexCount,
				.instanceCount = instanceCount,
				.startVertexLocation = startVertexLocation,
				.startInstanceLocation = startInstanceLocation
			};
			IRRuntimeDrawParams dp = { .draw = da };
			
			auto alloc = AllocateGPU(sizeof(drawInfo), cmd);
			std::memcpy(alloc.data, &drawInfo, sizeof(drawInfo));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
			alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			
			predraw(cmd);
			commandlist.render_encoder->drawMeshThreadgroups(objectThreadgroupCount, MTL::Size::Make(objectThreadgroupSize, 1, 1), MTL::Size::Make(meshThreadgroupSize, 1, 1));
			return;
		}
		else if (commandlist.drawargs_required)
		{
			commandlist.dirty_drawargs = true;
			IRRuntimeDrawArgument da = {
				.vertexCountPerInstance = vertexCount,
				.instanceCount = instanceCount,
				.startVertexLocation = startVertexLocation,
				.startInstanceLocation = startInstanceLocation
			};
			IRRuntimeDrawParams dp = { .draw = da };
			auto alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			alloc = AllocateGPU(sizeof(uint16_t), cmd);
			std::memcpy(alloc.data, &kIRNonIndexedDraw, sizeof(uint16_t));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}
		
		predraw(cmd);
		commandlist.render_encoder->drawPrimitives(commandlist.primitive_type, startVertexLocation, vertexCount, instanceCount, startInstanceLocation);
	}
	void GraphicsDevice_Metal::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		const uint64_t index_stride = commandlist.index_type == MTL::IndexTypeUInt32 ? sizeof(uint32_t) : sizeof(uint16_t);
		const uint64_t indexBufferOffset = startIndexLocation * index_stride;
		
		if (commandlist.gs_desc.basePipelineDescriptor != nullptr)
		{
			// IRRuntimeDrawIndexedPrimitivesGeometryEmulation Metal4 port:
			IRRuntimeDrawInfo drawInfo = IRRuntimeCalculateDrawInfoForGSEmulation(commandlist.ir_primitive_type,
																				  commandlist.index_type,
																				  commandlist.gs_desc.pipelineConfig.gsVertexSizeInBytes,
																				  commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
																				  instanceCount);
			
			mtlsize_t objectThreadgroupCount = IRRuntimeCalculateObjectTgCountForTessellationAndGeometryEmulation(indexCount,
																												  drawInfo.objectThreadgroupVertexStride,
																												  commandlist.ir_primitive_type,
																												  instanceCount);
			
			uint32_t objectThreadgroupSize,meshThreadgroupSize;
			IRRuntimeCalculateThreadgroupSizeForGeometry(commandlist.ir_primitive_type,
														 commandlist.gs_desc.pipelineConfig.gsMaxInputPrimitivesPerMeshThreadgroup,
														 drawInfo.objectThreadgroupVertexStride,
														 &objectThreadgroupSize,
														 &meshThreadgroupSize);
			
			IRRuntimeDrawIndexedArgument da = {
				.indexCountPerInstance = indexCount,
				.instanceCount = 1,
				.startIndexLocation = startIndexLocation,
				.baseVertexLocation = baseVertexLocation,
				.startInstanceLocation = startInstanceLocation
			};
			IRRuntimeDrawParams dp = { .drawIndexed = da };
			
			drawInfo.indexBuffer = commandlist.index_buffer.bufferAddress + indexBufferOffset;
			
			auto alloc = AllocateGPU(sizeof(drawInfo), cmd);
			std::memcpy(alloc.data, &drawInfo, sizeof(drawInfo));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
			alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			predraw(cmd);
			commandlist.render_encoder->drawMeshThreadgroups(objectThreadgroupCount, MTL::Size::Make(objectThreadgroupSize, 1, 1), MTL::Size::Make(meshThreadgroupSize, 1, 1));
			return;
		}
		else if (commandlist.drawargs_required)
		{
			commandlist.dirty_drawargs = true;
			IRRuntimeDrawIndexedArgument da = {
				.indexCountPerInstance = indexCount,
				.instanceCount = instanceCount,
				.startIndexLocation = startIndexLocation,
				.baseVertexLocation = baseVertexLocation,
				.startInstanceLocation = startInstanceLocation
			};
			IRRuntimeDrawParams dp = { .drawIndexed = da };
			auto alloc = AllocateGPU(sizeof(dp), cmd);
			std::memcpy(alloc.data, &dp, sizeof(dp));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferDrawArgumentsBindPoint);
			alloc = AllocateGPU(sizeof(uint16_t), cmd);
			uint16_t irindextype = IRMetalIndexToIRIndex(commandlist.index_type);
			std::memcpy(alloc.data, &irindextype, sizeof(uint16_t));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}
		
		predraw(cmd);
		commandlist.render_encoder->drawIndexedPrimitives(commandlist.primitive_type, indexCount, commandlist.index_type, commandlist.index_buffer.bufferAddress + indexBufferOffset, commandlist.index_buffer.length, instanceCount, baseVertexLocation, startInstanceLocation);
	}
	void GraphicsDevice_Metal::DrawInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		auto internal_state = to_internal(args);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		
		SDL_assert(commandlist.gs_desc.basePipelineDescriptor == nullptr); // indirect geometry shader emulation not supported because instance count is not available to compute emulation info
		
		if (commandlist.drawargs_required)
		{
			commandlist.dirty_drawargs = true;
			commandlist.argument_table->setAddress(internal_state->gpu_address + args_offset, kIRArgumentBufferDrawArgumentsBindPoint);
			auto alloc = AllocateGPU(sizeof(uint16_t), cmd);
			std::memcpy(alloc.data, &kIRNonIndexedDraw, sizeof(uint16_t));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}
		
		predraw(cmd);
		commandlist.render_encoder->drawPrimitives(commandlist.primitive_type, internal_state->gpu_address + args_offset);
	}
	void GraphicsDevice_Metal::DrawIndexedInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		auto internal_state = to_internal(args);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		
		SDL_assert(commandlist.gs_desc.basePipelineDescriptor == nullptr); // indirect geometry shader emulation not supported because instance count is not available to compute emulation info
		
		if (commandlist.drawargs_required)
		{
			commandlist.dirty_drawargs = true;
			commandlist.argument_table->setAddress(internal_state->gpu_address + args_offset, kIRArgumentBufferDrawArgumentsBindPoint);
			auto alloc = AllocateGPU(sizeof(uint16_t), cmd);
			uint16_t irindextype = IRMetalIndexToIRIndex(commandlist.index_type);
			std::memcpy(alloc.data, &irindextype, sizeof(uint16_t));
			commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}
		
		predraw(cmd);
		commandlist.render_encoder->drawIndexedPrimitives(commandlist.primitive_type, commandlist.index_type, commandlist.index_buffer.bufferAddress, commandlist.index_buffer.length, internal_state->gpu_address + args_offset);
	}
	void GraphicsDevice_Metal::DrawInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd)
	{
		if (!wiGraphicsGPUResourceIsValid(args) || !wiGraphicsGPUResourceIsValid(count) || max_count == 0)
			return;

		CommandList_Metal& commandlist = GetCommandList(cmd);
		const PipelineState* saved_active_pso = commandlist.active_pso;
		if (!wiGraphicsPipelineStateIsValid(saved_active_pso))
		{
			METAL_LOG_ERROR("[Wicked::Metal] DrawInstancedIndirectCount requires a valid bound pipeline state before encoding indirect commands");
			SDL_assert(false);
			return;
		}
		if (commandlist.gs_desc.basePipelineDescriptor != nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DrawInstancedIndirectCount with geometry shader emulation is not supported for GPU-encoded ICB path");
			SDL_assert(false);
			return;
		}
		if (commandlist.render_encoder.get() == nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DrawInstancedIndirectCount requires an active render pass");
			SDL_assert(false);
			return;
		}
		if (!EnsureDrawCountICBResources(cmd, false, max_count))
		{
			SDL_assert(false);
			return;
		}
		bool resume_with_load = true;
		if (!commandlist.active_renderpass_has_draws && commandlist.active_renderpass_occlusionqueries == nullptr)
		{
			commandlist.render_encoder->endEncoding();
			commandlist.render_encoder.reset();
			commandlist.render_encoder = nullptr;
			commandlist.dirty_pso = true;
			resume_with_load = false;
		}
		else if (!EndRenderPassForIndirectEncoding(cmd))
		{
			return;
		}

		bool encode_success = false;
		predispatch(cmd);
		do
		{
			CommandList_Metal& dispatch_commandlist = GetCommandList(cmd);
			NS::SharedPtr<MTL::ArgumentEncoder> icb_argument_encoder = NS::TransferPtr(drawcount_icb_encoder.draw_function->newArgumentEncoder(METAL_ICB_CONTAINER_BUFFER_INDEX));
			if (icb_argument_encoder.get() == nullptr)
			{
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create draw-count ICB argument encoder");
				break;
			}
			icb_argument_encoder->setArgumentBuffer(dispatch_commandlist.draw_count_icb.icb_argument_buffer.get(), 0);
			icb_argument_encoder->setIndirectCommandBuffer(dispatch_commandlist.draw_count_icb.icb.get(), METAL_ICB_CONTAINER_ID);

			dispatch_commandlist.compute_encoder->setComputePipelineState(drawcount_icb_encoder.draw_pipeline.get());

			MetalICBDrawCountEncodeParams params = {};
			params.maxCount = max_count;
			params.primitiveType = (uint32_t)dispatch_commandlist.primitive_type;
			params.drawArgsStride = (uint32_t)sizeof(IRRuntimeDrawArgument);
			params.drawArgsBindIndex = (uint32_t)kIRArgumentBufferDrawArgumentsBindPoint;
			// Always bind per-command draw params in the ICB path.
			// This keeps command-local baseVertex/baseInstance semantics deterministic even if
			// shader reflection doesn't mark needs_draw_params for a converted shader variant.
			params.needsDrawParams = 1u;
			auto params_alloc = AllocateGPU(sizeof(params), cmd);
			std::memcpy(params_alloc.data, &params, sizeof(params));

			auto args_internal = to_internal(args);
			auto count_internal = to_internal(count);
			dispatch_commandlist.argument_table->setAddress(args_internal->gpu_address + args_offset, METAL_ICB_BIND_ARGS);
			dispatch_commandlist.argument_table->setAddress(count_internal->gpu_address + count_offset, METAL_ICB_BIND_COUNT);
			dispatch_commandlist.argument_table->setAddress(dispatch_commandlist.draw_count_icb.icb_argument_buffer->gpuAddress(), METAL_ICB_BIND_CONTAINER);
			dispatch_commandlist.argument_table->setAddress(to_internal(&params_alloc.buffer)->gpu_address + params_alloc.offset, METAL_ICB_BIND_PARAMS);
			dispatch_commandlist.argument_table->setAddress(dispatch_commandlist.draw_count_icb.icb_execution_range_buffer->gpuAddress(), METAL_ICB_BIND_EXEC_RANGE);
			dispatch_commandlist.compute_encoder->setArgumentTable(dispatch_commandlist.argument_table.get());

			uint32_t threads_per_group = (uint32_t)drawcount_icb_encoder.draw_pipeline->threadExecutionWidth();
			threads_per_group = std::max(1u, threads_per_group);
			threads_per_group = std::min(threads_per_group, (uint32_t)drawcount_icb_encoder.draw_pipeline->maxTotalThreadsPerThreadgroup());
			const uint32_t group_count = (max_count + threads_per_group - 1) / threads_per_group;
			dispatch_commandlist.compute_encoder->dispatchThreadgroups(MTL::Size::Make(group_count, 1, 1), MTL::Size::Make(threads_per_group, 1, 1));
			dispatch_commandlist.compute_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone);
			encode_success = true;
		} while (false);

		if (!ResumeRenderPassAfterIndirectEncoding(cmd, resume_with_load))
		{
			return;
		}
		if (!encode_success)
		{
			SDL_assert(false);
			return;
		}

		CommandList_Metal& resume_commandlist = GetCommandList(cmd);
		resume_commandlist.active_pso = saved_active_pso;
		resume_commandlist.dirty_drawargs = true;
		{
			auto alloc = AllocateGPU(sizeof(uint16_t), cmd);
			std::memcpy(alloc.data, &kIRNonIndexedDraw, sizeof(uint16_t));
			resume_commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}

		predraw(cmd);
		resume_commandlist.render_encoder->executeCommandsInBuffer(
			resume_commandlist.draw_count_icb.icb.get(),
			resume_commandlist.draw_count_icb.icb_execution_range_buffer->gpuAddress());
	}
	void GraphicsDevice_Metal::DrawIndexedInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd)
	{
		if (!wiGraphicsGPUResourceIsValid(args) || !wiGraphicsGPUResourceIsValid(count) || max_count == 0)
			return;

		CommandList_Metal& commandlist = GetCommandList(cmd);
		const PipelineState* saved_active_pso = commandlist.active_pso;
		if (!wiGraphicsPipelineStateIsValid(saved_active_pso))
		{
			METAL_LOG_ERROR("[Wicked::Metal] DrawIndexedInstancedIndirectCount requires a valid bound pipeline state before encoding indirect commands");
			SDL_assert(false);
			return;
		}
		if (commandlist.gs_desc.basePipelineDescriptor != nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DrawIndexedInstancedIndirectCount with geometry shader emulation is not supported for GPU-encoded ICB path");
			SDL_assert(false);
			return;
		}
		if (commandlist.render_encoder.get() == nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DrawIndexedInstancedIndirectCount requires an active render pass");
			SDL_assert(false);
			return;
		}
		if (commandlist.index_buffer.bufferAddress == 0 || commandlist.index_buffer.length == 0)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DrawIndexedInstancedIndirectCount requires a bound index buffer");
			SDL_assert(false);
			return;
		}
		if (!EnsureDrawCountICBResources(cmd, true, max_count))
		{
			SDL_assert(false);
			return;
		}
		bool resume_with_load = true;
		if (!commandlist.active_renderpass_has_draws && commandlist.active_renderpass_occlusionqueries == nullptr)
		{
			commandlist.render_encoder->endEncoding();
			commandlist.render_encoder.reset();
			commandlist.render_encoder = nullptr;
			commandlist.dirty_pso = true;
			resume_with_load = false;
		}
		else if (!EndRenderPassForIndirectEncoding(cmd))
		{
			return;
		}

		bool encode_success = false;
		predispatch(cmd);
		do
		{
			CommandList_Metal& dispatch_commandlist = GetCommandList(cmd);
			NS::SharedPtr<MTL::ArgumentEncoder> icb_argument_encoder = NS::TransferPtr(drawcount_icb_encoder.draw_indexed_function->newArgumentEncoder(METAL_ICB_CONTAINER_BUFFER_INDEX));
			if (icb_argument_encoder.get() == nullptr)
			{
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create indexed draw-count ICB argument encoder");
				break;
			}
			icb_argument_encoder->setArgumentBuffer(dispatch_commandlist.draw_indexed_count_icb.icb_argument_buffer.get(), 0);
			icb_argument_encoder->setIndirectCommandBuffer(dispatch_commandlist.draw_indexed_count_icb.icb.get(), METAL_ICB_CONTAINER_ID);

			dispatch_commandlist.compute_encoder->setComputePipelineState(drawcount_icb_encoder.draw_indexed_pipeline.get());

			MetalICBDrawCountEncodeParams params = {};
			params.maxCount = max_count;
			params.primitiveType = (uint32_t)dispatch_commandlist.primitive_type;
			params.indexType = (uint32_t)dispatch_commandlist.index_type;
			params.drawArgsStride = (uint32_t)sizeof(IRRuntimeDrawIndexedArgument);
			params.drawArgsBindIndex = (uint32_t)kIRArgumentBufferDrawArgumentsBindPoint;
			// Always bind per-command draw params in the ICB path.
			// This keeps command-local baseVertex/baseInstance semantics deterministic even if
			// shader reflection doesn't mark needs_draw_params for a converted shader variant.
			params.needsDrawParams = 1u;
			auto params_alloc = AllocateGPU(sizeof(params), cmd);
			std::memcpy(params_alloc.data, &params, sizeof(params));

			auto args_internal = to_internal(args);
			auto count_internal = to_internal(count);
			dispatch_commandlist.argument_table->setAddress(args_internal->gpu_address + args_offset, METAL_ICB_BIND_ARGS);
			dispatch_commandlist.argument_table->setAddress(count_internal->gpu_address + count_offset, METAL_ICB_BIND_COUNT);
			dispatch_commandlist.argument_table->setAddress(dispatch_commandlist.draw_indexed_count_icb.icb_argument_buffer->gpuAddress(), METAL_ICB_BIND_CONTAINER);
			dispatch_commandlist.argument_table->setAddress(to_internal(&params_alloc.buffer)->gpu_address + params_alloc.offset, METAL_ICB_BIND_PARAMS);
			dispatch_commandlist.argument_table->setAddress(dispatch_commandlist.index_buffer.bufferAddress, METAL_ICB_BIND_INDEXBUFFER);
			dispatch_commandlist.argument_table->setAddress(dispatch_commandlist.draw_indexed_count_icb.icb_execution_range_buffer->gpuAddress(), METAL_ICB_BIND_EXEC_RANGE);
			dispatch_commandlist.compute_encoder->setArgumentTable(dispatch_commandlist.argument_table.get());

			uint32_t threads_per_group = (uint32_t)drawcount_icb_encoder.draw_indexed_pipeline->threadExecutionWidth();
			threads_per_group = std::max(1u, threads_per_group);
			threads_per_group = std::min(threads_per_group, (uint32_t)drawcount_icb_encoder.draw_indexed_pipeline->maxTotalThreadsPerThreadgroup());
			const uint32_t group_count = (max_count + threads_per_group - 1) / threads_per_group;
			dispatch_commandlist.compute_encoder->dispatchThreadgroups(MTL::Size::Make(group_count, 1, 1), MTL::Size::Make(threads_per_group, 1, 1));
			dispatch_commandlist.compute_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone);
			encode_success = true;
		} while (false);

		if (!ResumeRenderPassAfterIndirectEncoding(cmd, resume_with_load))
		{
			return;
		}
		if (!encode_success)
		{
			SDL_assert(false);
			return;
		}

		CommandList_Metal& resume_commandlist = GetCommandList(cmd);
		resume_commandlist.active_pso = saved_active_pso;
		resume_commandlist.dirty_drawargs = true;
		{
			auto alloc = AllocateGPU(sizeof(uint16_t), cmd);
			uint16_t irindextype = IRMetalIndexToIRIndex(resume_commandlist.index_type);
			std::memcpy(alloc.data, &irindextype, sizeof(uint16_t));
			resume_commandlist.argument_table->setAddress(to_internal(&alloc.buffer)->gpu_address + alloc.offset, kIRArgumentBufferUniformsBindPoint);
		}

		predraw(cmd);
		resume_commandlist.render_encoder->executeCommandsInBuffer(
			resume_commandlist.draw_indexed_count_icb.icb.get(),
			resume_commandlist.draw_indexed_count_icb.icb_execution_range_buffer->gpuAddress());
	}
	void GraphicsDevice_Metal::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ, CommandList cmd)
	{
		predispatch(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		auto cs_internal = to_internal(commandlist.active_cs);
		commandlist.compute_encoder->dispatchThreadgroups({threadGroupCountX, threadGroupCountY, threadGroupCountZ}, cs_internal->additional_data.numthreads);
	}
	void GraphicsDevice_Metal::DispatchIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		predispatch(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		auto cs_internal = to_internal(commandlist.active_cs);
		auto internal_state = to_internal(args);
		commandlist.compute_encoder->dispatchThreadgroups(internal_state->gpu_address + args_offset, cs_internal->additional_data.numthreads);
	}
	void GraphicsDevice_Metal::DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ, CommandList cmd)
	{
		predraw(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.render_encoder->drawMeshThreadgroups({threadGroupCountX, threadGroupCountY, threadGroupCountZ}, commandlist.numthreads_as, commandlist.numthreads_ms);
	}
	void GraphicsDevice_Metal::DispatchMeshIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd)
	{
		predraw(cmd);
		auto internal_state = to_internal(args);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		commandlist.render_encoder->drawMeshThreadgroups(internal_state->gpu_address + args_offset, commandlist.numthreads_as, commandlist.numthreads_ms);
	}
	void GraphicsDevice_Metal::DispatchMeshIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd)
	{
		if (!wiGraphicsGPUResourceIsValid(args) || !wiGraphicsGPUResourceIsValid(count) || max_count == 0)
			return;

		CommandList_Metal& commandlist = GetCommandList(cmd);
		const PipelineState* saved_active_pso = commandlist.active_pso;
		if (!wiGraphicsPipelineStateIsValid(saved_active_pso))
		{
			METAL_LOG_ERROR("[Wicked::Metal] DispatchMeshIndirectCount requires a valid bound pipeline state before encoding indirect commands");
			SDL_assert(false);
			return;
		}
		if (saved_active_pso->desc.ms == nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DispatchMeshIndirectCount requires a mesh shader pipeline");
			SDL_assert(false);
			return;
		}
		if (commandlist.gs_desc.basePipelineDescriptor != nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DispatchMeshIndirectCount with geometry shader emulation is not supported for GPU-encoded ICB path");
			SDL_assert(false);
			return;
		}
		if (commandlist.render_encoder.get() == nullptr)
		{
			METAL_LOG_ERROR("[Wicked::Metal] DispatchMeshIndirectCount requires an active render pass");
			SDL_assert(false);
			return;
		}
		if (!EnsureMeshCountICBResources(cmd, max_count))
		{
			SDL_assert(false);
			return;
		}
		bool resume_with_load = true;
		if (!commandlist.active_renderpass_has_draws && commandlist.active_renderpass_occlusionqueries == nullptr)
		{
			commandlist.render_encoder->endEncoding();
			commandlist.render_encoder.reset();
			commandlist.render_encoder = nullptr;
			commandlist.dirty_pso = true;
			resume_with_load = false;
		}
		else if (!EndRenderPassForIndirectEncoding(cmd))
		{
			return;
		}

		bool encode_success = false;
		predispatch(cmd);
		do
		{
			CommandList_Metal& dispatch_commandlist = GetCommandList(cmd);
			NS::SharedPtr<MTL::ArgumentEncoder> icb_argument_encoder = NS::TransferPtr(drawcount_icb_encoder.draw_mesh_function->newArgumentEncoder(METAL_ICB_CONTAINER_BUFFER_INDEX));
			if (icb_argument_encoder.get() == nullptr)
			{
				METAL_LOG_ERROR("[Wicked::Metal] Failed to create mesh draw-count ICB argument encoder");
				break;
			}
			icb_argument_encoder->setArgumentBuffer(dispatch_commandlist.draw_mesh_count_icb.icb_argument_buffer.get(), 0);
			icb_argument_encoder->setIndirectCommandBuffer(dispatch_commandlist.draw_mesh_count_icb.icb.get(), METAL_ICB_CONTAINER_ID);

			dispatch_commandlist.compute_encoder->setComputePipelineState(drawcount_icb_encoder.draw_mesh_pipeline.get());

			MetalICBDrawCountEncodeParams params = {};
			params.maxCount = max_count;
			params.drawArgsStride = sizeof(uint32_t) * 3u; // IndirectDispatchArgs
			params.objectThreadgroupSizeX = (uint32_t)dispatch_commandlist.numthreads_as.width;
			params.objectThreadgroupSizeY = (uint32_t)dispatch_commandlist.numthreads_as.height;
			params.objectThreadgroupSizeZ = (uint32_t)dispatch_commandlist.numthreads_as.depth;
			params.meshThreadgroupSizeX = (uint32_t)dispatch_commandlist.numthreads_ms.width;
			params.meshThreadgroupSizeY = (uint32_t)dispatch_commandlist.numthreads_ms.height;
			params.meshThreadgroupSizeZ = (uint32_t)dispatch_commandlist.numthreads_ms.depth;
			auto params_alloc = AllocateGPU(sizeof(params), cmd);
			std::memcpy(params_alloc.data, &params, sizeof(params));

			auto args_internal = to_internal(args);
			auto count_internal = to_internal(count);
			dispatch_commandlist.argument_table->setAddress(args_internal->gpu_address + args_offset, METAL_ICB_BIND_ARGS);
			dispatch_commandlist.argument_table->setAddress(count_internal->gpu_address + count_offset, METAL_ICB_BIND_COUNT);
			dispatch_commandlist.argument_table->setAddress(dispatch_commandlist.draw_mesh_count_icb.icb_argument_buffer->gpuAddress(), METAL_ICB_BIND_CONTAINER);
			dispatch_commandlist.argument_table->setAddress(to_internal(&params_alloc.buffer)->gpu_address + params_alloc.offset, METAL_ICB_BIND_PARAMS);
			dispatch_commandlist.argument_table->setAddress(dispatch_commandlist.draw_mesh_count_icb.icb_execution_range_buffer->gpuAddress(), METAL_ICB_BIND_EXEC_RANGE);
			dispatch_commandlist.compute_encoder->setArgumentTable(dispatch_commandlist.argument_table.get());

			uint32_t threads_per_group = (uint32_t)drawcount_icb_encoder.draw_mesh_pipeline->threadExecutionWidth();
			threads_per_group = std::max(1u, threads_per_group);
			threads_per_group = std::min(threads_per_group, (uint32_t)drawcount_icb_encoder.draw_mesh_pipeline->maxTotalThreadsPerThreadgroup());
			const uint32_t group_count = (max_count + threads_per_group - 1) / threads_per_group;
			dispatch_commandlist.compute_encoder->dispatchThreadgroups(MTL::Size::Make(group_count, 1, 1), MTL::Size::Make(threads_per_group, 1, 1));
			dispatch_commandlist.compute_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone);
			encode_success = true;
		} while (false);

		if (!ResumeRenderPassAfterIndirectEncoding(cmd, resume_with_load))
		{
			return;
		}
		if (!encode_success)
		{
			SDL_assert(false);
			return;
		}

		CommandList_Metal& resume_commandlist = GetCommandList(cmd);
		resume_commandlist.active_pso = saved_active_pso;

		predraw(cmd);
		resume_commandlist.render_encoder->executeCommandsInBuffer(
			resume_commandlist.draw_mesh_count_icb.icb.get(),
			resume_commandlist.draw_mesh_count_icb.icb_execution_range_buffer->gpuAddress());
	}
	void GraphicsDevice_Metal::CopyResource(const GPUResource* pDst, const GPUResource* pSrc, CommandList cmd)
	{
		precopy(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (wiGraphicsGPUResourceIsTexture(pDst) && wiGraphicsGPUResourceIsTexture(pSrc))
		{
			const Texture& srctex = *(Texture*)pSrc;
			const Texture& dsttex = *(Texture*)pDst;
			auto src_internal = to_internal<Texture>(pSrc);
			auto dst_internal = to_internal<Texture>(pDst);
			if (src_internal->texture.get() != nullptr && dst_internal->texture.get() != nullptr)
			{
				// Normal texture->texture copy
				commandlist.compute_encoder->copyFromTexture(src_internal->texture.get(), dst_internal->texture.get());
			}
			else if(src_internal->texture.get() != nullptr && dst_internal->buffer.get() != nullptr)
			{
				// Texture->linear copy:
				const uint64_t data_begin = (uint64_t)dsttex.mapped_subresources[0].data_ptr;
				uint32_t subresource_index = 0;
				for (uint32_t slice = 0; slice < srctex.desc.array_size; ++slice)
				{
					for (uint32_t mip = 0; mip < srctex.desc.mip_levels; ++mip)
					{
						const uint32_t mip_width = std::max(1u, srctex.desc.width >> mip);
						const uint32_t mip_height = std::max(1u, srctex.desc.height >> mip);
						const uint32_t mip_depth = std::max(1u, srctex.desc.depth >> mip);
						const SubresourceData& subresource_data = dsttex.mapped_subresources[subresource_index++];
						uint64_t data_offset = uint64_t((uint64_t)subresource_data.data_ptr - data_begin);
						commandlist.compute_encoder->copyFromTexture(src_internal->texture.get(), slice, mip, {0,0,0}, {mip_width, mip_height, mip_depth}, dst_internal->buffer.get(), data_offset, subresource_data.row_pitch, subresource_data.slice_pitch * mip_depth);
					}
				}
			}
			else if(src_internal->texture.get() != nullptr && dst_internal->buffer.get() != nullptr)
			{
				// Linear->texture copy:
				const uint64_t data_begin = (uint64_t)srctex.mapped_subresources[0].data_ptr;
				uint32_t subresource_index = 0;
				for (uint32_t slice = 0; slice < dsttex.desc.array_size; ++slice)
				{
					for (uint32_t mip = 0; mip < dsttex.desc.mip_levels; ++mip)
					{
						const uint32_t mip_width = std::max(1u, dsttex.desc.width >> mip);
						const uint32_t mip_height = std::max(1u, dsttex.desc.height >> mip);
						const uint32_t mip_depth = std::max(1u, dsttex.desc.depth >> mip);
						const SubresourceData& subresource_data = srctex.mapped_subresources[subresource_index++];
						uint64_t data_offset = uint64_t((uint64_t)subresource_data.data_ptr - data_begin);
						commandlist.compute_encoder->copyFromBuffer(src_internal->buffer.get(), data_offset, subresource_data.row_pitch, subresource_data.slice_pitch * mip_depth, {mip_width,mip_height,mip_depth}, dst_internal->texture.get(), slice, mip, {0,0,0});
					}
				}
			}
		}
		else if (wiGraphicsGPUResourceIsBuffer(pDst) && wiGraphicsGPUResourceIsBuffer(pSrc))
		{
			auto src_internal = to_internal<GPUBuffer>(pSrc);
			auto dst_internal = to_internal<GPUBuffer>(pDst);
			commandlist.compute_encoder->copyFromBuffer(src_internal->buffer.get(), 0, dst_internal->buffer.get(), 0, dst_internal->buffer->length());
		}
	}
	void GraphicsDevice_Metal::CopyBuffer(const GPUBuffer* pDst, uint64_t dst_offset, const GPUBuffer* pSrc, uint64_t src_offset, uint64_t size, CommandList cmd)
	{
		precopy(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		auto internal_state_src = to_internal(pSrc);
		auto internal_state_dst = to_internal(pDst);
		commandlist.compute_encoder->copyFromBuffer(internal_state_src->buffer.get(), src_offset, internal_state_dst->buffer.get(), dst_offset, size);
	}
	void GraphicsDevice_Metal::CopyTexture(const Texture* dst, uint32_t dstX, uint32_t dstY, uint32_t dstZ, uint32_t dstMip, uint32_t dstSlice, const Texture* src, uint32_t srcMip, uint32_t srcSlice, CommandList cmd, const Box* srcbox, ImageAspect dst_aspect, ImageAspect src_aspect)
	{
		precopy(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		const uint32_t srcWidth = std::max(1u, src->desc.width >> srcMip);
		const uint32_t srcHeight = std::max(1u, src->desc.height >> srcMip);
		const uint32_t srcDepth = std::max(1u, src->desc.depth >> srcMip);
		const uint32_t dstWidth = std::max(1u, dst->desc.width >> dstMip);
		const uint32_t dstHeight = std::max(1u, dst->desc.height >> dstMip);
		const uint32_t dstDepth = std::max(1u, dst->desc.depth >> dstMip);
		auto src_internal = to_internal(src);
		auto dst_internal = to_internal(dst);
		MTL::Origin srcOrigin = {};
		MTL::Size srcSize = {};
		MTL::Origin dstOrigin = { dstX, dstY, dstZ };
		if (srcbox == nullptr)
		{
			srcSize.width = srcWidth;
			srcSize.height = srcHeight;
			srcSize.depth = srcDepth;
		}
		else
		{
			srcOrigin.x = srcbox->left;
			srcOrigin.y = srcbox->top;
			srcOrigin.z = srcbox->front;
			srcSize.width = srcbox->right - srcbox->left;
			srcSize.height = srcbox->bottom - srcbox->top;
			srcSize.depth = srcbox->back - srcbox->front;
		}
		
		if (src_internal->texture.get() != nullptr && dst_internal->texture.get() != nullptr)
		{
			// normal texture -> texture copy
			if (dst->desc.format != src->desc.format)
			{
				// Hack:
				//	Metal cannot do format reinterpret texture->texture copy, so instead of rewriting my block compression shaders,
				//	I implement texture->buffer->texture copy as a workaround
				const size_t buffer_size = ComputeTextureMipMemorySizeInBytes(src->desc, srcMip);
				const size_t row_pitch = ComputeTextureMipRowPitch(src->desc, srcMip);
				static NS::SharedPtr<MTL::Buffer> reinterpret_buffer[QUEUE_COUNT]; // one per queue for correct gpu multi-queue hazard safety
				static std::mutex locker;
				std::scoped_lock lck(locker);
				if (reinterpret_buffer[commandlist.queue].get() == nullptr || reinterpret_buffer[commandlist.queue]->length() < buffer_size)
				{
					if (reinterpret_buffer[commandlist.queue].get() != nullptr)
					{
						std::scoped_lock lck2(allocationhandler->destroylocker);
						allocationhandler->Retire(allocationhandler->destroyer_resources, reinterpret_buffer[commandlist.queue]);
					}
					reinterpret_buffer[commandlist.queue] = NS::TransferPtr(device->newBuffer(buffer_size, MTL::ResourceStorageModePrivate));
					reinterpret_buffer[commandlist.queue]->setLabel(NS::TransferPtr(NS::String::alloc()->init("reinterpret_buffer", NS::UTF8StringEncoding)).get());
					allocationhandler->make_resident(reinterpret_buffer[commandlist.queue].get());
				}
				commandlist.compute_encoder->copyFromTexture(src_internal->texture.get(), srcSlice, srcMip, srcOrigin, srcSize, reinterpret_buffer[commandlist.queue].get(), 0, row_pitch, buffer_size);
				commandlist.compute_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone);
				if (!IsFormatBlockCompressed(src->desc.format) && IsFormatBlockCompressed(dst->desc.format))
				{
					// raw -> block compressed copy
					const uint32_t block_size = GetFormatBlockSize(dst->desc.format);
					srcSize.width = std::min((uint32_t)srcSize.width * block_size, dstWidth);
					srcSize.height = std::min((uint32_t)srcSize.height * block_size, dstHeight);
				}
				if (IsFormatBlockCompressed(src->desc.format) && !IsFormatBlockCompressed(dst->desc.format))
				{
					// block compressed -> raw copy
					const uint32_t block_size = GetFormatBlockSize(dst->desc.format);
					srcSize.width = std::max(1u, (uint32_t)srcSize.width / block_size);
					srcSize.height = std::max(1u, (uint32_t)srcSize.height / block_size);
				}
				commandlist.compute_encoder->copyFromBuffer(reinterpret_buffer[commandlist.queue].get(), 0, row_pitch, buffer_size, srcSize, dst_internal->texture.get(), dstSlice, dstMip, dstOrigin);
				commandlist.compute_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionNone);
			}
			else
			{
				commandlist.compute_encoder->copyFromTexture(src_internal->texture.get(), srcSlice, srcMip, srcOrigin, srcSize, dst_internal->texture.get(), dstSlice, dstMip, dstOrigin);
			}
		}
		else if (src_internal->texture.get() != nullptr && dst_internal->buffer.get() != nullptr)
		{
			// texture -> linear copy
			const uint32_t subresource = ComputeSubresource(dstMip, dstSlice, dst_aspect, dst->desc.mip_levels, dst->desc.array_size);
			SDL_assert(dst->mapped_subresource_count > subresource);
			const SubresourceData& data0 = dst->mapped_subresources[0];
			const SubresourceData& data = dst->mapped_subresources[subresource];
			const uint64_t dst_offset = (uint64_t)data.data_ptr - (uint64_t)data0.data_ptr;
			commandlist.compute_encoder->copyFromTexture(src_internal->texture.get(), srcSlice, srcMip, srcOrigin, srcSize, dst_internal->buffer.get(), dst_offset, data.row_pitch, data.row_pitch * dstHeight * dstDepth);
		}
		else if (src_internal->buffer.get() != nullptr && dst_internal->texture.get() != nullptr)
		{
			// linear -> texture copy
			const uint32_t subresource = ComputeSubresource(srcMip, srcSlice, src_aspect, src->desc.mip_levels, src->desc.array_size);
			SDL_assert(src->mapped_subresource_count > subresource);
			const SubresourceData& data0 = src->mapped_subresources[0];
			const SubresourceData& data = src->mapped_subresources[subresource];
			uint64_t src_offset = (uint64_t)data.data_ptr - (uint64_t)data0.data_ptr;
			src_offset += srcbox->top * data.row_pitch + srcbox->left * GetFormatStride(src->desc.format) / GetFormatBlockSize(src->desc.format);
			commandlist.compute_encoder->copyFromBuffer(src_internal->buffer.get(), src_offset, data.row_pitch, data.row_pitch * srcHeight * srcDepth, srcSize, dst_internal->texture.get(), dstSlice, dstMip, dstOrigin);
		}
		else
		{
			SDL_assert(0); // not implemented
		}
	}
	void GraphicsDevice_Metal::QueryBegin(const GPUQueryHeap* heap, uint32_t index, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		switch (heap->desc.type)
		{
			case GpuQueryType::OCCLUSION:
				SDL_assert(commandlist.render_encoder.get() != nullptr);
				commandlist.render_encoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting, index * sizeof(uint64_t));
				break;
			case GpuQueryType::OCCLUSION_BINARY:
				SDL_assert(commandlist.render_encoder.get() != nullptr);
				commandlist.render_encoder->setVisibilityResultMode(MTL::VisibilityResultModeBoolean, index * sizeof(uint64_t));
				break;
			case GpuQueryType::TIMESTAMP:
				break;
			default:
				break;
		}
	}
	void GraphicsDevice_Metal::QueryEnd(const GPUQueryHeap* heap, uint32_t index, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		auto internal_state = to_internal(heap);
		switch (heap->desc.type)
		{
			case GpuQueryType::OCCLUSION:
			case GpuQueryType::OCCLUSION_BINARY:
				SDL_assert(commandlist.render_encoder.get() != nullptr);
				commandlist.render_encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, index * sizeof(uint64_t));
				break;
			case GpuQueryType::TIMESTAMP:
				if (commandlist.render_encoder.get() != nullptr)
				{
					// Note: fMTL::RenderStageFragment timestamp is unreliable if no fragment was rendered. But we can't determine here in a non-intrusive way whether a fragment was rendered or not in the current render pass for this timestamp
					commandlist.render_encoder->writeTimestamp(MTL4::TimestampGranularityPrecise, MTL::RenderStageFragment, internal_state->counter_heap.get(), index);
				}
				else if (commandlist.compute_encoder.get() != nullptr)
				{
					commandlist.compute_encoder->writeTimestamp(MTL4::TimestampGranularityPrecise, internal_state->counter_heap.get(), index);
				}
				else
				{
					commandlist.commandbuffer->writeTimestampIntoHeap(internal_state->counter_heap.get(), index);
				}
				break;
			default:
				break;
		}
	}
	void GraphicsDevice_Metal::QueryResolve(const GPUQueryHeap* heap, uint32_t index, uint32_t count, const GPUBuffer* dest, uint64_t dest_offset, CommandList cmd)
	{
		if (count == 0)
			return;
		CommandList_Metal& commandlist = GetCommandList(cmd);

		auto internal_state = to_internal(heap);
		auto dst_internal = to_internal(dest);
		
		switch (heap->desc.type)
		{
			case GpuQueryType::OCCLUSION:
			case GpuQueryType::OCCLUSION_BINARY:
				precopy(cmd);
				commandlist.compute_encoder->copyFromBuffer(internal_state->buffer.get(), index * sizeof(uint64_t), dst_internal->buffer.get(), dest_offset, count * sizeof(uint64_t));
				break;
			case GpuQueryType::TIMESTAMP:
				if (commandlist.compute_encoder.get() != nullptr)
				{
					commandlist.compute_encoder->endEncoding();
					commandlist.compute_encoder.reset();
				}
				commandlist.commandbuffer->resolveCounterHeap(internal_state->counter_heap.get(), {index, count}, {dst_internal->gpu_address, count * sizeof(uint64_t)}, nullptr, nullptr);
				break;
			default:
				break;
		}
	}
	void GraphicsDevice_Metal::Barrier(const GPUBarrier* barriers, uint32_t numBarriers, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		for (uint32_t i = 0; i < numBarriers; ++i)
		{
			const GPUBarrier& barrier = barriers[i];
			arrput(commandlist.barriers, barrier);
		}
	}
	void GraphicsDevice_Metal::BuildRaytracingAccelerationStructure(const RaytracingAccelerationStructure* dst, CommandList cmd, const RaytracingAccelerationStructure* src)
	{
		predispatch(cmd);
		CommandList_Metal& commandlist = GetCommandList(cmd);
		auto dst_internal = to_internal(dst);
		
		// descriptor is recreated because buffer references might have changed since creation:
		NS::SharedPtr<MTL4::AccelerationStructureDescriptor> descriptor = mtl_acceleration_structure_descriptor(&dst->desc);
		
		if (src != nullptr && (dst->desc.flags & RaytracingAccelerationStructureDesc::FLAG_ALLOW_UPDATE))
		{
			auto src_internal = to_internal(src);
			commandlist.compute_encoder->refitAccelerationStructure(src_internal->acceleration_structure.get(), descriptor.get(), dst_internal->acceleration_structure.get(), {dst_internal->scratch->gpuAddress(), dst_internal->scratch->length()});
		}
		else
		{
			commandlist.compute_encoder->buildAccelerationStructure(dst_internal->acceleration_structure.get(), descriptor.get(), {dst_internal->scratch->gpuAddress(), dst_internal->scratch->length()});
		}
	}
	void GraphicsDevice_Metal::BindRaytracingPipelineState(const RaytracingPipelineState* rtpso, CommandList cmd)
	{
		// TODO
	}
	void GraphicsDevice_Metal::DispatchRays(const DispatchRaysDesc* desc, CommandList cmd)
	{
		// TODO
	}
	void GraphicsDevice_Metal::PushConstants(const void* data, uint32_t size, CommandList cmd, uint32_t offset)
	{
		SDL_assert(offset + size < sizeof(RootLayout::constants));
		CommandList_Metal& commandlist = GetCommandList(cmd);
		std::memcpy((uint8_t*)commandlist.root.constants + offset, data, size);
		commandlist.dirty_root = true;
	}
	void GraphicsDevice_Metal::PredicationBegin(const GPUBuffer* buffer, uint64_t offset, PredicationOp op, CommandList cmd)
	{
		// This is not supported in Metal API
	}
	void GraphicsDevice_Metal::PredicationEnd(CommandList cmd)
	{
		// This is not supported in Metal API
	}
	void GraphicsDevice_Metal::ClearUAV(const GPUResource* resource, uint32_t value, CommandList cmd)
	{
		// Note: the clears with Metal API don't always match up with the uint32_t clear value that is provided to this function, but in the usual case for clearing to 0 or some common values it will work
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (wiGraphicsGPUResourceIsTexture(resource))
		{
			auto internal_state = to_internal<Texture>(resource);
			bool found = false;
			for (size_t i = 0; i < arrlenu(commandlist.texture_clears); ++i)
			{
				auto& x = commandlist.texture_clears[i];
				if (x.texture == internal_state->texture.get())
				{
					// Avoid adding batched clear command twice
					found = true;
					break;
				}
			}
			if (!found)
			{
				auto& entry = arrput(commandlist.texture_clears, {});
				entry.texture = internal_state->texture.get();
				entry.texture->retain();
				entry.value = value;
			}
		}
		else if (wiGraphicsGPUResourceIsBuffer(resource))
		{
			precopy(cmd);
			auto internal_state = to_internal<GPUBuffer>(resource);
			commandlist.compute_encoder->fillBuffer(internal_state->buffer.get(), {0, internal_state->buffer->length()}, (uint8_t)value);
		}
	}
	void GraphicsDevice_Metal::VideoDecode(const VideoDecoder* video_decoder, const VideoDecodeOperation* op, CommandList cmd)
	{
	}

	void GraphicsDevice_Metal::EventBegin(const char* name, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		NS::SharedPtr<NS::String> str = NS::TransferPtr(NS::String::alloc()->init(name, NS::UTF8StringEncoding));
		if (commandlist.render_encoder.get() != nullptr)
		{
			commandlist.render_encoder->pushDebugGroup(str.get());
		}
		else if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->pushDebugGroup(str.get());
		}
		else
		{
			commandlist.commandbuffer->pushDebugGroup(str.get());
		}
	}
	void GraphicsDevice_Metal::EventEnd(CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		if (commandlist.render_encoder.get() != nullptr)
		{
			commandlist.render_encoder->popDebugGroup();
		}
		else if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->popDebugGroup();
		}
		else
		{
			commandlist.commandbuffer->popDebugGroup();
		}
	}
	void GraphicsDevice_Metal::SetMarker(const char* name, CommandList cmd)
	{
		CommandList_Metal& commandlist = GetCommandList(cmd);
		NS::SharedPtr<NS::String> str = NS::TransferPtr(NS::String::alloc()->init(name, NS::UTF8StringEncoding));
		if (commandlist.render_encoder.get() != nullptr)
		{
			commandlist.render_encoder->setLabel(str.get());
		}
		else if (commandlist.compute_encoder.get() != nullptr)
		{
			commandlist.compute_encoder->setLabel(str.get());
		}
		else
		{
			commandlist.commandbuffer->setLabel(str.get());
		}
	}
}

#endif // __APPLE__
