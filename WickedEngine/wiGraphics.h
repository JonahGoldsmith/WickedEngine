#pragma once
#include "CommonInclude.h"
#include "wiAllocator.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include "../stb_ds.h"

#ifndef WI_STB_ARRAY_HELPERS_DEFINED
#define WI_STB_ARRAY_HELPERS_DEFINED

#undef arrlen
#undef arrlenu
#undef arrcap
#undef arrsetcap
#undef arrsetlen
#undef arrput
#undef arrpop

#define wi_stb_arrlen(a) ((a) != nullptr ? stbds_header(a)->length : 0u)
#define wi_stb_arrcap(a) ((a) != nullptr ? stbds_header(a)->capacity : 0u)

#if defined(__cplusplus)
#define WI_STB_ARRAY_CAST(a, value) reinterpret_cast<std::remove_reference_t<decltype(a)>>(value)
#else
#define WI_STB_ARRAY_CAST(a, value) (value)
#endif

#define arrlen(a) wi_stb_arrlen(a)
#define arrlenu(a) wi_stb_arrlen(a)
#define arrcap(a) wi_stb_arrcap(a)
#define arrsetcap(a, n) (wi_stb_arrcap(a) < (size_t)(n) ? ((a) = WI_STB_ARRAY_CAST((a), stbds_arrgrowf((a), sizeof(*(a)), 0, (size_t)(n))), 0) : 0)
#define arrsetlen(a, n) (arrsetcap((a), (n)), ((a) != nullptr ? (stbds_header(a)->length = (size_t)(n)) : 0))
#define arrput(a, v) (arrsetlen((a), wi_stb_arrlen(a) + 1), (a)[wi_stb_arrlen(a) - 1] = v)
#define arrpop(a) ((a)[--stbds_header(a)->length])
#endif // WI_STB_ARRAY_HELPERS_DEFINED

namespace wi
{
	inline char* CloneCString(const char* value)
	{
		if (value == nullptr)
		{
			return nullptr;
		}
		const size_t len = std::strlen(value);
		char* copy = static_cast<char*>(std::malloc(len + 1));
		if (copy == nullptr)
		{
			return nullptr;
		}
		std::memcpy(copy, value, len + 1);
		return copy;
	}
	inline void DestroyCString(char*& value)
	{
		if (value != nullptr)
		{
			std::free(value);
			value = nullptr;
		}
	}
	inline void SetCString(char*& dst, const char* src)
	{
		DestroyCString(dst);
		dst = CloneCString(src);
	}

	struct Shader;
	struct GPUResource;
	struct GPUBuffer;
	struct Texture;

	enum ValidationMode : uint8_t
	{
		Disabled,	// No validation is enabled
		Enabled,	// CPU command validation
		GPU,		// CPU and GPU-based validation
		Verbose		// Print all warnings, errors and info messages
	};

	enum AdapterType : uint8_t
	{
		Other,
		IntegratedGpu,
		DiscreteGpu,
		VirtualGpu,
		Cpu,
	};

	enum GPUPreference : uint8_t
	{
		Discrete,
		Integrated,
		Nvidia,
		Intel,
		AMD,
	};

	enum ShaderStage : uint8_t
	{
		MS,		// Mesh Shader
		AS,		// Amplification Shader
		VS,		// Vertex Shader
		HS,		// Hull Shader
		DS,		// Domain Shader
		GS,		// Geometry Shader
		PS,		// Pixel Shader
		CS,		// Compute Shader
		LIB,	// Shader Library
		Count,
	};
	enum ShaderFormat : uint8_t
	{
		SHADER_FORMAT_NONE,		// Not used
		HLSL5,		// DXBC
		HLSL6,		// DXIL
		SPIRV,		// SPIR-V
		HLSL6_XS,	// XBOX Series Native
		PS5,		// Playstation 5
		METAL,		// Apple Metal
	};
	enum ShaderModel : uint8_t
	{
		SM_5_0,
		SM_6_0,
		SM_6_1,
		SM_6_2,
		SM_6_3,
		SM_6_4,
		SM_6_5,
		SM_6_6,
		SM_6_7,
	};
	enum PrimitiveTopology : uint8_t
	{
		PRIMITIVE_UNDEFINED,
		TRIANGLELIST,
		TRIANGLESTRIP,
		POINTLIST,
		LINELIST,
		LINESTRIP,
		PATCHLIST,
	};
	enum ComparisonFunc : uint8_t
	{
		NEVER,
		LESS,
		EQUAL,
		LESS_EQUAL,
		GREATER,
		NOT_EQUAL,
		GREATER_EQUAL,
		ALWAYS,
	};
	enum DepthWriteMask : uint8_t
	{
		DEPTH_WRITE_ZERO,	// Disables depth write
		ALL,	// Enables depth write
	};
	enum StencilOp : uint8_t
	{
		KEEP,
		STENCIL_ZERO,
		REPLACE,
		INCR_SAT,
		DECR_SAT,
		INVERT,
		INCR,
		DECR,
	};
	enum Blend : uint8_t
	{
		ZERO,
		ONE,
		SRC_COLOR,
		INV_SRC_COLOR,
		SRC_ALPHA,
		INV_SRC_ALPHA,
		DEST_ALPHA,
		INV_DEST_ALPHA,
		DEST_COLOR,
		INV_DEST_COLOR,
		SRC_ALPHA_SAT,
		BLEND_FACTOR,
		INV_BLEND_FACTOR,
		SRC1_COLOR,
		INV_SRC1_COLOR,
		SRC1_ALPHA,
		INV_SRC1_ALPHA,
	}; 
	enum BlendOp : uint8_t
	{
		ADD,
		SUBTRACT,
		REV_SUBTRACT,
		MIN,
		MAX,
	};
	enum FillMode : uint8_t
	{
		WIREFRAME,
		SOLID,
	};
	enum CullMode : uint8_t
	{
		CULL_NONE,
		FRONT,
		BACK,
	};
	enum InputClassification : uint8_t
	{
		PER_VERTEX_DATA,
		PER_INSTANCE_DATA,
	};
	enum Usage : uint8_t
	{
		DEFAULT,	// CPU no access, GPU read/write
		UPLOAD,	    // CPU write, GPU read
		READBACK,	// CPU read, GPU write
	};
	enum TextureAddressMode : uint8_t
	{
		WRAP,
		MIRROR,
		CLAMP,
		BORDER,
		MIRROR_ONCE,
	};
	enum Filter : uint8_t
	{
		MIN_MAG_MIP_POINT,
		MIN_MAG_POINT_MIP_LINEAR,
		MIN_POINT_MAG_LINEAR_MIP_POINT,
		MIN_POINT_MAG_MIP_LINEAR,
		MIN_LINEAR_MAG_MIP_POINT,
		MIN_LINEAR_MAG_POINT_MIP_LINEAR,
		MIN_MAG_LINEAR_MIP_POINT,
		MIN_MAG_MIP_LINEAR,
		ANISOTROPIC,
		COMPARISON_MIN_MAG_MIP_POINT,
		COMPARISON_MIN_MAG_POINT_MIP_LINEAR,
		COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT,
		COMPARISON_MIN_POINT_MAG_MIP_LINEAR,
		COMPARISON_MIN_LINEAR_MAG_MIP_POINT,
		COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
		COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
		COMPARISON_MIN_MAG_MIP_LINEAR,
		COMPARISON_ANISOTROPIC,
		MINIMUM_MIN_MAG_MIP_POINT,
		MINIMUM_MIN_MAG_POINT_MIP_LINEAR,
		MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT,
		MINIMUM_MIN_POINT_MAG_MIP_LINEAR,
		MINIMUM_MIN_LINEAR_MAG_MIP_POINT,
		MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
		MINIMUM_MIN_MAG_LINEAR_MIP_POINT,
		MINIMUM_MIN_MAG_MIP_LINEAR,
		MINIMUM_ANISOTROPIC,
		MAXIMUM_MIN_MAG_MIP_POINT,
		MAXIMUM_MIN_MAG_POINT_MIP_LINEAR,
		MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT,
		MAXIMUM_MIN_POINT_MAG_MIP_LINEAR,
		MAXIMUM_MIN_LINEAR_MAG_MIP_POINT,
		MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
		MAXIMUM_MIN_MAG_LINEAR_MIP_POINT,
		MAXIMUM_MIN_MAG_MIP_LINEAR,
		MAXIMUM_ANISOTROPIC,
	};
	enum SamplerBorderColor : uint8_t
	{
		TRANSPARENT_BLACK,
		OPAQUE_BLACK,
		OPAQUE_WHITE,
	};
	enum Format : uint8_t
	{
		UNKNOWN,

		R32G32B32A32_FLOAT,
		R32G32B32A32_UINT,
		R32G32B32A32_SINT,

		R32G32B32_FLOAT,
		R32G32B32_UINT,
		R32G32B32_SINT,

		R16G16B16A16_FLOAT,
		R16G16B16A16_UNORM,
		R16G16B16A16_UINT,
		R16G16B16A16_SNORM,
		R16G16B16A16_SINT,

		R32G32_FLOAT,
		R32G32_UINT,
		R32G32_SINT,
		D32_FLOAT_S8X24_UINT,	// depth (32-bit) + stencil (8-bit) | SRV: R32_FLOAT (default or depth aspect), R8_UINT (stencil aspect)

		R10G10B10A2_UNORM,
		R10G10B10A2_UINT,
		R11G11B10_FLOAT,
		R8G8B8A8_UNORM,
		R8G8B8A8_UNORM_SRGB,
		R8G8B8A8_UINT,
		R8G8B8A8_SNORM,
		R8G8B8A8_SINT,
		B8G8R8A8_UNORM,
		B8G8R8A8_UNORM_SRGB,
		R16G16_FLOAT,
		R16G16_UNORM,
		R16G16_UINT,
		R16G16_SNORM,
		R16G16_SINT,
		D32_FLOAT,				// depth (32-bit) | SRV: R32_FLOAT
		R32_FLOAT,
		R32_UINT,
		R32_SINT, 
		D24_UNORM_S8_UINT,		// depth (24-bit) + stencil (8-bit) | SRV: R24_INTERNAL (default or depth aspect), R8_UINT (stencil aspect)
		R9G9B9E5_SHAREDEXP,

		R8G8_UNORM,
		R8G8_UINT,
		R8G8_SNORM,
		R8G8_SINT,
		R16_FLOAT,
		D16_UNORM,				// depth (16-bit) | SRV: R16_UNORM
		R16_UNORM,
		R16_UINT,
		R16_SNORM,
		R16_SINT,

		R8_UNORM,
		R8_UINT,
		R8_SNORM,
		R8_SINT,

		// Formats that are not usable in render pass must be below because formats in render pass must be encodable as 6 bits:

		BC1_UNORM,			// Three color channels (5 bits:6 bits:5 bits), with 0 or 1 bit(s) of alpha
		BC1_UNORM_SRGB,		// Three color channels (5 bits:6 bits:5 bits), with 0 or 1 bit(s) of alpha
		BC2_UNORM,			// Three color channels (5 bits:6 bits:5 bits), with 4 bits of alpha
		BC2_UNORM_SRGB,		// Three color channels (5 bits:6 bits:5 bits), with 4 bits of alpha
		BC3_UNORM,			// Three color channels (5 bits:6 bits:5 bits) with 8 bits of alpha
		BC3_UNORM_SRGB,		// Three color channels (5 bits:6 bits:5 bits) with 8 bits of alpha
		BC4_UNORM,			// One color channel (8 bits)
		BC4_SNORM,			// One color channel (8 bits)
		BC5_UNORM,			// Two color channels (8 bits:8 bits)
		BC5_SNORM,			// Two color channels (8 bits:8 bits)
		BC6H_UF16,			// Three color channels (16 bits:16 bits:16 bits) in "half" floating point
		BC6H_SF16,			// Three color channels (16 bits:16 bits:16 bits) in "half" floating point
		BC7_UNORM,			// Three color channels (4 to 7 bits per channel) with 0 to 8 bits of alpha
		BC7_UNORM_SRGB,		// Three color channels (4 to 7 bits per channel) with 0 to 8 bits of alpha

		NV12,				// video YUV420; SRV Luminance aspect: R8_UNORM, SRV Chrominance aspect: R8G8_UNORM
	};
	enum GpuQueryType : uint8_t
	{
		TIMESTAMP,			// retrieve time point of gpu execution
		OCCLUSION,			// how many samples passed depth test?
		OCCLUSION_BINARY,	// depth test passed or not?
	};
	enum IndexBufferFormat : uint8_t
	{
		UINT16,
		UINT32,
	};
	enum SubresourceType : uint8_t
	{
		SRV, // shader resource view
		UAV, // unordered access view
		RTV, // render target view
		DSV, // depth stencil view
	};

	enum ShadingRate : uint8_t
	{
		RATE_1X1,	// Default/full shading rate
		RATE_1X2,
		RATE_2X1,
		RATE_2X2,
		RATE_2X4,
		RATE_4X2,
		RATE_4X4,

		RATE_INVALID
	};

	enum PredicationOp : uint8_t
	{
		EQUAL_ZERO,
		NOT_EQUAL_ZERO,
	};

	enum ImageAspect : uint8_t
	{
		COLOR,
		DEPTH,
		STENCIL,
		LUMINANCE,
		CHROMINANCE,
	};

	enum VideoFrameType : uint8_t
	{
		Intra,
		Predictive,
	};

	enum VideoProfile : uint8_t
	{
		H264,	// AVC
		H265,	// HEVC
	};

	enum ComponentSwizzle : uint8_t
	{
		R,
		G,
		B,
		A,
		SWIZZLE_ZERO,
		SWIZZLE_ONE,
	};

	enum ColorSpace : uint8_t
	{
		SRGB,			// SDR color space (8 or 10 bits per channel)
		HDR10_ST2084,	// HDR10 color space (10 bits per channel)
		HDR_LINEAR,		// HDR color space (16 bits per channel)
	};

	// Flags ////////////////////////////////////////////

	enum ColorWrite
	{
		DISABLE = 0,
		ENABLE_RED = 1 << 0,
		ENABLE_GREEN = 1 << 1,
		ENABLE_BLUE = 1 << 2,
		ENABLE_ALPHA = 1 << 3,
		ENABLE_ALL = ~0,
	};

	enum BindFlag : uint8_t
	{
		BIND_NONE = 0,
		BIND_VERTEX_BUFFER = 1 << 0,
		BIND_INDEX_BUFFER = 1 << 1,
		BIND_CONSTANT_BUFFER = 1 << 2,
		BIND_SHADER_RESOURCE = 1 << 3,
		RENDER_TARGET = 1 << 4,
		DEPTH_STENCIL = 1 << 5,
		BIND_UNORDERED_ACCESS = 1 << 6,
		SHADING_RATE = 1 << 7,
	};

	enum ResourceMiscFlag
	{
		RESOURCE_MISC_NONE = 0,
		TEXTURECUBE = 1 << 0,
		INDIRECT_ARGS = 1 << 1,
		BUFFER_RAW = 1 << 2,
		BUFFER_STRUCTURED = 1 << 3,
		RAY_TRACING = 1 << 4,
		RESOURCE_MISC_PREDICATION = 1 << 5,
		TRANSIENT_ATTACHMENT = 1 << 6,	// hint: used in renderpass, without needing to write content to memory (VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
		SPARSE = 1 << 7,	// sparse resource without backing memory allocation
		ALIASING_BUFFER = 1 << 8,			// memory allocation will be suitable for buffers
		ALIASING_TEXTURE_NON_RT_DS = 1 << 9,// memory allocation will be suitable for textures that are non render targets nor depth stencils
		ALIASING_TEXTURE_RT_DS = 1 << 10,	// memory allocation will be suitable for textures that are either render targets or depth stencils
		// Keep this as an integral constant expression so bitmask operator templates are not instantiated
		// before enable_bitmask_operators<ResourceMiscFlag> specialization is declared.
		ALIASING = (1 << 8) | (1 << 9) | (1 << 10), // memory allocation will be suitable for all kinds of resources. Requires GraphicsDeviceCapability::ALIASING_GENERIC to be supported
		TYPED_FORMAT_CASTING = 1 << 11,	// enable casting formats between same type and different modifiers: eg. UNORM -> SRGB
		TYPELESS_FORMAT_CASTING = 1 << 12,	// enable casting formats to other formats that have the same bit-width and channel layout: eg. R32_FLOAT -> R32_UINT
		VIDEO_DECODE = 1 << 13,	// resource is usabe in video decoding operations (for buffers it is indicating a bitstream buffer, for textures it is a DPB and output texture if DPB_AND_OUTPUT_COINCIDE is supported)
		VIDEO_DECODE_OUTPUT_ONLY = 1 << 14,	// resource is usabe in video decoding operations but as output only and not as DPB (used for DPB textures when DPB_AND_OUTPUT_COINCIDE is NOT supported)
		VIDEO_DECODE_DPB_ONLY = 1 << 15,	// resource is usabe in video decoding operations but as strictly DPB only (used for output textures when DPB_AND_OUTPUT_COINCIDE is NOT supported)
		NO_DEFAULT_DESCRIPTORS = 1 << 16, // skips creation of default descriptors for resources
		SHARED = 1 << 17, // shared texture

		VIDEO_COMPATIBILITY_H264 = 1 << 18,	// required for vulkan resource creation for every resource (bitstream buffer, DPB, output) that will be used in a H264 decode session
		VIDEO_COMPATIBILITY_H265 = 1 << 19,	// required for vulkan resource creation for every resource (bitstream buffer, DPB, output) that will be used in a H265 decode session

		// Compat:
		SPARSE_TILE_POOL_BUFFER = ALIASING_BUFFER,
		SPARSE_TILE_POOL_TEXTURE_NON_RT_DS = ALIASING_TEXTURE_NON_RT_DS,
		SPARSE_TILE_POOL_TEXTURE_RT_DS = ALIASING_TEXTURE_RT_DS,
		SPARSE_TILE_POOL = ALIASING,
	};

	enum GraphicsDeviceCapability
	{
		GRAPHICS_DEVICE_CAPABILITY_NONE = 0,
		TESSELLATION = 1 << 0,
		CONSERVATIVE_RASTERIZATION = 1 << 1,
		RASTERIZER_ORDERED_VIEWS = 1 << 2,
		UAV_LOAD_FORMAT_COMMON = 1 << 3, // eg: R16G16B16A16_FLOAT, R8G8B8A8_UNORM and more common ones
		UAV_LOAD_FORMAT_R11G11B10_FLOAT = 1 << 4,
		VARIABLE_RATE_SHADING = 1 << 5,
		VARIABLE_RATE_SHADING_TIER2 = 1 << 6,
		MESH_SHADER = 1 << 7,
		RAYTRACING = 1 << 8,
		GRAPHICS_DEVICE_CAPABILITY_PREDICATION = 1 << 9,
		SAMPLER_MINMAX = 1 << 10,
		DEPTH_BOUNDS_TEST = 1 << 11,
		SPARSE_BUFFER = 1 << 12,
		SPARSE_TEXTURE2D = 1 << 13,
		SPARSE_TEXTURE3D = 1 << 14,
		SPARSE_NULL_MAPPING = 1 << 15,
		ALIASING_GENERIC = 1 << 16, // allows using ResourceMiscFlag::ALIASING (non resource type specific version)
		DEPTH_RESOLVE_MIN_MAX = 1 << 17,
		STENCIL_RESOLVE_MIN_MAX = 1 << 18,
		CACHE_COHERENT_UMA = 1 << 19,	// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture
		VIDEO_DECODE_H264 = 1 << 20,
		VIDEO_DECODE_H265 = 1 << 21,
		R9G9B9E5_SHAREDEXP_RENDERABLE = 1 << 22, // indicates supporting R9G9B9E5_SHAREDEXP format for rendering to
		COPY_BETWEEN_DIFFERENT_IMAGE_ASPECTS_NOT_SUPPORTED = 1 << 23, // indicates that CopyTexture src and dst ImageAspect must match
		
		// Compat:
		GENERIC_SPARSE_TILE_POOL = ALIASING_GENERIC,
	};

	enum ResourceState
	{
		// Common resource states:
		UNDEFINED = 0,						// invalid state (don't preserve contents)
		SHADER_RESOURCE = 1 << 0,			// shader resource, read only
		SHADER_RESOURCE_COMPUTE = 1 << 1,	// shader resource, read only, non-pixel shader
		UNORDERED_ACCESS = 1 << 2,			// shader resource, write enabled
		COPY_SRC = 1 << 3,					// copy from
		COPY_DST = 1 << 4,					// copy to

		// Texture specific resource states:
		RENDERTARGET = 1 << 5,				// render target, write enabled
		DEPTHSTENCIL = 1 << 6,				// depth stencil, write enabled
		DEPTHSTENCIL_READONLY = 1 << 7,		// depth stencil, read only
		SHADING_RATE_SOURCE = 1 << 8,		// shading rate control per tile

		// GPUBuffer specific resource states:
		VERTEX_BUFFER = 1 << 9,				// vertex buffer, read only
		INDEX_BUFFER = 1 << 10,				// index buffer, read only
		CONSTANT_BUFFER = 1 << 11,			// constant buffer, read only
		INDIRECT_ARGUMENT = 1 << 12,		// argument buffer to DrawIndirect() or DispatchIndirect()
		RAYTRACING_ACCELERATION_STRUCTURE = 1 << 13, // acceleration structure storage or scratch
		PREDICATION = 1 << 14,				// storage for predication comparison value

		// Other:
		VIDEO_DECODE_SRC = 1 << 15,			// video decode operation source (bitstream buffer or DPB texture)
		VIDEO_DECODE_DST = 1 << 16,			// video decode operation destination output texture
		VIDEO_DECODE_DPB = 1 << 17,			// video decode operation destination DPB texture
		SWAPCHAIN = 1 << 18,				// resource state of swap chain's back buffer texture when it's not rendering
	};

	enum RenderPassFlags
	{
		RENDER_PASS_FLAG_NONE = 0,
		ALLOW_UAV_WRITES = 1 << 0,			// allows UAV writes to happen within render pass
		SUSPENDING = 1 << 1,				// suspends the renderpass to be continued in the next submitted command list
		RESUMING = 1 << 2,					// resumes the renderpass that was suspended in the previously submitted command list
	};

	enum VideoDecoderSupportFlags
	{
		VIDEO_DECODER_SUPPORT_NONE = 0,
		DPB_AND_OUTPUT_COINCIDE = 1 << 0,				// the video decoder supports using the DPB texture as output shader resource. If not supported, then DPB_AND_OUTPUT_DISTINCT must be supported.
		DPB_AND_OUTPUT_DISTINCT = 1 << 1,				// the video decoder supports outputting to a texture that is not part of the DPB as part of the decode operation. If not supported, then DPB_AND_OUTPUT_COINCIDE must be supported.
		DPB_INDIVIDUAL_TEXTURES_SUPPORTED = 1 << 2,		// the video decoder supports using a DPB that is not an array texture, so each slot can be an individually allocated texture
	};


	// Descriptor structs:

	struct Viewport
	{
		float top_left_x = 0;
		float top_left_y = 0;
		float width = 0;
		float height = 0;
		float min_depth = 0;
		float max_depth = 1;
	};

	struct InputLayout
	{
		static const uint32_t APPEND_ALIGNED_ELEMENT = ~0u; // automatically figure out AlignedByteOffset depending on Format

		struct Element
		{
			char* semantic_name = nullptr;
			uint32_t semantic_index = 0;
			Format format = Format::UNKNOWN;
			uint32_t input_slot = 0;
			uint32_t aligned_byte_offset = APPEND_ALIGNED_ELEMENT;
			InputClassification input_slot_class = InputClassification::PER_VERTEX_DATA;
		};
		Element* elements = nullptr; // stb_ds-backed input layout element array
	};

	inline void InitInputLayoutElement(InputLayout::Element& element)
	{
		element.semantic_name = nullptr;
		element.semantic_index = 0;
		element.format = Format::UNKNOWN;
		element.input_slot = 0;
		element.aligned_byte_offset = InputLayout::APPEND_ALIGNED_ELEMENT;
		element.input_slot_class = InputClassification::PER_VERTEX_DATA;
	}
	inline void DestroyInputLayoutElement(InputLayout::Element& element)
	{
		DestroyCString(element.semantic_name);
		InitInputLayoutElement(element);
	}
	inline void CloneInputLayoutElement(InputLayout::Element& dst, const InputLayout::Element& src)
	{
		DestroyInputLayoutElement(dst);
		dst.semantic_name = CloneCString(src.semantic_name);
		dst.semantic_index = src.semantic_index;
		dst.format = src.format;
		dst.input_slot = src.input_slot;
		dst.aligned_byte_offset = src.aligned_byte_offset;
		dst.input_slot_class = src.input_slot_class;
	}
	inline void InitInputLayout(InputLayout& input_layout)
	{
		input_layout.elements = nullptr;
	}
	inline void DestroyInputLayout(InputLayout& input_layout)
	{
		if (input_layout.elements != nullptr)
		{
			for (size_t i = 0; i < arrlenu(input_layout.elements); ++i)
			{
				DestroyInputLayoutElement(input_layout.elements[i]);
			}
		}
		arrfree(input_layout.elements);
		InitInputLayout(input_layout);
	}
	inline void CloneInputLayout(InputLayout& dst, const InputLayout& src)
	{
		if (&dst == &src)
		{
			return;
		}
		DestroyInputLayout(dst);
		const size_t count = arrlenu(src.elements);
		arrsetlen(dst.elements, count);
		for (size_t i = 0; i < count; ++i)
		{
			InitInputLayoutElement(dst.elements[i]);
			CloneInputLayoutElement(dst.elements[i], src.elements[i]);
		}
	}

	union ClearValue
	{
		float color[4];
		struct ClearDepthStencil
		{
			float depth;
			uint32_t stencil;
		} depth_stencil;
	};

	struct Swizzle
	{
		ComponentSwizzle r = ComponentSwizzle::R;
		ComponentSwizzle g = ComponentSwizzle::G;
		ComponentSwizzle b = ComponentSwizzle::B;
		ComponentSwizzle a = ComponentSwizzle::A;
	};

	struct TextureDesc
	{
		enum Type : uint8_t
		{
			TEXTURE_1D,
			TEXTURE_2D,
			TEXTURE_3D,
		} type = Type::TEXTURE_2D;
		Format format = Format::UNKNOWN;
		Usage usage = Usage::DEFAULT;
		BindFlag bind_flags = BindFlag::BIND_NONE;
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
		uint32_t array_size = 1;
		uint32_t mip_levels = 1;
		uint32_t sample_count = 1;
		ClearValue clear = {};
		Swizzle swizzle;
		ResourceMiscFlag misc_flags = ResourceMiscFlag::RESOURCE_MISC_NONE;
		ResourceState layout = ResourceState::SHADER_RESOURCE;
	};

	struct SamplerDesc
	{
		Filter filter = Filter::MIN_MAG_MIP_POINT;
		TextureAddressMode address_u = TextureAddressMode::CLAMP;
		TextureAddressMode address_v = TextureAddressMode::CLAMP;
		TextureAddressMode address_w = TextureAddressMode::CLAMP;
		float mip_lod_bias = 0;
		uint32_t max_anisotropy = 0;
		ComparisonFunc comparison_func = ComparisonFunc::NEVER;
		SamplerBorderColor border_color = SamplerBorderColor::TRANSPARENT_BLACK;
		float min_lod = 0;
		float max_lod = std::numeric_limits<float>::max();
	};

	struct RasterizerState
	{
		FillMode fill_mode = FillMode::SOLID;
		CullMode cull_mode = CullMode::CULL_NONE;
		bool front_counter_clockwise = false;
		int32_t depth_bias = 0;
		float depth_bias_clamp = 0;
		float slope_scaled_depth_bias = 0;
		bool depth_clip_enable = false;
		bool multisample_enable = false;
		bool antialiased_line_enable = false;
		bool conservative_rasterization_enable = false;
		uint32_t forced_sample_count = 0;
	};
	inline void InitRasterizerState(RasterizerState& state)
	{
		state.fill_mode = FillMode::SOLID;
		state.cull_mode = CullMode::CULL_NONE;
		state.front_counter_clockwise = false;
		state.depth_bias = 0;
		state.depth_bias_clamp = 0;
		state.slope_scaled_depth_bias = 0;
		state.depth_clip_enable = false;
		state.multisample_enable = false;
		state.antialiased_line_enable = false;
		state.conservative_rasterization_enable = false;
		state.forced_sample_count = 0;
	}

	struct DepthStencilState
	{
		bool depth_enable = false;
		DepthWriteMask depth_write_mask = DepthWriteMask::DEPTH_WRITE_ZERO;
		ComparisonFunc depth_func = ComparisonFunc::NEVER;
		bool stencil_enable = false;
		uint8_t stencil_read_mask = 0xff;
		uint8_t stencil_write_mask = 0xff;

		struct DepthStencilOp
		{
			StencilOp stencil_fail_op = StencilOp::KEEP;
			StencilOp stencil_depth_fail_op = StencilOp::KEEP;
			StencilOp stencil_pass_op = StencilOp::KEEP;
			ComparisonFunc stencil_func = ComparisonFunc::NEVER;
		};
		DepthStencilOp front_face;
		DepthStencilOp back_face;
		bool depth_bounds_test_enable = false;
	};
	inline void InitDepthStencilOp(DepthStencilState::DepthStencilOp& op)
	{
		op.stencil_fail_op = StencilOp::KEEP;
		op.stencil_depth_fail_op = StencilOp::KEEP;
		op.stencil_pass_op = StencilOp::KEEP;
		op.stencil_func = ComparisonFunc::NEVER;
	}
	inline void InitDepthStencilState(DepthStencilState& state)
	{
		state.depth_enable = false;
		state.depth_write_mask = DepthWriteMask::DEPTH_WRITE_ZERO;
		state.depth_func = ComparisonFunc::NEVER;
		state.stencil_enable = false;
		state.stencil_read_mask = 0xff;
		state.stencil_write_mask = 0xff;
		InitDepthStencilOp(state.front_face);
		InitDepthStencilOp(state.back_face);
		state.depth_bounds_test_enable = false;
	}

	struct BlendState
	{
		bool alpha_to_coverage_enable = false;
		bool independent_blend_enable = false;

		struct RenderTargetBlendState
		{
			bool blend_enable = false;
			Blend src_blend = Blend::SRC_ALPHA;
			Blend dest_blend = Blend::INV_SRC_ALPHA;
			BlendOp blend_op = BlendOp::ADD;
			Blend src_blend_alpha = Blend::ONE;
			Blend dest_blend_alpha = Blend::ONE;
			BlendOp blend_op_alpha = BlendOp::ADD;
			ColorWrite render_target_write_mask = ColorWrite::ENABLE_ALL;
		};
		RenderTargetBlendState render_target[8];
	};
	inline void InitRenderTargetBlendState(BlendState::RenderTargetBlendState& state)
	{
		state.blend_enable = false;
		state.src_blend = Blend::SRC_ALPHA;
		state.dest_blend = Blend::INV_SRC_ALPHA;
		state.blend_op = BlendOp::ADD;
		state.src_blend_alpha = Blend::ONE;
		state.dest_blend_alpha = Blend::ONE;
		state.blend_op_alpha = BlendOp::ADD;
		state.render_target_write_mask = ColorWrite::ENABLE_ALL;
	}
	inline void InitBlendState(BlendState& state)
	{
		state.alpha_to_coverage_enable = false;
		state.independent_blend_enable = false;
		for (auto& rt : state.render_target)
		{
			InitRenderTargetBlendState(rt);
		}
	}

	struct GPUBufferDesc
	{
		uint64_t size = 0;
		uint32_t stride = 0; // only needed for structured buffer types!
		uint32_t alignment = 0; // needed for tile pools
		Usage usage = Usage::DEFAULT;
		Format format = Format::UNKNOWN; // only needed for typed buffer!
		BindFlag bind_flags = BindFlag::BIND_NONE;
		ResourceMiscFlag misc_flags = ResourceMiscFlag::RESOURCE_MISC_NONE;
	};

	struct GPUQueryHeapDesc
	{
		GpuQueryType type = GpuQueryType::TIMESTAMP;
		uint32_t query_count = 0;
	};

	// This structure contains pointers to render state config, the pointers must be valid until the pipeline state is in use!
	struct PipelineStateDesc
	{
		const Shader* vs = nullptr;
		const Shader* ps = nullptr;
		const Shader* hs = nullptr;
		const Shader* ds = nullptr;
		const Shader* gs = nullptr;
		const Shader* ms = nullptr;
		const Shader* as = nullptr;
		const BlendState* bs = nullptr;
		const RasterizerState* rs = nullptr;
		const DepthStencilState* dss = nullptr;
		const InputLayout* il = nullptr;
		PrimitiveTopology pt = PrimitiveTopology::TRIANGLELIST;
		uint32_t patch_control_points = 3;
		uint32_t sample_mask = 0xFFFFFFFF;
	};

		struct GPUBarrier
		{
			enum Type
			{
				MEMORY,		// UAV accesses
			IMAGE,		// image layout transition
			BUFFER,		// buffer state transition
			ALIASING,	// memory aliasing transition
		} type = Type::MEMORY;

		struct Memory
		{
			const GPUResource* resource;
		};
		struct Image
		{
			const Texture* texture;
			ResourceState layout_before;
			ResourceState layout_after;
			int mip;
			int slice;
			const ImageAspect* aspect;
		};
		struct Buffer
		{
			const GPUBuffer* buffer;
			ResourceState state_before;
			ResourceState state_after;
		};
		struct Aliasing
		{
			const GPUResource* resource_before;
			const GPUResource* resource_after;
		};
			union
			{
				Memory memory;
				Image image;
				Buffer buffer;
				Aliasing aliasing;
			};
		};
		inline GPUBarrier wiGraphicsCreateGPUBarrierMemory(const GPUResource* resource = nullptr)
		{
			GPUBarrier barrier = {};
			barrier.type = GPUBarrier::Type::MEMORY;
			barrier.memory.resource = resource;
			return barrier;
		}
		inline GPUBarrier wiGraphicsCreateGPUBarrierImage(
			const Texture* texture,
			ResourceState before,
			ResourceState after,
			int mip = -1,
			int slice = -1,
			const ImageAspect* aspect = nullptr)
		{
			GPUBarrier barrier = {};
			barrier.type = GPUBarrier::Type::IMAGE;
			barrier.image.texture = texture;
			barrier.image.layout_before = before;
			barrier.image.layout_after = after;
			barrier.image.mip = mip;
			barrier.image.slice = slice;
			barrier.image.aspect = aspect;
			return barrier;
		}
		inline GPUBarrier wiGraphicsCreateGPUBarrierBuffer(const GPUBuffer* buffer, ResourceState before, ResourceState after)
		{
			GPUBarrier barrier = {};
			barrier.type = GPUBarrier::Type::BUFFER;
			barrier.buffer.buffer = buffer;
			barrier.buffer.state_before = before;
			barrier.buffer.state_after = after;
			return barrier;
		}
		inline GPUBarrier wiGraphicsCreateGPUBarrierAliasing(const GPUResource* before, const GPUResource* after)
		{
			GPUBarrier barrier = {};
			barrier.type = GPUBarrier::Type::ALIASING;
			barrier.aliasing.resource_before = before;
			barrier.aliasing.resource_after = after;
			return barrier;
		}

	struct SwapChainDesc
	{
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t buffer_count = 2;
		Format format = Format::R10G10B10A2_UNORM;
		bool fullscreen = false;
		bool vsync = true;
		float clear_color[4] = { 0,0,0,1 };
		bool allow_hdr = true;
	};

	struct SubresourceData
	{
		const void* data_ptr = nullptr;	// pointer to the beginning of the subresource data (pointer to beginning of resource + subresource offset)
		uint32_t row_pitch = 0;			// bytes between two rows of a texture (2D and 3D textures)
		uint32_t slice_pitch = 0;		// bytes between two depth slices of a texture (3D textures only)
	};

		struct Rect
		{
			int32_t left = 0;	// start width
			int32_t top = 0;	// start height
			int32_t right = 0;	// end width
			int32_t bottom = 0;	// end height
		};
		inline void wiGraphicsRectFromViewport(Rect* rect, const Viewport* vp)
		{
			if (rect == nullptr || vp == nullptr)
			{
				return;
			}
			rect->left = int32_t(vp->top_left_x);
			rect->right = int32_t(vp->top_left_x + vp->width);
			rect->top = int32_t(vp->top_left_y);
			rect->bottom = int32_t(vp->top_left_y + vp->height);
		}

	struct Box
	{
		uint32_t left = 0;		// start width
		uint32_t top = 0;		// start height
		uint32_t front = 0;		// start depth
		uint32_t right = 0;		// end width
		uint32_t bottom = 0;	// end height
		uint32_t back = 0;		// end depth
	};

	struct SparseTextureProperties
	{
		uint32_t tile_width = 0;				// width of 1 tile in texels
		uint32_t tile_height = 0;				// height of 1 tile in texels
		uint32_t tile_depth = 0;				// depth of 1 tile in texels
		uint32_t total_tile_count = 0;			// number of tiles for entire resource
		uint32_t packed_mip_start = 0;			// first mip of packed mipmap levels, these cannot be individually mapped and they cannot use a box mapping
		uint32_t packed_mip_count = 0;			// number of packed mipmap levels, these cannot be individually mapped and they cannot use a box mapping
		uint32_t packed_mip_tile_offset = 0;	// offset of the tiles for packed mip data relative to the entire resource
		uint32_t packed_mip_tile_count = 0;		// how many tiles are required for the packed mipmaps
	};

	struct VideoDesc
	{
		uint32_t width = 0;					// must meet the codec specific alignment requirements
		uint32_t height = 0;				// must meet the codec specific alignment requirements
		uint32_t bit_rate = 0;				// can be 0, it means that decoding will be prepared for worst case
		Format format = Format::NV12;
		VideoProfile profile = VideoProfile::H264;
		const void* pps_datas = nullptr;	// array of picture parameter set structures. The structure type depends on video codec
		size_t pps_count = 0;				// number of picture parameter set structures in the pps_datas array
		const void* sps_datas = nullptr;	// array of sequence parameter set structures. The structure type depends on video codec
		size_t sps_count = 0;				// number of sequence parameter set structures in the sps_datas array
		uint32_t num_dpb_slots = 0;			// The number of decode picture buffer slots. Usually it is required to be at least number_of_reference_frames + 1
	};


	// Resources:

	struct Sampler
	{
		wi::allocator::shared_ptr<void> internal_state;

		SamplerDesc desc;
	};

	struct Shader
	{
		wi::allocator::shared_ptr<void> internal_state;

		ShaderStage stage = ShaderStage::Count;
	};

	struct GPUResource
	{
		wi::allocator::shared_ptr<void> internal_state;

		// These are only valid if the resource was created with CPU access (USAGE::UPLOAD or USAGE::READBACK)
		void* mapped_data = nullptr;	// for buffers, it is a pointer to the buffer data; for textures, it is a pointer to texture data with linear tiling;
		size_t mapped_size = 0;			// for buffers, it is the full buffer size; for textures it is the full texture size including all subresources;

		uint32_t sparse_page_size = 0;	// specifies the required alignment of backing allocation for sparse tile pool

		enum Type : uint8_t
		{
			BUFFER,
			TEXTURE,
			RAYTRACING_ACCELERATION_STRUCTURE,
			UNKNOWN_TYPE,
		} type = Type::UNKNOWN_TYPE;
	};

		struct GPUBuffer final : public GPUResource
		{
			GPUBufferDesc desc;
		};

	struct Texture final : public GPUResource
	{
		TextureDesc	desc;

		// These are only valid if the texture was created with CPU access (USAGE::UPLOAD or USAGE::READBACK)
		const SubresourceData* mapped_subresources = nullptr;	// an array of subresource mappings in the following memory layout: slice0|mip0, slice0|mip1, slice0|mip2, ... sliceN|mipN
		size_t mapped_subresource_count = 0;					// the array size of mapped_subresources (number of slices * number of miplevels)

		// These are only valid if texture was created with ResourceMiscFlag::SPARSE flag:
		const SparseTextureProperties* sparse_properties = nullptr;

	#if defined(_WIN32)
			void* shared_handle = nullptr; /* HANDLE */
	#else
			int shared_handle = 0;
	#endif
		};

	struct VideoDecoder
	{
		wi::allocator::shared_ptr<void> internal_state;

		VideoDesc desc;
		VideoDecoderSupportFlags support = VideoDecoderSupportFlags::VIDEO_DECODER_SUPPORT_NONE;
	};

	struct VideoDecodeOperation
	{
		enum Flags
		{
			FLAG_EMPTY = 0,
			FLAG_SESSION_RESET = 1 << 0, // first usage of decoder needs reset
		};
		uint32_t flags = FLAG_EMPTY;
		const GPUBuffer* stream = nullptr;
		uint64_t stream_offset = 0; // must be aligned with GraphicsDevice::GetVideoDecodeBitstreamAlignment()
		uint64_t stream_size = 0;
		VideoFrameType frame_type = VideoFrameType::Intra;
		uint32_t reference_priority = 0; // nal_ref_idc from nal unit header
		int decoded_frame_index = 0; // frame index in order of decoding
		const void* slice_header = nullptr; // slice header for current frame
		const void* pps = nullptr; // picture parameter set for current slice header
		const void* sps = nullptr; // sequence parameter set for current picture parameter set
		int poc[2] = {}; // PictureOrderCount Top and Bottom fields
		uint32_t current_dpb = 0; // DPB slot for current output picture
		uint8_t dpb_reference_count = 0; // number of references in dpb_reference_slots array
		const uint8_t* dpb_reference_slots = nullptr; // dpb slot indices that are used as reference pictures
		const int* dpb_poc = nullptr; // for each DPB reference slot, indicate the PictureOrderCount
		const int* dpb_framenum = nullptr; // for each DPB reference slot, indicate the framenum value
		const Texture* DPB = nullptr; // DPB texture with arraysize = num_references + 1
		const Texture* output = nullptr; // output of the operation, it should be nullptr if DPB_AND_OUTPUT_COINCIDE is used (because in that case the DPB will be used as output instead of a separate output)
	};
	inline bool wiGraphicsSamplerIsValid(const Sampler* sampler)
	{
		return sampler != nullptr && sampler->internal_state.IsValid();
	}
	inline const SamplerDesc* wiGraphicsSamplerGetDesc(const Sampler* sampler)
	{
		return sampler != nullptr ? &sampler->desc : nullptr;
	}
	inline bool wiGraphicsShaderIsValid(const Shader* shader)
	{
		return shader != nullptr && shader->internal_state.IsValid();
	}
	inline bool wiGraphicsGPUResourceIsValid(const GPUResource* resource)
	{
		return resource != nullptr && resource->internal_state.IsValid();
	}
	inline bool wiGraphicsGPUResourceIsTexture(const GPUResource* resource)
	{
		return resource != nullptr && resource->type == GPUResource::Type::TEXTURE;
	}
	inline bool wiGraphicsGPUResourceIsBuffer(const GPUResource* resource)
	{
		return resource != nullptr && resource->type == GPUResource::Type::BUFFER;
	}
	inline bool wiGraphicsGPUResourceIsAccelerationStructure(const GPUResource* resource)
	{
		return resource != nullptr && resource->type == GPUResource::Type::RAYTRACING_ACCELERATION_STRUCTURE;
	}
	inline const GPUBufferDesc* wiGraphicsGPUBufferGetDesc(const GPUBuffer* buffer)
	{
		return buffer != nullptr ? &buffer->desc : nullptr;
	}
	inline const TextureDesc* wiGraphicsTextureGetDesc(const Texture* texture)
	{
		return texture != nullptr ? &texture->desc : nullptr;
	}
	inline bool wiGraphicsVideoDecoderIsValid(const VideoDecoder* video_decoder)
	{
		return video_decoder != nullptr && video_decoder->internal_state.IsValid();
	}
	inline const VideoDesc* wiGraphicsVideoDecoderGetDesc(const VideoDecoder* video_decoder)
	{
		return video_decoder != nullptr ? &video_decoder->desc : nullptr;
	}

		struct RenderPassImage
		{
		enum Type
		{
			RENDERTARGET,
			DEPTH_STENCIL,
			RESOLVE, // resolve render target (color)
			RESOLVE_DEPTH,
			SHADING_RATE_SOURCE
		} type = Type::RENDERTARGET;
		enum LoadOp
		{
			LOAD,
			CLEAR,
			LOADOP_DONTCARE,
		} loadop = LoadOp::LOAD;
		const Texture* texture = nullptr;
		int subresource = -1;
		enum StoreOp
		{
			STORE,
			STOREOP_DONTCARE,
		} storeop = StoreOp::STORE;
		ResourceState layout_before = ResourceState::UNDEFINED;	// layout before the render pass
		ResourceState layout = ResourceState::UNDEFINED;	// layout within the render pass
		ResourceState layout_after = ResourceState::UNDEFINED;	// layout after the render pass
			enum DepthResolveMode
			{
				Min,
				Max,
			} depth_resolve_mode = DepthResolveMode::Min;
		};
		inline RenderPassImage wiGraphicsCreateRenderPassImageRenderTarget(
			const Texture* resource,
			RenderPassImage::LoadOp load_op = RenderPassImage::LoadOp::LOAD,
			RenderPassImage::StoreOp store_op = RenderPassImage::StoreOp::STORE,
			ResourceState layout_before = ResourceState::SHADER_RESOURCE,
			ResourceState layout_after = ResourceState::SHADER_RESOURCE,
			int subresource_RTV = -1)
		{
			RenderPassImage image = {};
			image.type = RenderPassImage::Type::RENDERTARGET;
			image.texture = resource;
			image.loadop = load_op;
			image.storeop = store_op;
			image.layout_before = layout_before;
			image.layout = ResourceState::RENDERTARGET;
			image.layout_after = layout_after;
			image.subresource = subresource_RTV;
			return image;
		}
		inline RenderPassImage wiGraphicsCreateRenderPassImageDepthStencil(
			const Texture* resource,
			RenderPassImage::LoadOp load_op = RenderPassImage::LoadOp::LOAD,
			RenderPassImage::StoreOp store_op = RenderPassImage::StoreOp::STORE,
			ResourceState layout_before = ResourceState::DEPTHSTENCIL,
			ResourceState layout = ResourceState::DEPTHSTENCIL,
			ResourceState layout_after = ResourceState::DEPTHSTENCIL,
			int subresource_DSV = -1)
		{
			RenderPassImage image = {};
			image.type = RenderPassImage::Type::DEPTH_STENCIL;
			image.texture = resource;
			image.loadop = load_op;
			image.storeop = store_op;
			image.layout_before = layout_before;
			image.layout = layout;
			image.layout_after = layout_after;
			image.subresource = subresource_DSV;
			return image;
		}
		inline RenderPassImage wiGraphicsCreateRenderPassImageResolve(
			const Texture* resource,
			ResourceState layout_before = ResourceState::SHADER_RESOURCE,
			ResourceState layout_after = ResourceState::SHADER_RESOURCE,
			int subresource_SRV = -1)
		{
			RenderPassImage image = {};
			image.type = RenderPassImage::Type::RESOLVE;
			image.texture = resource;
			image.layout_before = layout_before;
			image.layout = ResourceState::COPY_DST;
			image.layout_after = layout_after;
			image.subresource = subresource_SRV;
			return image;
		}
		inline RenderPassImage wiGraphicsCreateRenderPassImageResolveDepth(
			const Texture* resource,
			RenderPassImage::DepthResolveMode depth_resolve_mode = RenderPassImage::DepthResolveMode::Min,
			ResourceState layout_before = ResourceState::SHADER_RESOURCE,
			ResourceState layout_after = ResourceState::SHADER_RESOURCE,
			int subresource_SRV = -1)
		{
			RenderPassImage image = {};
			image.type = RenderPassImage::Type::RESOLVE_DEPTH;
			image.texture = resource;
			image.layout_before = layout_before;
			image.layout = ResourceState::COPY_DST;
			image.layout_after = layout_after;
			image.subresource = subresource_SRV;
			image.depth_resolve_mode = depth_resolve_mode;
			return image;
		}
		inline RenderPassImage wiGraphicsCreateRenderPassImageShadingRateSource(
			const Texture* resource,
			ResourceState layout_before = ResourceState::SHADING_RATE_SOURCE,
			ResourceState layout_after = ResourceState::SHADING_RATE_SOURCE)
		{
			RenderPassImage image = {};
			image.type = RenderPassImage::Type::SHADING_RATE_SOURCE;
			image.texture = resource;
			image.layout_before = layout_before;
			image.layout = ResourceState::SHADING_RATE_SOURCE;
			image.layout_after = layout_after;
			return image;
		}

		struct RenderPassInfo
		{
			Format rt_formats[8] = {};
			uint32_t rt_count = 0;
			Format ds_format = Format::UNKNOWN;
		uint32_t sample_count = 1;

		constexpr uint64_t get_hash() const
		{
			union Hasher
			{
				struct
				{
					uint64_t rt_format_0 : 6;
					uint64_t rt_format_1 : 6;
					uint64_t rt_format_2 : 6;
					uint64_t rt_format_3 : 6;
					uint64_t rt_format_4 : 6;
					uint64_t rt_format_5 : 6;
					uint64_t rt_format_6 : 6;
					uint64_t rt_format_7 : 6;
					uint64_t ds_format : 6;
					uint64_t sample_count : 3;
				} bits;
				uint64_t value;
			} hasher = {};
			static_assert(sizeof(Hasher) == sizeof(uint64_t));
			hasher.bits.rt_format_0 = (uint64_t)rt_formats[0];
			hasher.bits.rt_format_1 = (uint64_t)rt_formats[1];
			hasher.bits.rt_format_2 = (uint64_t)rt_formats[2];
			hasher.bits.rt_format_3 = (uint64_t)rt_formats[3];
			hasher.bits.rt_format_4 = (uint64_t)rt_formats[4];
			hasher.bits.rt_format_5 = (uint64_t)rt_formats[5];
			hasher.bits.rt_format_6 = (uint64_t)rt_formats[6];
			hasher.bits.rt_format_7 = (uint64_t)rt_formats[7];
			hasher.bits.ds_format = (uint64_t)ds_format;
				hasher.bits.sample_count = (uint64_t)sample_count;
				return hasher.value;
			}
		};
		inline RenderPassInfo wiGraphicsCreateRenderPassInfoFromImages(const RenderPassImage* images, uint32_t image_count)
		{
			RenderPassInfo info = {};
		for (uint32_t i = 0; i < image_count; ++i)
		{
			const RenderPassImage& image = images[i];
			const TextureDesc* desc = wiGraphicsTextureGetDesc(image.texture);
			if (desc == nullptr)
			{
				continue;
			}
			switch (image.type)
			{
			case RenderPassImage::Type::RENDERTARGET:
				info.rt_formats[info.rt_count++] = desc->format;
				info.sample_count = desc->sample_count;
				break;
			case RenderPassImage::Type::DEPTH_STENCIL:
				info.ds_format = desc->format;
				info.sample_count = desc->sample_count;
				break;
			default:
				break;
				}
			}
			return info;
		}
		inline RenderPassInfo wiGraphicsCreateRenderPassInfoFromSwapChainDesc(const SwapChainDesc* swapchain_desc)
		{
			RenderPassInfo info = {};
			if (swapchain_desc != nullptr)
			{
				info.rt_count = 1;
				info.rt_formats[0] = swapchain_desc->format;
			}
			return info;
		}

	struct GPUQueryHeap
	{
		wi::allocator::shared_ptr<void> internal_state;

		GPUQueryHeapDesc desc;
	};
	inline bool wiGraphicsGPUQueryHeapIsValid(const GPUQueryHeap* query_heap)
	{
		return query_heap != nullptr && query_heap->internal_state.IsValid();
	}
	inline const GPUQueryHeapDesc* wiGraphicsGPUQueryHeapGetDesc(const GPUQueryHeap* query_heap)
	{
		return query_heap != nullptr ? &query_heap->desc : nullptr;
	}

	struct PipelineState
	{
		wi::allocator::shared_ptr<void> internal_state;

		PipelineStateDesc desc;
	};
	inline bool wiGraphicsPipelineStateIsValid(const PipelineState* pipeline_state)
	{
		return pipeline_state != nullptr && pipeline_state->internal_state.IsValid();
	}
	inline const PipelineStateDesc* wiGraphicsPipelineStateGetDesc(const PipelineState* pipeline_state)
	{
		return pipeline_state != nullptr ? &pipeline_state->desc : nullptr;
	}

	struct SwapChain
	{
		wi::allocator::shared_ptr<void> internal_state;

		SwapChainDesc desc;
	};
	inline bool wiGraphicsSwapChainIsValid(const SwapChain* swapchain)
	{
		return swapchain != nullptr && swapchain->internal_state.IsValid();
	}
	inline const SwapChainDesc* wiGraphicsSwapChainGetDesc(const SwapChain* swapchain)
	{
		return swapchain != nullptr ? &swapchain->desc : nullptr;
	}

	struct RaytracingAccelerationStructureDesc
	{
		enum Flags
		{
			FLAG_EMPTY = 0,
			FLAG_ALLOW_UPDATE = 1 << 0,
			FLAG_ALLOW_COMPACTION = 1 << 1,
			FLAG_PREFER_FAST_TRACE = 1 << 2,
			FLAG_PREFER_FAST_BUILD = 1 << 3,
			FLAG_MINIMIZE_MEMORY = 1 << 4,
		};
		uint32_t flags = FLAG_EMPTY;

		enum Type
		{
			BOTTOMLEVEL,
			TOPLEVEL,
		} type = Type::BOTTOMLEVEL;

		struct BottomLevel
		{
			struct Geometry
			{
				enum FLAGS
				{
					FLAG_EMPTY = 0,
					FLAG_OPAQUE = 1 << 0,
					FLAG_NO_DUPLICATE_ANYHIT_INVOCATION = 1 << 1,
					FLAG_USE_TRANSFORM = 1 << 2,
				};
				uint32_t flags = FLAG_EMPTY;

				enum Type
				{
					TRIANGLES,
					PROCEDURAL_AABBS,
				} type = Type::TRIANGLES;

				struct Triangles
				{
					GPUBuffer vertex_buffer;
					GPUBuffer index_buffer;
					uint32_t index_count = 0;
					uint64_t index_offset = 0;
					uint32_t vertex_count = 0;
					uint64_t vertex_byte_offset = 0;
					uint32_t vertex_stride = 0;
					IndexBufferFormat index_format = IndexBufferFormat::UINT32;
					Format vertex_format = Format::R32G32B32_FLOAT;
					GPUBuffer transform_3x4_buffer;
					uint32_t transform_3x4_buffer_offset = 0;
				} triangles;
				struct Procedural_AABBs
				{
					GPUBuffer aabb_buffer;
					uint32_t offset = 0;
					uint32_t count = 0;
					uint32_t stride = 0;
				} aabbs;

			};
			Geometry* geometries = nullptr; // stb_ds-backed geometry array
		} bottom_level;

		struct TopLevel
		{
			struct Instance
			{
				enum FLAGS
				{
					FLAG_EMPTY = 0,
					FLAG_TRIANGLE_CULL_DISABLE = 1 << 0,
					FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE = 1 << 1,
					FLAG_FORCE_OPAQUE = 1 << 2,
					FLAG_FORCE_NON_OPAQUE = 1 << 3,
				};
				float transform[3][4];
				uint32_t instance_id : 24;
				uint32_t instance_mask : 8;
				uint32_t instance_contribution_to_hit_group_index : 24;
				uint32_t flags : 8;
				const GPUResource* bottom_level = nullptr;
			};
			GPUBuffer instance_buffer;
			uint32_t offset = 0;
			uint32_t count = 0;
		} top_level;
	};
		struct RaytracingAccelerationStructure final : public GPUResource
		{
			RaytracingAccelerationStructureDesc desc;

			size_t size = 0;
		};
	inline const RaytracingAccelerationStructureDesc* wiGraphicsRaytracingAccelerationStructureGetDesc(const RaytracingAccelerationStructure* acceleration_structure)
	{
		return acceleration_structure != nullptr ? &acceleration_structure->desc : nullptr;
	}

	struct ShaderLibrary
	{
		enum Type
		{
			RAYGENERATION,
			MISS,
			CLOSESTHIT,
			ANYHIT,
			INTERSECTION,
		} type = Type::RAYGENERATION;
		const Shader* shader = nullptr;
		char* function_name = nullptr;
	};
	struct ShaderHitGroup
	{
		enum Type
		{
			GENERAL, // raygen or miss
			TRIANGLES,
			PROCEDURAL,
		} type = Type::TRIANGLES;
		char* name = nullptr;
		uint32_t general_shader = ~0u;
		uint32_t closest_hit_shader = ~0u;
		uint32_t any_hit_shader = ~0u;
		uint32_t intersection_shader = ~0u;
	};
	inline void InitRaytracingAccelerationStructureDesc(RaytracingAccelerationStructureDesc& desc)
	{
		desc.flags = RaytracingAccelerationStructureDesc::FLAG_EMPTY;
		desc.type = RaytracingAccelerationStructureDesc::Type::BOTTOMLEVEL;
		desc.bottom_level.geometries = nullptr;
		desc.top_level = {};
	}
	inline void InitShaderLibrary(ShaderLibrary& desc)
	{
		desc.type = ShaderLibrary::Type::RAYGENERATION;
		desc.shader = nullptr;
		desc.function_name = nullptr;
	}
	inline void DestroyShaderLibrary(ShaderLibrary& desc)
	{
		DestroyCString(desc.function_name);
		InitShaderLibrary(desc);
	}
	inline void CloneShaderLibrary(ShaderLibrary& dst, const ShaderLibrary& src)
	{
		DestroyShaderLibrary(dst);
		dst.type = src.type;
		dst.shader = src.shader;
		dst.function_name = CloneCString(src.function_name);
	}
	inline void InitShaderHitGroup(ShaderHitGroup& desc)
	{
		desc.type = ShaderHitGroup::Type::TRIANGLES;
		desc.name = nullptr;
		desc.general_shader = ~0u;
		desc.closest_hit_shader = ~0u;
		desc.any_hit_shader = ~0u;
		desc.intersection_shader = ~0u;
	}
	inline void DestroyShaderHitGroup(ShaderHitGroup& desc)
	{
		DestroyCString(desc.name);
		InitShaderHitGroup(desc);
	}
	inline void CloneShaderHitGroup(ShaderHitGroup& dst, const ShaderHitGroup& src)
	{
		DestroyShaderHitGroup(dst);
		dst.type = src.type;
		dst.name = CloneCString(src.name);
		dst.general_shader = src.general_shader;
		dst.closest_hit_shader = src.closest_hit_shader;
		dst.any_hit_shader = src.any_hit_shader;
		dst.intersection_shader = src.intersection_shader;
	}
	inline void DestroyRaytracingAccelerationStructureDesc(RaytracingAccelerationStructureDesc& desc)
	{
		arrfree(desc.bottom_level.geometries);
		InitRaytracingAccelerationStructureDesc(desc);
	}
	inline void CloneRaytracingAccelerationStructureDesc(RaytracingAccelerationStructureDesc& dst, const RaytracingAccelerationStructureDesc& src)
	{
		if (&dst == &src)
		{
			return;
		}
		DestroyRaytracingAccelerationStructureDesc(dst);
		dst.flags = src.flags;
		dst.type = src.type;
		dst.top_level = src.top_level;
		const size_t geometry_count = arrlenu(src.bottom_level.geometries);
		arrsetlen(dst.bottom_level.geometries, geometry_count);
		for (size_t i = 0; i < geometry_count; ++i)
		{
			dst.bottom_level.geometries[i] = src.bottom_level.geometries[i];
		}
	}

	struct RaytracingPipelineStateDesc
	{
		ShaderLibrary* shader_libraries = nullptr; // stb_ds-backed shader library array
		ShaderHitGroup* hit_groups = nullptr; // stb_ds-backed hit group array
		uint32_t max_trace_recursion_depth = 1;
		uint32_t max_attribute_size_in_bytes = 0;
		uint32_t max_payload_size_in_bytes = 0;
	};
	inline void InitRaytracingPipelineStateDesc(RaytracingPipelineStateDesc& desc)
	{
		desc.shader_libraries = nullptr;
		desc.hit_groups = nullptr;
		desc.max_trace_recursion_depth = 1;
		desc.max_attribute_size_in_bytes = 0;
		desc.max_payload_size_in_bytes = 0;
	}
	inline void DestroyRaytracingPipelineStateDesc(RaytracingPipelineStateDesc& desc)
	{
		if (desc.shader_libraries != nullptr)
		{
			for (size_t i = 0; i < arrlenu(desc.shader_libraries); ++i)
			{
				DestroyShaderLibrary(desc.shader_libraries[i]);
			}
		}
		if (desc.hit_groups != nullptr)
		{
			for (size_t i = 0; i < arrlenu(desc.hit_groups); ++i)
			{
				DestroyShaderHitGroup(desc.hit_groups[i]);
			}
		}
		arrfree(desc.shader_libraries);
		arrfree(desc.hit_groups);
		InitRaytracingPipelineStateDesc(desc);
	}
	inline void CloneRaytracingPipelineStateDesc(RaytracingPipelineStateDesc& dst, const RaytracingPipelineStateDesc& src)
	{
		if (&dst == &src)
		{
			return;
		}
		DestroyRaytracingPipelineStateDesc(dst);
		const size_t shader_library_count = arrlenu(src.shader_libraries);
		arrsetlen(dst.shader_libraries, shader_library_count);
		for (size_t i = 0; i < shader_library_count; ++i)
		{
			InitShaderLibrary(dst.shader_libraries[i]);
			CloneShaderLibrary(dst.shader_libraries[i], src.shader_libraries[i]);
		}
		const size_t hit_group_count = arrlenu(src.hit_groups);
		arrsetlen(dst.hit_groups, hit_group_count);
		for (size_t i = 0; i < hit_group_count; ++i)
		{
			InitShaderHitGroup(dst.hit_groups[i]);
			CloneShaderHitGroup(dst.hit_groups[i], src.hit_groups[i]);
		}
		dst.max_trace_recursion_depth = src.max_trace_recursion_depth;
		dst.max_attribute_size_in_bytes = src.max_attribute_size_in_bytes;
		dst.max_payload_size_in_bytes = src.max_payload_size_in_bytes;
	}
	struct RaytracingPipelineState
	{
		wi::allocator::shared_ptr<void> internal_state;

		RaytracingPipelineStateDesc desc;
	};
	inline bool wiGraphicsRaytracingPipelineStateIsValid(const RaytracingPipelineState* rtpso)
	{
		return rtpso != nullptr && rtpso->internal_state.IsValid();
	}
	inline const RaytracingPipelineStateDesc* wiGraphicsRaytracingPipelineStateGetDesc(const RaytracingPipelineState* rtpso)
	{
		return rtpso != nullptr ? &rtpso->desc : nullptr;
	}

	struct PipelineHash
	{
		const PipelineState* pso = nullptr;
		uint64_t renderpass_hash = 0;

		constexpr bool operator==(const PipelineHash& other) const
		{
			return (pso == other.pso) && (renderpass_hash == other.renderpass_hash);
		}
		constexpr uint64_t get_hash() const
		{
			union
			{
				const PipelineState* ptr;
				uint64_t value;
			} pso_hasher = {};
			static_assert(sizeof(pso_hasher) == sizeof(uint64_t));
			pso_hasher.ptr = pso; // reinterpret_cast in constexpr workaround
			return (pso_hasher.value ^ (renderpass_hash << 1)) >> 1;
		}
	};

	struct ShaderTable
	{
		const GPUBuffer* buffer = nullptr;
		uint64_t offset = 0;
		uint64_t size = 0;
		uint64_t stride = 0;
	};
	struct DispatchRaysDesc
	{
		ShaderTable ray_generation;
		ShaderTable miss;
		ShaderTable hit_group;
		ShaderTable callable;
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
	};

	struct SparseResourceCoordinate
	{
		uint32_t x = 0;		// tile offset of buffer or texture in width
		uint32_t y = 0;		// tile offset of texture in height
		uint32_t z = 0;		// tile offset of 3D texture in depth
		uint32_t mip = 0;	// mip level of texture resource
		uint32_t slice = 0;	// array slice of texture resource
	};
	struct SparseRegionSize
	{
		uint32_t width = 1;		// number of tiles to be mapped in X dimension (buffer or texture)
		uint32_t height = 1;	// number of tiles to be mapped in Y dimension (texture only)
		uint32_t depth = 1;		// number of tiles to be mapped in Z dimension (3D texture only)
	};
	enum TileRangeFlags
	{
		None = 0,		// map page to tile memory
		Null = 1 << 0,	// set page to null
	};
	struct SparseUpdateCommand
	{
		const GPUResource* sparse_resource = nullptr;			// the resource to do sparse mapping for (this requires resource to be created with ResourceMisc::SPARSE)
		uint32_t num_resource_regions = 0;						// number of: coordinates, sizes
		const SparseResourceCoordinate* coordinates = nullptr;	// mapping coordinates within sparse resource (num_resource_regions array size)
		const SparseRegionSize* sizes = nullptr;				// mapping sizes within sparse resource (num_resource_regions array size)
		const GPUBuffer* tile_pool = nullptr;					// this buffer must have been created with ResourceMisc::TILE_POOL
		const TileRangeFlags* range_flags = nullptr;			// flags (num_ranges array size)
		const uint32_t* range_start_offsets = nullptr;			// offset within tile pool (in pages) (num_ranges array size)
		const uint32_t* range_tile_counts = nullptr;			// number of tiles to be mapped (num_ranges array size)
	};


	constexpr bool IsFormatSRGB(Format format)
	{
		switch (format)
		{
		case Format::R8G8B8A8_UNORM_SRGB:
		case Format::B8G8R8A8_UNORM_SRGB:
		case Format::BC1_UNORM_SRGB:
		case Format::BC2_UNORM_SRGB:
		case Format::BC3_UNORM_SRGB:
		case Format::BC7_UNORM_SRGB:
			return true;
		default:
			return false;
		}
	}
	constexpr bool IsFormatUnorm(Format format)
	{
		switch (format)
		{
		case Format::R16G16B16A16_UNORM:
		case Format::R10G10B10A2_UNORM:
		case Format::R8G8B8A8_UNORM:
		case Format::R8G8B8A8_UNORM_SRGB:
		case Format::B8G8R8A8_UNORM:
		case Format::B8G8R8A8_UNORM_SRGB:
		case Format::R16G16_UNORM:
		case Format::D24_UNORM_S8_UINT:
		case Format::R8G8_UNORM:
		case Format::D16_UNORM:
		case Format::R16_UNORM:
		case Format::R8_UNORM:
		case Format::BC1_UNORM:
		case Format::BC1_UNORM_SRGB:
		case Format::BC2_UNORM:
		case Format::BC2_UNORM_SRGB:
		case Format::BC3_UNORM:
		case Format::BC3_UNORM_SRGB:
		case Format::BC4_UNORM:
		case Format::BC5_UNORM:
		case Format::BC7_UNORM:
		case Format::BC7_UNORM_SRGB:
			return true;
		default:
			return false;
		}
	}
	constexpr bool IsFormatBlockCompressed(Format format)
	{
		switch (format)
		{
		case Format::BC1_UNORM:
		case Format::BC1_UNORM_SRGB:
		case Format::BC2_UNORM:
		case Format::BC2_UNORM_SRGB:
		case Format::BC3_UNORM:
		case Format::BC3_UNORM_SRGB:
		case Format::BC4_UNORM:
		case Format::BC4_SNORM:
		case Format::BC5_UNORM:
		case Format::BC5_SNORM:
		case Format::BC6H_UF16:
		case Format::BC6H_SF16:
		case Format::BC7_UNORM:
		case Format::BC7_UNORM_SRGB:
			return true;
		default:
			return false;
		}
	}
	constexpr bool IsFormatDepthSupport(Format format)
	{
		switch (format)
		{
		case Format::D16_UNORM:
		case Format::D32_FLOAT:
		case Format::D32_FLOAT_S8X24_UINT:
		case Format::D24_UNORM_S8_UINT:
			return true;
		default:
			return false;
		}
	}
	constexpr bool IsFormatStencilSupport(Format format)
	{
		switch (format)
		{
		case Format::D32_FLOAT_S8X24_UINT:
		case Format::D24_UNORM_S8_UINT:
			return true;
		default:
			return false;
		}
	}
	constexpr uint32_t GetFormatBlockSize(Format format)
	{
		if(IsFormatBlockCompressed(format))
		{
			return 4u;
		}
		return 1u;
	}

	// Returns the byte size of one element for a given texture format
	//	For uncompressed formats, element = pixel
	//	For block compressed formats, element = block
	constexpr uint32_t GetFormatStride(Format format)
	{
		switch (format)
		{
		case Format::R32G32B32A32_FLOAT:
		case Format::R32G32B32A32_UINT:
		case Format::R32G32B32A32_SINT:
		case Format::BC2_UNORM:
		case Format::BC2_UNORM_SRGB:
		case Format::BC3_UNORM:
		case Format::BC3_UNORM_SRGB:
		case Format::BC5_SNORM:
		case Format::BC5_UNORM:
		case Format::BC6H_UF16:
		case Format::BC6H_SF16:
		case Format::BC7_UNORM:
		case Format::BC7_UNORM_SRGB:
			return 16u;

		case Format::R32G32B32_FLOAT:
		case Format::R32G32B32_UINT:
		case Format::R32G32B32_SINT:
			return 12u;

		case Format::R16G16B16A16_FLOAT:
		case Format::R16G16B16A16_UNORM:
		case Format::R16G16B16A16_UINT:
		case Format::R16G16B16A16_SNORM:
		case Format::R16G16B16A16_SINT:
		case Format::R32G32_FLOAT:
		case Format::R32G32_UINT:
		case Format::R32G32_SINT:
		case Format::D32_FLOAT_S8X24_UINT:
		case Format::BC1_UNORM:
		case Format::BC1_UNORM_SRGB:
		case Format::BC4_SNORM:
		case Format::BC4_UNORM:
			return 8u;

		case Format::R10G10B10A2_UNORM:
		case Format::R10G10B10A2_UINT:
		case Format::R11G11B10_FLOAT:
		case Format::R8G8B8A8_UNORM:
		case Format::R8G8B8A8_UNORM_SRGB:
		case Format::R8G8B8A8_UINT:
		case Format::R8G8B8A8_SNORM:
		case Format::R8G8B8A8_SINT:
		case Format::B8G8R8A8_UNORM:
		case Format::B8G8R8A8_UNORM_SRGB:
		case Format::R16G16_FLOAT:
		case Format::R16G16_UNORM:
		case Format::R16G16_UINT:
		case Format::R16G16_SNORM:
		case Format::R16G16_SINT:
		case Format::D32_FLOAT:
		case Format::R32_FLOAT:
		case Format::R32_UINT:
		case Format::R32_SINT:
		case Format::D24_UNORM_S8_UINT:
		case Format::R9G9B9E5_SHAREDEXP:
			return 4u;

		case Format::R8G8_UNORM:
		case Format::R8G8_UINT:
		case Format::R8G8_SNORM:
		case Format::R8G8_SINT:
		case Format::R16_FLOAT:
		case Format::D16_UNORM:
		case Format::R16_UNORM:
		case Format::R16_UINT:
		case Format::R16_SNORM:
		case Format::R16_SINT:
			return 2u;

		case Format::R8_UNORM:
		case Format::R8_UINT:
		case Format::R8_SNORM:
		case Format::R8_SINT:
			return 1u;


		default:
			assert(0); // didn't catch format!
			return 16u;
		}
	}
	constexpr Format GetFormatNonSRGB(Format format)
	{
		switch (format)
		{
		case Format::R8G8B8A8_UNORM_SRGB:
			return Format::R8G8B8A8_UNORM;
		case Format::B8G8R8A8_UNORM_SRGB:
			return Format::B8G8R8A8_UNORM;
		case Format::BC1_UNORM_SRGB:
			return Format::BC1_UNORM;
		case Format::BC2_UNORM_SRGB:
			return Format::BC2_UNORM;
		case Format::BC3_UNORM_SRGB:
			return Format::BC3_UNORM;
		case Format::BC7_UNORM_SRGB:
			return Format::BC7_UNORM;
		default:
			return format;
		}
	}
	constexpr Format GetFormatSRGB(Format format)
	{
		switch (format)
		{
		case Format::R8G8B8A8_UNORM:
		case Format::R8G8B8A8_UNORM_SRGB:
			return Format::R8G8B8A8_UNORM_SRGB;
		case Format::B8G8R8A8_UNORM:
		case Format::B8G8R8A8_UNORM_SRGB:
			return Format::B8G8R8A8_UNORM_SRGB;
		case Format::BC1_UNORM:
		case Format::BC1_UNORM_SRGB:
			return Format::BC1_UNORM_SRGB;
		case Format::BC2_UNORM:
		case Format::BC2_UNORM_SRGB:
			return Format::BC2_UNORM_SRGB;
		case Format::BC3_UNORM:
		case Format::BC3_UNORM_SRGB:
			return Format::BC3_UNORM_SRGB;
		case Format::BC7_UNORM:
		case Format::BC7_UNORM_SRGB:
			return Format::BC7_UNORM_SRGB;
		default:
			return Format::UNKNOWN;
		}
	}
	constexpr const char* GetFormatString(Format format)
	{
		switch (format)
		{
		case wi::Format::UNKNOWN:
			return "UNKNOWN";
		case wi::Format::R32G32B32A32_FLOAT:
			return "R32G32B32A32_FLOAT";
		case wi::Format::R32G32B32A32_UINT:
			return "R32G32B32A32_UINT";
		case wi::Format::R32G32B32A32_SINT:
			return "R32G32B32A32_SINT";
		case wi::Format::R32G32B32_FLOAT:
			return "R32G32B32_FLOAT";
		case wi::Format::R32G32B32_UINT:
			return "R32G32B32_UINT";
		case wi::Format::R32G32B32_SINT:
			return "R32G32B32_SINT";
		case wi::Format::R16G16B16A16_FLOAT:
			return "R16G16B16A16_FLOAT";
		case wi::Format::R16G16B16A16_UNORM:
			return "R16G16B16A16_UNORM";
		case wi::Format::R16G16B16A16_UINT:
			return "R16G16B16A16_UINT";
		case wi::Format::R16G16B16A16_SNORM:
			return "R16G16B16A16_SNORM";
		case wi::Format::R16G16B16A16_SINT:
			return "R16G16B16A16_SINT";
		case wi::Format::R32G32_FLOAT:
			return "R32G32_FLOAT";
		case wi::Format::R32G32_UINT:
			return "R32G32_UINT";
		case wi::Format::R32G32_SINT:
			return "R32G32_SINT";
		case wi::Format::D32_FLOAT_S8X24_UINT:
			return "D32_FLOAT_S8X24_UINT";
		case wi::Format::R10G10B10A2_UNORM:
			return "R10G10B10A2_UNORM";
		case wi::Format::R10G10B10A2_UINT:
			return "R10G10B10A2_UINT";
		case wi::Format::R11G11B10_FLOAT:
			return "R11G11B10_FLOAT";
		case wi::Format::R8G8B8A8_UNORM:
			return "R8G8B8A8_UNORM";
		case wi::Format::R8G8B8A8_UNORM_SRGB:
			return "R8G8B8A8_UNORM_SRGB";
		case wi::Format::R8G8B8A8_UINT:
			return "R8G8B8A8_UINT";
		case wi::Format::R8G8B8A8_SNORM:
			return "R8G8B8A8_SNORM";
		case wi::Format::R8G8B8A8_SINT:
			return "R8G8B8A8_SINT";
		case wi::Format::B8G8R8A8_UNORM:
			return "B8G8R8A8_UNORM";
		case wi::Format::B8G8R8A8_UNORM_SRGB:
			return "B8G8R8A8_UNORM_SRGB";
		case wi::Format::R16G16_FLOAT:
			return "R16G16_FLOAT";
		case wi::Format::R16G16_UNORM:
			return "R16G16_UNORM";
		case wi::Format::R16G16_UINT:
			return "R16G16_UINT";
		case wi::Format::R16G16_SNORM:
			return "R16G16_SNORM";
		case wi::Format::R16G16_SINT:
			return "R16G16_SINT";
		case wi::Format::D32_FLOAT:
			return "D32_FLOAT";
		case wi::Format::R32_FLOAT:
			return "R32_FLOAT";
		case wi::Format::R32_UINT:
			return "R32_UINT";
		case wi::Format::R32_SINT:
			return "R32_SINT";
		case wi::Format::D24_UNORM_S8_UINT:
			return "D24_UNORM_S8_UINT";
		case wi::Format::R9G9B9E5_SHAREDEXP:
			return "R9G9B9E5_SHAREDEXP";
		case wi::Format::R8G8_UNORM:
			return "R8G8_UNORM";
		case wi::Format::R8G8_UINT:
			return "R8G8_UINT";
		case wi::Format::R8G8_SNORM:
			return "R8G8_SNORM";
		case wi::Format::R8G8_SINT:
			return "R8G8_SINT";
		case wi::Format::R16_FLOAT:
			return "R16_FLOAT";
		case wi::Format::D16_UNORM:
			return "D16_UNORM";
		case wi::Format::R16_UNORM:
			return "R16_UNORM";
		case wi::Format::R16_UINT:
			return "R16_UINT";
		case wi::Format::R16_SNORM:
			return "R16_SNORM";
		case wi::Format::R16_SINT:
			return "R16_SINT";
		case wi::Format::R8_UNORM:
			return "R8_UNORM";
		case wi::Format::R8_UINT:
			return "R8_UINT";
		case wi::Format::R8_SNORM:
			return "R8_SNORM";
		case wi::Format::R8_SINT:
			return "R8_SINT";
		case wi::Format::BC1_UNORM:
			return "BC1_UNORM";
		case wi::Format::BC1_UNORM_SRGB:
			return "BC1_UNORM_SRGB";
		case wi::Format::BC2_UNORM:
			return "BC2_UNORM";
		case wi::Format::BC2_UNORM_SRGB:
			return "BC2_UNORM_SRGB";
		case wi::Format::BC3_UNORM:
			return "BC3_UNORM";
		case wi::Format::BC3_UNORM_SRGB:
			return "BC3_UNORM_SRGB";
		case wi::Format::BC4_UNORM:
			return "BC4_UNORM";
		case wi::Format::BC4_SNORM:
			return "BC4_SNORM";
		case wi::Format::BC5_UNORM:
			return "BC5_UNORM";
		case wi::Format::BC5_SNORM:
			return "BC5_SNORM";
		case wi::Format::BC6H_UF16:
			return "BC6H_UF16";
		case wi::Format::BC6H_SF16:
			return "BC6H_SF16";
		case wi::Format::BC7_UNORM:
			return "BC7_UNORM";
		case wi::Format::BC7_UNORM_SRGB:
			return "BC7_UNORM_SRGB";
		case wi::Format::NV12:
			return "NV12";
		default:
			return "";
		}
	}
	constexpr IndexBufferFormat GetIndexBufferFormat(Format format)
	{
		switch (format)
		{
		default:
		case Format::R32_UINT:
			return IndexBufferFormat::UINT32;
		case Format::R16_UINT:
			return IndexBufferFormat::UINT16;
		}
	}
	constexpr IndexBufferFormat GetIndexBufferFormat(uint32_t vertex_count)
	{
		return vertex_count > 65536 ? IndexBufferFormat::UINT32 : IndexBufferFormat::UINT16;
	}
	constexpr Format GetIndexBufferFormatRaw(uint32_t vertex_count)
	{
		return vertex_count > 65536 ? Format::R32_UINT : Format::R16_UINT;
	}
	constexpr uint32_t GetIndexStride(IndexBufferFormat format)
	{
		switch (format) {
			default:
			case IndexBufferFormat::UINT32:
				return sizeof(uint32_t);
			case IndexBufferFormat::UINT16:
				return sizeof(uint16_t);
		}
	}
	constexpr const char* GetIndexBufferFormatString(IndexBufferFormat format)
	{
		switch (format)
		{
		default:
		case IndexBufferFormat::UINT32:
			return "UINT32";
		case IndexBufferFormat::UINT16:
			return "UINT16";
		}
	}

	constexpr const char GetComponentSwizzleChar(ComponentSwizzle value)
	{
		switch (value)
		{
		default:
		case wi::ComponentSwizzle::R:
			return 'R';
		case wi::ComponentSwizzle::G:
			return 'G';
		case wi::ComponentSwizzle::B:
			return 'B';
		case wi::ComponentSwizzle::A:
			return 'A';
		case wi::ComponentSwizzle::SWIZZLE_ZERO:
			return '0';
		case wi::ComponentSwizzle::SWIZZLE_ONE:
			return '1';
		}
	}
	struct SwizzleString
	{
		char chars[5] = {};
		constexpr operator const char*() const { return chars; }
	};
	constexpr const SwizzleString GetSwizzleString(Swizzle swizzle)
	{
		SwizzleString ret;
		ret.chars[0] = GetComponentSwizzleChar(swizzle.r);
		ret.chars[1] = GetComponentSwizzleChar(swizzle.g);
		ret.chars[2] = GetComponentSwizzleChar(swizzle.b);
		ret.chars[3] = GetComponentSwizzleChar(swizzle.a);
		ret.chars[4] = 0;
		return ret;
	}
	constexpr Swizzle SwizzleFromString(const char* str)
	{
		Swizzle swizzle;
		if (str == nullptr)
			return swizzle;
		ComponentSwizzle* comp = (ComponentSwizzle*)&swizzle;
		for (int i = 0; i < 4; ++i)
		{
			switch (str[i])
			{
			case 'r':
			case 'R':
			case 'x':
			case 'X':
				*comp = ComponentSwizzle::R;
				break;
			case 'g':
			case 'G':
			case 'y':
			case 'Y':
				*comp = ComponentSwizzle::G;
				break;
			case 'b':
			case 'B':
			case 'z':
			case 'Z':
				*comp = ComponentSwizzle::B;
				break;
			case 'a':
			case 'A':
			case 'w':
			case 'W':
				*comp = ComponentSwizzle::A;
				break;
			case '0':
				*comp = ComponentSwizzle::SWIZZLE_ZERO;
				break;
			case '1':
				*comp = ComponentSwizzle::SWIZZLE_ONE;
				break;
			case 0:
				return swizzle;
			}
			comp++;
		}
		return swizzle;
	}

	// Get mipmap count for a given texture dimension.
	//	width, height, depth: dimensions of the texture
	//	min_dimension: break when all dimensions go below a specified dimension (optional, default: 1x1x1)
	constexpr uint32_t GetMipCount(uint32_t width, uint32_t height, uint32_t depth = 1u, uint32_t min_dimension = 1u)
	{
		uint32_t mips = 1;
		while (width > min_dimension || height > min_dimension || depth > min_dimension)
		{
			width = std::max(min_dimension, width >> 1u);
			height = std::max(min_dimension, height >> 1u);
			depth = std::max(min_dimension, depth >> 1u);
			mips++;
		}
		return mips;
	}

	// Get mipmap count from a texture description (if the description specified 0 mipmaps then this will return the max allowed)
	constexpr uint32_t GetMipCount(const TextureDesc& desc)
	{
		return desc.mip_levels == 0 ? GetMipCount(desc.width, desc.height, desc.depth) : desc.mip_levels;
	}

	// Returns the plane slice index for an aspect
	constexpr uint32_t GetPlaneSlice(ImageAspect aspect)
	{
		switch (aspect)
		{
		case wi::ImageAspect::COLOR:
		case wi::ImageAspect::DEPTH:
		case wi::ImageAspect::LUMINANCE:
			return 0;
		case wi::ImageAspect::STENCIL:
		case wi::ImageAspect::CHROMINANCE:
			return 1;
		default:
			assert(0); // invalid aspect
			break;
		}
		return 0;
	}

	// Computes the subresource index for indexing SubresourceData arrays
	constexpr uint32_t ComputeSubresource(uint32_t mip, uint32_t slice, uint32_t plane, uint32_t mip_count, uint32_t array_size)
	{
		return mip + slice * mip_count + plane * mip_count * array_size;
	}

	// Computes the subresource index for indexing SubresourceData arrays
	constexpr uint32_t ComputeSubresource(uint32_t mip, uint32_t slice, ImageAspect aspect, uint32_t mip_count, uint32_t array_size)
	{
		return ComputeSubresource(mip, slice, GetPlaneSlice(aspect), mip_count, array_size);
	}

	// Compute the texture memory usage for one row in a specific mip level
	constexpr size_t ComputeTextureMipRowPitch(const TextureDesc& desc, uint32_t mip)
	{
		const uint32_t bytes_per_block = GetFormatStride(desc.format);
		const uint32_t pixels_per_block = GetFormatBlockSize(desc.format);
		const uint32_t mip_width = std::max(1u, desc.width >> mip);
		const uint32_t num_blocks_x = (mip_width + pixels_per_block - 1) / pixels_per_block;
		return num_blocks_x * bytes_per_block * desc.sample_count;
	}

	// Compute the texture memory usage for one mip level
	constexpr size_t ComputeTextureMipMemorySizeInBytes(const TextureDesc& desc, uint32_t mip)
	{
		const uint32_t bytes_per_block = GetFormatStride(desc.format);
		const uint32_t pixels_per_block = GetFormatBlockSize(desc.format);
		const uint32_t mip_width = std::max(1u, desc.width >> mip);
		const uint32_t mip_height = std::max(1u, desc.height >> mip);
		const uint32_t mip_depth = std::max(1u, desc.depth >> mip);
		const uint32_t num_blocks_x = (mip_width + pixels_per_block - 1) / pixels_per_block;
		const uint32_t num_blocks_y = (mip_height + pixels_per_block - 1) / pixels_per_block;
		return num_blocks_x * num_blocks_y * mip_depth * bytes_per_block * desc.sample_count;
	}

	// Compute the approximate texture memory usage
	//	Approximate because this doesn't reflect GPU specific texture memory requirements, like alignment and metadata
	constexpr size_t ComputeTextureMemorySizeInBytes(const TextureDesc& desc)
	{
		size_t size = 0;
		const uint32_t bytes_per_block = GetFormatStride(desc.format);
		const uint32_t pixels_per_block = GetFormatBlockSize(desc.format);
		const uint32_t mips = GetMipCount(desc);
		for (uint32_t layer = 0; layer < desc.array_size; ++layer)
		{
			for (uint32_t mip = 0; mip < mips; ++mip)
			{
				const uint32_t mip_width = std::max(1u, desc.width >> mip);
				const uint32_t mip_height = std::max(1u, desc.height >> mip);
				const uint32_t mip_depth = std::max(1u, desc.depth >> mip);
				const uint32_t num_blocks_x = (mip_width + pixels_per_block - 1) / pixels_per_block;
				const uint32_t num_blocks_y = (mip_height + pixels_per_block - 1) / pixels_per_block;
				size += num_blocks_x * num_blocks_y * mip_depth * bytes_per_block;
			}
		}
		size *= desc.sample_count;
		return size;
	}

	constexpr uint32_t GetTextureSubresourceCount(const TextureDesc& desc)
	{
		const uint32_t mips = GetMipCount(desc);
		return desc.array_size * mips;
	}

	// Creates texture SubresourceData array
	//	alignment	: it can be used to force GPU-specific rowpitch alignment for linear tile mode (in bytes)
	inline void CreateTextureSubresourceDatas(const TextureDesc& desc, void* data_ptr, SubresourceData*& subresource_datas, uint32_t alignment = 1)
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


	// Deprecated, kept for back-compat:
		struct RenderPassAttachment
		{
		enum Type
		{
			RENDERTARGET,
			DEPTH_STENCIL,
			RESOLVE, // resolve render target (color)
			RESOLVE_DEPTH,
			SHADING_RATE_SOURCE
		} type = Type::RENDERTARGET;
		enum LoadOp
		{
			LOAD,
			CLEAR,
			LOADOP_DONTCARE,
		} loadop = LoadOp::LOAD;
		Texture texture;
		int subresource = -1;
		enum StoreOp
		{
			STORE,
			STOREOP_DONTCARE,
		} storeop = StoreOp::STORE;
		ResourceState initial_layout = ResourceState::UNDEFINED;	// layout before the render pass
		ResourceState subpass_layout = ResourceState::UNDEFINED;	// layout within the render pass
		ResourceState final_layout = ResourceState::UNDEFINED;		// layout after the render pass
			enum DepthResolveMode
			{
				Min,
				Max,
			} depth_resolve_mode = DepthResolveMode::Min;
		};
		inline RenderPassAttachment wiGraphicsCreateRenderPassAttachmentRenderTarget(
			const Texture& resource,
			RenderPassAttachment::LoadOp load_op = RenderPassAttachment::LoadOp::LOAD,
			RenderPassAttachment::StoreOp store_op = RenderPassAttachment::StoreOp::STORE,
			ResourceState initial_layout = ResourceState::SHADER_RESOURCE,
			ResourceState subpass_layout = ResourceState::RENDERTARGET,
			ResourceState final_layout = ResourceState::SHADER_RESOURCE,
			int subresource_RTV = -1)
		{
			RenderPassAttachment attachment = {};
			attachment.type = RenderPassAttachment::Type::RENDERTARGET;
			attachment.texture = resource;
			attachment.loadop = load_op;
			attachment.storeop = store_op;
			attachment.initial_layout = initial_layout;
			attachment.subpass_layout = subpass_layout;
			attachment.final_layout = final_layout;
			attachment.subresource = subresource_RTV;
			return attachment;
		}
		inline RenderPassAttachment wiGraphicsCreateRenderPassAttachmentDepthStencil(
			const Texture& resource,
			RenderPassAttachment::LoadOp load_op = RenderPassAttachment::LoadOp::LOAD,
			RenderPassAttachment::StoreOp store_op = RenderPassAttachment::StoreOp::STORE,
			ResourceState initial_layout = ResourceState::DEPTHSTENCIL,
			ResourceState subpass_layout = ResourceState::DEPTHSTENCIL,
			ResourceState final_layout = ResourceState::DEPTHSTENCIL,
			int subresource_DSV = -1)
		{
			RenderPassAttachment attachment = {};
			attachment.type = RenderPassAttachment::Type::DEPTH_STENCIL;
			attachment.texture = resource;
			attachment.loadop = load_op;
			attachment.storeop = store_op;
			attachment.initial_layout = initial_layout;
			attachment.subpass_layout = subpass_layout;
			attachment.final_layout = final_layout;
			attachment.subresource = subresource_DSV;
			return attachment;
		}
		inline RenderPassAttachment wiGraphicsCreateRenderPassAttachmentResolve(
			const Texture& resource,
			ResourceState initial_layout = ResourceState::SHADER_RESOURCE,
			ResourceState final_layout = ResourceState::SHADER_RESOURCE,
			int subresource_SRV = -1)
		{
			RenderPassAttachment attachment = {};
			attachment.type = RenderPassAttachment::Type::RESOLVE;
			attachment.texture = resource;
			attachment.initial_layout = initial_layout;
			attachment.final_layout = final_layout;
			attachment.subresource = subresource_SRV;
			return attachment;
		}
		inline RenderPassAttachment wiGraphicsCreateRenderPassAttachmentResolveDepth(
			const Texture& resource,
			RenderPassAttachment::DepthResolveMode depth_resolve_mode = RenderPassAttachment::DepthResolveMode::Min,
			ResourceState initial_layout = ResourceState::SHADER_RESOURCE,
			ResourceState final_layout = ResourceState::SHADER_RESOURCE,
			int subresource_SRV = -1)
		{
			RenderPassAttachment attachment = {};
			attachment.type = RenderPassAttachment::Type::RESOLVE_DEPTH;
			attachment.texture = resource;
			attachment.initial_layout = initial_layout;
			attachment.final_layout = final_layout;
			attachment.subresource = subresource_SRV;
			attachment.depth_resolve_mode = depth_resolve_mode;
			return attachment;
		}
		inline RenderPassAttachment wiGraphicsCreateRenderPassAttachmentShadingRateSource(
			const Texture& resource,
			ResourceState initial_layout = ResourceState::SHADING_RATE_SOURCE,
			ResourceState final_layout = ResourceState::SHADING_RATE_SOURCE)
		{
			RenderPassAttachment attachment = {};
			attachment.type = RenderPassAttachment::Type::SHADING_RATE_SOURCE;
			attachment.texture = resource;
			attachment.initial_layout = initial_layout;
			attachment.subpass_layout = ResourceState::SHADING_RATE_SOURCE;
			attachment.final_layout = final_layout;
			return attachment;
		}
		inline RenderPassImage wiGraphicsConvertRenderPassAttachmentToImage(const RenderPassAttachment* attachment)
		{
			RenderPassImage image = {};
			if (attachment == nullptr)
			{
				return image;
			}
			switch (attachment->type)
			{
			default:
			case RenderPassAttachment::Type::RENDERTARGET:
				image.type = RenderPassImage::Type::RENDERTARGET;
				break;
			case RenderPassAttachment::Type::DEPTH_STENCIL:
				image.type = RenderPassImage::Type::DEPTH_STENCIL;
				break;
			case RenderPassAttachment::Type::RESOLVE:
				image.type = RenderPassImage::Type::RESOLVE;
				break;
			case RenderPassAttachment::Type::RESOLVE_DEPTH:
				image.type = RenderPassImage::Type::RESOLVE_DEPTH;
				break;
			case RenderPassAttachment::Type::SHADING_RATE_SOURCE:
				image.type = RenderPassImage::Type::SHADING_RATE_SOURCE;
				break;
			}
			switch (attachment->depth_resolve_mode)
			{
			default:
			case RenderPassAttachment::DepthResolveMode::Min:
				image.depth_resolve_mode = RenderPassImage::DepthResolveMode::Min;
				break;
			case RenderPassAttachment::DepthResolveMode::Max:
				image.depth_resolve_mode = RenderPassImage::DepthResolveMode::Max;
				break;
			}
			switch (attachment->loadop)
			{
			case RenderPassAttachment::LoadOp::LOAD:
				image.loadop = RenderPassImage::LoadOp::LOAD;
				break;
			case RenderPassAttachment::LoadOp::CLEAR:
				image.loadop = RenderPassImage::LoadOp::CLEAR;
				break;
			case RenderPassAttachment::LoadOp::LOADOP_DONTCARE:
				image.loadop = RenderPassImage::LoadOp::LOADOP_DONTCARE;
				break;
			default:
				break;
			}
			switch (attachment->storeop)
			{
			case RenderPassAttachment::StoreOp::STORE:
				image.storeop = RenderPassImage::StoreOp::STORE;
				break;
			case RenderPassAttachment::StoreOp::STOREOP_DONTCARE:
				image.storeop = RenderPassImage::StoreOp::STOREOP_DONTCARE;
				break;
			default:
				break;
			}
			image.layout_before = attachment->initial_layout;
			image.layout = attachment->subpass_layout;
			image.layout_after = attachment->final_layout;
			image.texture = &attachment->texture;
			image.subresource = attachment->subresource;
			return image;
		}
	// Deprecated, kept for back-compat:
	struct RenderPassDesc
	{
		enum Flags
		{
			EMPTY = 0,
			ALLOW_UAV_WRITES = 1 << 0,
		};
		Flags flags = Flags::EMPTY;
		RenderPassAttachment* attachments = nullptr; // stb_ds-backed render pass attachment array
	};
	// Deprecated, kept for back-compat:
	struct RenderPass
	{
		bool valid = false;
		RenderPassDesc desc;
	};
	inline const RenderPassDesc* wiGraphicsRenderPassGetDesc(const RenderPass* renderpass)
	{
		return renderpass != nullptr ? &renderpass->desc : nullptr;
	}
	inline bool wiGraphicsRenderPassIsValid(const RenderPass* renderpass)
	{
		return renderpass != nullptr && renderpass->valid;
	}

	inline void InitRenderPassDesc(RenderPassDesc& desc)
	{
		desc.flags = RenderPassDesc::Flags::EMPTY;
		desc.attachments = nullptr;
	}
	inline void DestroyRenderPassDesc(RenderPassDesc& desc)
	{
		arrfree(desc.attachments);
		InitRenderPassDesc(desc);
	}
	inline void CloneRenderPassDesc(RenderPassDesc& dst, const RenderPassDesc& src)
	{
		if (&dst == &src)
		{
			return;
		}
		DestroyRenderPassDesc(dst);
		dst.flags = src.flags;
		const size_t attachment_count = arrlenu(src.attachments);
		arrsetlen(dst.attachments, attachment_count);
		for (size_t i = 0; i < attachment_count; ++i)
		{
			dst.attachments[i] = src.attachments[i];
		}
	}
}

template<>
struct enable_bitmask_operators<wi::ColorWrite> {
	static const bool enable = true;
};
template<>
struct enable_bitmask_operators<wi::BindFlag> {
	static const bool enable = true;
};
template<>
struct enable_bitmask_operators<wi::ResourceMiscFlag> {
	static const bool enable = true;
};
template<>
struct enable_bitmask_operators<wi::GraphicsDeviceCapability> {
	static const bool enable = true;
};
template<>
struct enable_bitmask_operators<wi::ResourceState> {
	static const bool enable = true;
};
template<>
struct enable_bitmask_operators<wi::RenderPassDesc::Flags> {
	static const bool enable = true;
};
template<>
struct enable_bitmask_operators<wi::RenderPassFlags> {
	static const bool enable = true;
};
template<>
struct enable_bitmask_operators<wi::VideoDecoderSupportFlags> {
	static const bool enable = true;
};

namespace std
{
	template <>
	struct hash<wi::PipelineHash>
	{
		inline uint64_t operator()(const wi::PipelineHash& hash) const
		{
			return hash.get_hash();
		}
	};
}
