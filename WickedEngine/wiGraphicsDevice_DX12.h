#pragma once
#include "CommonInclude.h"
#include "wiPlatform.h"

#ifdef _WIN32
#define WICKEDENGINE_BUILD_DX12
#endif // _WIN32

#ifdef WICKEDENGINE_BUILD_DX12
#include "wiGraphicsDevice.h"
#include "wiSpinLock.h"
#include "../stb_ds.h"

#include <cassert>
#include <type_traits>

#if __has_include(<SDL3/SDL_assert.h>) && __has_include(<SDL3/SDL_log.h>) && __has_include(<SDL3/SDL_stdinc.h>)
#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#define WI_DX12_HAS_SDL 1
#else
#define WI_DX12_HAS_SDL 0
#endif

#ifdef PLATFORM_XBOX
#include "wiGraphicsDevice_DX12_XBOX.h"
#else
#include "Utility/dx12/d3d12.h"
#include "Utility/dx12/d3d12video.h"
#include <dxgi1_6.h>
#define PPV_ARGS(x) IID_PPV_ARGS(&x)
#endif // PLATFORM_XBOX

#include <wrl/client.h> // ComPtr

#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#define __ID3D12Device1_INTERFACE_DEFINED__
#ifdef PLATFORM_XBOX
#define D3D12MA_OPTIONS16_SUPPORTED 0
#endif // PLATFORM_XBOX
#include "Utility/D3D12MemAlloc.h"

#include <deque>
#include <atomic>
#include <mutex>

#if WI_DX12_HAS_SDL
#define WI_DX12_LOG_ERROR(...) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define WI_DX12_LOG_WARN(...) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define WI_DX12_ASSERT(cond) SDL_assert(cond)
#else
#define WI_DX12_LOG_ERROR(...) do { } while (0)
#define WI_DX12_LOG_WARN(...) do { } while (0)
#define WI_DX12_ASSERT(cond) assert(cond)
#endif

#if !defined(SDL_arraysize)
#define SDL_arraysize(array) (sizeof(array) / sizeof((array)[0]))
#endif

#define dx12_assert(cond, fname) do { const bool dx12_condition = static_cast<bool>(cond); if (!dx12_condition) { WI_DX12_LOG_ERROR("DX12 error: %s failed (hr=0x%08X) (%s:%d)", fname, (unsigned int)hr, __FILE__, __LINE__); WI_DX12_ASSERT(dx12_condition); } } while (0)
#define dx12_check(call) [&]() { HRESULT hr = call; dx12_assert(SUCCEEDED(hr), #call); return hr; }()

namespace wi
{
	namespace dx12_internal
	{
		template<typename T>
		inline void destroy_stb_array(T*& data)
		{
			if (data != nullptr)
			{
				// stb_ds array: destroy contained objects explicitly before arrfree().
				if constexpr (!std::is_trivially_destructible_v<T>)
				{
					for (size_t i = 0; i < arrlenu(data); ++i)
					{
						data[i].~T();
					}
				}
				arrfree(data);
			}
		}

		template<typename T>
		inline T pop_back_stb_array(T*& data)
		{
			WI_DX12_ASSERT(data != nullptr);
			WI_DX12_ASSERT(arrlenu(data) > 0);
			T value = std::move(data[arrlenu(data) - 1]);
			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				data[arrlenu(data) - 1].~T();
			}
			arrsetlen(data, arrlenu(data) - 1);
			return value;
		}

	}

	class GraphicsDevice_DX12 final : public GraphicsDevice
	{
	protected:
#ifndef PLATFORM_XBOX
		Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
#endif // PLATFORM_XBOX
		Microsoft::WRL::ComPtr<ID3D12Device5> device;
		Microsoft::WRL::ComPtr<ID3D12VideoDevice> video_device;

#ifdef PLATFORM_WINDOWS_DESKTOP
		Microsoft::WRL::ComPtr<ID3D12Fence> deviceRemovedFence;
		HANDLE deviceRemovedWaitHandle = {};
#endif // PLATFORM_WINDOWS_DESKTOP

		Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatchIndirectCommandSignature;
		Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawInstancedIndirectCommandSignature;
		Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndexedInstancedIndirectCommandSignature;
		Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatchMeshIndirectCommandSignature;

		// Multi count draw command signatures (when drawID is used) need to be created with valid root signature, so they are delayed until shader creation:
		struct MultiDrawSignature
		{
			Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawInstancedIndirectCountCommandSignature;
			Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndexedInstancedIndirectCountCommandSignature;
			Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatchMeshIndirectCountCommandSignature;
		};
		struct MultiDrawCacheEntry
		{
			ID3D12RootSignature* key = nullptr;
			MultiDrawSignature* value = nullptr;
		};
		struct PipelineCacheEntry
		{
			PipelineHash key = {};
			PipelineState* value = nullptr;
		};
		mutable std::mutex multidraw_signature_locker;
		// stb_ds hash map: keyed by root signature pointer, destroyed explicitly in the cpp teardown path.
		mutable MultiDrawCacheEntry* multidraw_signatures = nullptr;

		// stb_ds array: decode profile list is rebuilt explicitly and freed with arrfree().
		GUID* video_decode_profile_list = nullptr;

		bool deviceRemoved = false;
		bool tearingSupported = false;
		bool additionalShadingRatesSupported = false;
		bool casting_fully_typed_formats = false;

		uint32_t rtv_descriptor_size = 0;
		uint32_t dsv_descriptor_size = 0;
		uint32_t resource_descriptor_size = 0;
		uint32_t sampler_descriptor_size = 0;
		D3D12_RESOURCE_HEAP_TIER resource_heap_tier = {};

		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> nulldescriptorheap_cbv_srv_uav;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> nulldescriptorheap_sampler;
		D3D12_CPU_DESCRIPTOR_HANDLE nullCBV = {};
		D3D12_CPU_DESCRIPTOR_HANDLE nullSRV = {};
		D3D12_CPU_DESCRIPTOR_HANDLE nullUAV = {};
		D3D12_CPU_DESCRIPTOR_HANDLE nullSAM = {};

		struct Semaphore
		{
			Microsoft::WRL::ComPtr<ID3D12Fence> fence;
			uint64_t fenceValue = 0;
		};

		struct CommandQueue
		{
			D3D12_COMMAND_QUEUE_DESC desc = {};
			Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
			ID3D12CommandList** submit_cmds = nullptr;

			void signal(const Semaphore& semaphore);
			void wait(const Semaphore& semaphore);
			void submit();

			~CommandQueue()
			{
				dx12_internal::destroy_stb_array(submit_cmds);
			}
		} queues[QUEUE_COUNT];

		struct CopyAllocator
		{
			GraphicsDevice_DX12* device = nullptr;
			Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue; // create separate copy queue to reduce interference with main QUEUE_COPY
			std::mutex locker;

			struct CopyCMD
			{
				Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
				Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
				GPUBuffer uploadbuffer;
				uint64_t submittedValue = 0;
				inline bool IsValid() const { return commandList != nullptr; }
			};
			CopyCMD** freelist = nullptr;
			CopyCMD** inflight = nullptr;

			void init(GraphicsDevice_DX12* device);
			void recycle_completed_unlocked();
			CopyCMD allocate(uint64_t staging_size);
			void recycle_completed();
			UploadTicket submit(CopyCMD cmd);

			~CopyAllocator()
			{
				if (freelist != nullptr)
				{
					for (size_t i = 0; i < arrlenu(freelist); ++i)
					{
						delete freelist[i];
					}
				}
				dx12_internal::destroy_stb_array(freelist);
				if (inflight != nullptr)
				{
					for (size_t i = 0; i < arrlenu(inflight); ++i)
					{
						delete inflight[i];
					}
				}
				dx12_internal::destroy_stb_array(inflight);
			}
		};
		mutable CopyAllocator copyAllocator;

		uint64_t frame_fence_values[BUFFERCOUNT] = {};
		Microsoft::WRL::ComPtr<ID3D12Fence> frame_fence[BUFFERCOUNT][QUEUE_COUNT];
		Microsoft::WRL::ComPtr<ID3D12Fence> token_fence[QUEUE_COUNT];
		std::atomic<uint64_t> token_fence_values[QUEUE_COUNT] = {};
		mutable std::mutex upload_token_locker;
		mutable SubmissionToken pending_implicit_uploads = {};

		struct DescriptorBinder
		{
			DescriptorBindingTable table;
			GraphicsDevice_DX12* device = nullptr;

			const void* optimizer_graphics = nullptr;
			uint64_t dirty_graphics = 0ull; // 1 dirty bit flag per root parameter
			const void* optimizer_compute = nullptr;
			uint64_t dirty_compute = 0ull; // 1 dirty bit flag per root parameter

			void init(GraphicsDevice_DX12* device);
			void reset();
			void flush(bool graphics, CommandList cmd);
		};

		Semaphore** semaphore_pool = nullptr;
		std::mutex semaphore_pool_locker;
		Semaphore new_semaphore()
		{
			std::scoped_lock lck(semaphore_pool_locker);
			if (semaphore_pool == nullptr || arrlenu(semaphore_pool) == 0)
			{
				Semaphore dependency = {};
				dx12_check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, PPV_ARGS(dependency.fence)));
				dx12_check(dependency.fence.Get()->SetName(L"DependencySemaphore"));
				dependency.fenceValue++;
				return dependency;
			}
			Semaphore* dependency = dx12_internal::pop_back_stb_array(semaphore_pool);
			Semaphore semaphore = *dependency;
			delete dependency;
			semaphore.fenceValue++;
			return semaphore;
		}
		void free_semaphore(const Semaphore& semaphore)
		{
			std::scoped_lock lck(semaphore_pool_locker);
			arrput(semaphore_pool, new Semaphore(semaphore));
		}

		struct CommandList_DX12
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators[BUFFERCOUNT][QUEUE_COUNT];
			Microsoft::WRL::ComPtr<ID3D12CommandList> commandLists[QUEUE_COUNT];
			using graphics_command_list_version = ID3D12GraphicsCommandList6;
			uint32_t buffer_index = 0;

			QUEUE_TYPE queue = {};
			uint32_t id = 0;
			Semaphore** waits = nullptr;
			Semaphore** signals = nullptr;

			DescriptorBinder binder;
			GPULinearAllocator frame_allocators[BUFFERCOUNT];

			D3D12_RESOURCE_BARRIER* frame_barriers = nullptr;
			struct Discard
			{
				ID3D12Resource* resource = nullptr;
				D3D12_DISCARD_REGION region = {};
			};
			Discard* discards = nullptr;
			D3D_PRIMITIVE_TOPOLOGY prev_pt = {};
			PipelineCacheEntry** pipelines_worker = nullptr;
			PipelineHash prev_pipeline_hash = {};
			const PipelineState* active_pso = {};
			const Shader* active_cs = {};
			const RaytracingPipelineState* active_rt = {};
			const ID3D12RootSignature* active_rootsig_graphics = {};
			const ID3D12RootSignature* active_rootsig_compute = {};
			ShadingRate prev_shadingrate = {};
			uint32_t prev_stencilref = 0;
			const SwapChain** swapchains = nullptr;
			bool dirty_pso = {};
			D3D12_RAYTRACING_GEOMETRY_DESC* accelerationstructure_build_geometries = nullptr;
			RenderPassInfo renderpass_info;
			D3D12_RESOURCE_BARRIER* renderpass_barriers_begin = nullptr;
			D3D12_RESOURCE_BARRIER* renderpass_barriers_begin_after_discards = nullptr;
			D3D12_RESOURCE_BARRIER* renderpass_barriers_end = nullptr;
			ID3D12Resource* shading_rate_image = nullptr;
			ID3D12Resource* resolve_src[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D12Resource* resolve_dst[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			DXGI_FORMAT resolve_formats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D12Resource* resolve_src_ds = nullptr;
			ID3D12Resource* resolve_dst_ds = nullptr;
			DXGI_FORMAT resolve_ds_format = {};
			D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS* resolve_subresources[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS* resolve_subresources_dsv = nullptr;
			D3D12_RESOURCE_BARRIER* resolve_src_barriers = nullptr;

			void reset(uint32_t bufferindex)
			{
				buffer_index = bufferindex;
				if (waits != nullptr)
				{
					for (size_t i = 0; i < arrlenu(waits); ++i)
					{
						delete waits[i];
					}
				}
				dx12_internal::destroy_stb_array(waits);
				if (signals != nullptr)
				{
					for (size_t i = 0; i < arrlenu(signals); ++i)
					{
						delete signals[i];
					}
				}
				dx12_internal::destroy_stb_array(signals);
				binder.reset();
				frame_allocators[buffer_index].reset();
				prev_pt = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
				prev_pipeline_hash = {};
				active_pso = nullptr;
				active_cs = nullptr;
				active_rt = nullptr;
				active_rootsig_graphics = nullptr;
				active_rootsig_compute = nullptr;
				prev_shadingrate = ShadingRate::RATE_INVALID;
				prev_stencilref = 0;
				dirty_pso = false;
				dx12_internal::destroy_stb_array(swapchains);
				renderpass_info = {};
				dx12_internal::destroy_stb_array(frame_barriers);
				dx12_internal::destroy_stb_array(discards);
				if (pipelines_worker != nullptr)
				{
					for (size_t i = 0; i < arrlenu(pipelines_worker); ++i)
					{
						delete pipelines_worker[i]->value;
						delete pipelines_worker[i];
					}
				}
				dx12_internal::destroy_stb_array(pipelines_worker);
				dx12_internal::destroy_stb_array(accelerationstructure_build_geometries);
				dx12_internal::destroy_stb_array(renderpass_barriers_begin);
				dx12_internal::destroy_stb_array(renderpass_barriers_begin_after_discards);
				dx12_internal::destroy_stb_array(renderpass_barriers_end);
				for (size_t i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
				{
					resolve_src[i] = {};
					resolve_dst[i] = {};
					dx12_internal::destroy_stb_array(resolve_subresources[i]);
				}
				dx12_internal::destroy_stb_array(resolve_subresources_dsv);
				dx12_internal::destroy_stb_array(resolve_src_barriers);
				resolve_src_ds = nullptr;
				resolve_dst_ds = nullptr;
				shading_rate_image = nullptr;
			}

			inline ID3D12CommandAllocator* GetCommandAllocator()
			{
				return commandAllocators[buffer_index][queue].Get();
			}
			inline ID3D12CommandList* GetCommandList()
			{
				return commandLists[queue].Get();
			}
			inline ID3D12GraphicsCommandList* GetGraphicsCommandList()
			{
				WI_DX12_ASSERT(queue != QUEUE_VIDEO_DECODE);
				return (ID3D12GraphicsCommandList*)commandLists[queue].Get();
			}
			inline graphics_command_list_version* GetGraphicsCommandListLatest()
			{
				WI_DX12_ASSERT(queue != QUEUE_VIDEO_DECODE && queue != QUEUE_COPY);
				return (graphics_command_list_version*)commandLists[queue].Get();
			}
			inline ID3D12VideoDecodeCommandList* GetVideoDecodeCommandList()
			{
				WI_DX12_ASSERT(queue == QUEUE_VIDEO_DECODE);
				return (ID3D12VideoDecodeCommandList*)commandLists[queue].Get();
			}

			~CommandList_DX12()
			{
				reset(buffer_index);
			}
		};
		wi::allocator::BlockAllocator<CommandList_DX12, 64> cmd_allocator;
		CommandList_DX12** commandlists = nullptr;
		uint32_t cmd_count = 0;
		wi::SpinLock cmd_locker;

		constexpr CommandList_DX12& GetCommandList(CommandList cmd) const
		{
			WI_DX12_ASSERT(wiGraphicsCommandListIsValid(cmd));
			return *(CommandList_DX12*)cmd.internal_state;
		}

		// stb_ds hash map/array: pipeline cache entries are owned explicitly and cleared in device teardown.
		PipelineCacheEntry* pipelines_global = nullptr;

		void pso_validate(CommandList cmd);

		void predraw(CommandList cmd);
		void predispatch(CommandList cmd);
		struct UploadDescInternal
		{
			enum class Type
			{
				BUFFER,
				TEXTURE,
			};
			Type type = Type::BUFFER;
			const void* src_data = nullptr;
			uint64_t src_size = 0;
			const GPUBuffer* dst_buffer = nullptr;
			uint64_t dst_offset = 0;
			const Texture* dst_texture = nullptr;
			const SubresourceData* subresources = nullptr;
			uint32_t subresource_count = 0;
			ResourceState texture_final_layout = ResourceState::SHADER_RESOURCE;
		};
		SubmissionToken SubmitCommandListsInternal();
		UploadTicket UploadAsyncInternal(const UploadDescInternal& upload);

	public:
		GraphicsDevice_DX12(ValidationMode validationMode = ValidationMode::Disabled, GPUPreference preference = GPUPreference::Discrete);
		~GraphicsDevice_DX12() override;

		bool CreateSwapChain(const SwapChainDesc* desc, wi::platform::window_type window, SwapChain* swapchain) const override;
		bool CreateBuffer2(const GPUBufferDesc * desc, const std::function<void(void*)>& init_callback, GPUBuffer* buffer, const GPUResource* alias = nullptr, uint64_t alias_offset = 0ull) const override;
		bool CreateTexture(const TextureDesc* desc, const SubresourceData* initial_data, Texture* texture, const GPUResource* alias = nullptr, uint64_t alias_offset = 0ull) const override;
		bool CreateShader(ShaderStage stage, const void* shadercode, size_t shadercode_size, Shader* shader, const char* entrypoint = "main") const override;
		bool CreateSampler(const SamplerDesc* desc, Sampler* sampler) const override;
		bool CreateQueryHeap(const GPUQueryHeapDesc* desc, GPUQueryHeap* queryheap) const override;
		bool CreatePipelineState(const PipelineStateDesc* desc, PipelineState* pso, const RenderPassInfo* renderpass_info = nullptr) const override;
		bool CreateRaytracingAccelerationStructure(const RaytracingAccelerationStructureDesc* desc, RaytracingAccelerationStructure* bvh) const override;
		bool CreateRaytracingPipelineState(const RaytracingPipelineStateDesc* desc, RaytracingPipelineState* rtpso) const override;
		bool CreateVideoDecoder(const VideoDesc* desc, VideoDecoder* video_decoder) const override;

		int CreateSubresource(Texture* texture, SubresourceType type, uint32_t firstSlice, uint32_t sliceCount, uint32_t firstMip, uint32_t mipCount, const Format* format_change = nullptr, const ImageAspect* aspect = nullptr, const Swizzle* swizzle = nullptr, float min_lod_clamp = 0) const override;
		int CreateSubresource(GPUBuffer* buffer, SubresourceType type, uint64_t offset, uint64_t size = ~0, const Format* format_change = nullptr, const uint32_t* structuredbuffer_stride_change = nullptr) const override;

		void DeleteSubresources(GPUResource* resource) override;

		int GetDescriptorIndex(const GPUResource* resource, SubresourceType type, int subresource = -1) const override;
		int GetDescriptorIndex(const Sampler* sampler) const override;

		void WriteShadingRateValue(ShadingRate rate, void* dest) const override;
		void WriteTopLevelAccelerationStructureInstance(const RaytracingAccelerationStructureDesc::TopLevel::Instance* instance, void* dest) const override;
		void WriteShaderIdentifier(const RaytracingPipelineState* rtpso, uint32_t group_index, void* dest) const override;

		void SetName(GPUResource* pResource, const char* name) const override;

		CommandList BeginCommandList(QUEUE_TYPE queue = QUEUE_GRAPHICS) override;
		SubmissionToken SubmitCommandListsEx(const SubmitDesc& desc) override;
		bool IsQueuePointComplete(QueueSyncPoint point) const override;
		void WaitQueuePoint(QueueSyncPoint point) const override;
		QueueSyncPoint GetLastSubmittedQueuePoint(QUEUE_TYPE queue) const override;
		QueueSyncPoint GetLastCompletedQueuePoint(QUEUE_TYPE queue) const override;
		UploadTicket EnqueueBufferUpload(const BufferUploadDesc& upload) override;
		UploadTicket EnqueueTextureUpload(const TextureUploadDesc& upload) override;
		bool IsUploadComplete(const UploadTicket& ticket) const override;
		void WaitUpload(const UploadTicket& ticket) const override;
		void OnDeviceRemoved();

		void WaitForGPU() const override;
		void ClearPipelineStateCache() override;
		size_t GetActivePipelineCount() const override { return hmlenu(pipelines_global); }

		ShaderFormat GetShaderFormat() const override
		{
#ifdef PLATFORM_XBOX
			return ShaderFormat::HLSL6_XS;
#else
			return ShaderFormat::HLSL6;
#endif // PLATFORM_XBOX
		}

		Texture GetBackBuffer(const SwapChain* swapchain) const override;

		ColorSpace GetSwapChainColorSpace(const SwapChain* swapchain) const override;
		bool IsSwapChainSupportsHDR(const SwapChain* swapchain) const override;

		uint32_t GetMinOffsetAlignment(const GPUBufferDesc* desc) const override
		{
			uint32_t alignment = std::max(1u, desc->alignment);
			if (has_flag(desc->bind_flags, BindFlag::BIND_CONSTANT_BUFFER))
			{
				alignment = std::max(alignment, (uint32_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
			}
			if (has_flag(desc->misc_flags, ResourceMiscFlag::BUFFER_RAW))
			{
				alignment = std::max(alignment, (uint32_t)D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT);
			}
			if (has_flag(desc->misc_flags, ResourceMiscFlag::BUFFER_STRUCTURED))
			{
				alignment = std::max(alignment, desc->stride);
			}
			if (desc->format != Format::UNKNOWN || has_flag(desc->misc_flags, ResourceMiscFlag::TYPED_FORMAT_CASTING))
			{
				alignment = std::max(alignment, 16u);
			}
			if (has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_BUFFER))
			{
				alignment = std::max(alignment, (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
			}
			if (has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS))
			{
				alignment = std::max(alignment, (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
			}
			if (has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_RT_DS))
			{
				alignment = std::max(alignment, (uint32_t)D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
			}
			return alignment;
		}

		MemoryUsage GetMemoryUsage() const override
		{
			MemoryUsage retval;
			D3D12MA::Budget budget;
			allocationhandler->allocator->GetBudget(&budget, nullptr);
			retval.budget = budget.BudgetBytes;
			retval.usage = budget.UsageBytes;
			return retval;
		}

		uint32_t GetMaxViewportCount() const override { return D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; };

		void SparseUpdate(QUEUE_TYPE queue, const SparseUpdateCommand* commands, uint32_t command_count) override;

		const char* GetTag() const override { return "[DX12]"; }

		///////////////Thread-sensitive////////////////////////

		void WaitCommandList(CommandList cmd, CommandList wait_for) override;
		void RenderPassBegin(const SwapChain* swapchain, CommandList cmd) override;
		void RenderPassBegin(const RenderPassImage* images, uint32_t image_count, CommandList cmd, RenderPassFlags flags = RenderPassFlags::RENDER_PASS_FLAG_NONE) override;
		void RenderPassEnd(CommandList cmd) override;
		void BindScissorRects(uint32_t numRects, const Rect* rects, CommandList cmd) override;
		void BindViewports(uint32_t NumViewports, const Viewport* pViewports, CommandList cmd) override;
		void BindResource(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource = -1) override;
		void BindResources(const GPUResource *const* resources, uint32_t slot, uint32_t count, CommandList cmd) override;
		void BindUAV(const GPUResource* resource, uint32_t slot, CommandList cmd, int subresource = -1) override;
		void BindUAVs(const GPUResource *const* resources, uint32_t slot, uint32_t count, CommandList cmd) override;
		void BindSampler(const Sampler* sampler, uint32_t slot, CommandList cmd) override;
		void BindConstantBuffer(const GPUBuffer* buffer, uint32_t slot, CommandList cmd, uint64_t offset = 0ull) override;
		void BindVertexBuffers(const GPUBuffer *const* vertexBuffers, uint32_t slot, uint32_t count, const uint32_t* strides, const uint64_t* offsets, CommandList cmd) override;
		void BindIndexBuffer(const GPUBuffer* indexBuffer, const IndexBufferFormat format, uint64_t offset, CommandList cmd) override;
		void BindStencilRef(uint32_t value, CommandList cmd) override;
		void BindBlendFactor(float r, float g, float b, float a, CommandList cmd) override;
		void BindShadingRate(ShadingRate rate, CommandList cmd) override;
		void BindPipelineState(const PipelineState* pso, CommandList cmd) override;
		void BindComputeShader(const Shader* cs, CommandList cmd) override;
		void BindDepthBounds(float min_bounds, float max_bounds, CommandList cmd) override;
		void Draw(uint32_t vertexCount, uint32_t startVertexLocation, CommandList cmd) override;
		void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation, CommandList cmd) override;
		void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation, CommandList cmd) override;
		void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation, CommandList cmd) override;
		void DrawInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd) override;
		void DrawIndexedInstancedIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd) override;
		void DrawInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd) override;
		void DrawIndexedInstancedIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd) override;
		void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ, CommandList cmd) override;
		void DispatchIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd) override;
		void DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ, CommandList cmd) override;
		void DispatchMeshIndirect(const GPUBuffer* args, uint64_t args_offset, CommandList cmd) override;
		void DispatchMeshIndirectCount(const GPUBuffer* args, uint64_t args_offset, const GPUBuffer* count, uint64_t count_offset, uint32_t max_count, CommandList cmd) override;
		void CopyResource(const GPUResource* pDst, const GPUResource* pSrc, CommandList cmd) override;
		void CopyBuffer(const GPUBuffer* pDst, uint64_t dst_offset, const GPUBuffer* pSrc, uint64_t src_offset, uint64_t size, CommandList cmd) override;
		void CopyTexture(const Texture* dst, uint32_t dstX, uint32_t dstY, uint32_t dstZ, uint32_t dstMip, uint32_t dstSlice, const Texture* src, uint32_t srcMip, uint32_t srcSlice, CommandList cmd, const Box* srcbox, ImageAspect dst_aspect, ImageAspect src_aspect) override;
		void QueryBegin(const GPUQueryHeap* heap, uint32_t index, CommandList cmd) override;
		void QueryEnd(const GPUQueryHeap* heap, uint32_t index, CommandList cmd) override;
		void QueryResolve(const GPUQueryHeap* heap, uint32_t index, uint32_t count, const GPUBuffer* dest, uint64_t dest_offset, CommandList cmd) override;
		void QueryReset(const GPUQueryHeap* heap, uint32_t index, uint32_t count, CommandList cmd) override {}
		void Barrier(const GPUBarrier* barriers, uint32_t numBarriers, CommandList cmd) override;
		void BuildRaytracingAccelerationStructure(const RaytracingAccelerationStructure* dst, CommandList cmd, const RaytracingAccelerationStructure* src = nullptr) override;
		void BindRaytracingPipelineState(const RaytracingPipelineState* rtpso, CommandList cmd) override;
		void DispatchRays(const DispatchRaysDesc* desc, CommandList cmd) override;
		void PushConstants(const void* data, uint32_t size, CommandList cmd, uint32_t offset = 0) override;
		void PredicationBegin(const GPUBuffer* buffer, uint64_t offset, PredicationOp op, CommandList cmd) override;
		void PredicationEnd(CommandList cmd) override;
		void ClearUAV(const GPUResource* resource, uint32_t value, CommandList cmd) override;
		void VideoDecode(const VideoDecoder* video_decoder, const VideoDecodeOperation* op, CommandList cmd) override;

		void EventBegin(const char* name, CommandList cmd) override;
		void EventEnd(CommandList cmd) override;
		void SetMarker(const char* name, CommandList cmd) override;

		RenderPassInfo GetRenderPassInfo(CommandList cmd) override
		{
			return GetCommandList(cmd).renderpass_info;
		}

		GPULinearAllocator& GetFrameAllocator(CommandList cmd) override
		{
			return GetCommandList(cmd).frame_allocators[GetBufferIndex()];
		}

		ID3D12Resource* GetTextureInternalResource(const Texture* texture);
		ID3D12CommandQueue* GetGraphicsCommandQueue();

		struct DescriptorHeapGPU
		{
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_GPU;
			D3D12_CPU_DESCRIPTOR_HANDLE start_cpu = {};
			D3D12_GPU_DESCRIPTOR_HANDLE start_gpu = {};

			// CPU status:
			std::atomic<uint64_t> allocationOffset{ 0 };

			// GPU status:
			Microsoft::WRL::ComPtr<ID3D12Fence> fence;
			uint64_t fenceValue = 0;
			uint64_t cached_completedValue = 0;

			void SignalGPU(ID3D12CommandQueue* queue)
			{
				// Descriptor heaps' progress is recorded by the GPU:
				fenceValue = allocationOffset.load();
				dx12_check(queue->Signal(fence.Get(), fenceValue));
				cached_completedValue = fence->GetCompletedValue();
			}
		};
		DescriptorHeapGPU descriptorheap_res;
		DescriptorHeapGPU descriptorheap_sam;

		struct AllocationHandler
		{
			Microsoft::WRL::ComPtr<D3D12MA::Allocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12Device> device;
			std::mutex destroylocker;
			Microsoft::WRL::ComPtr<ID3D12Fence> queue_timeline_fences[QUEUE_COUNT];
			uint64_t submitted_queue_values[QUEUE_COUNT] = {};

			Microsoft::WRL::ComPtr<D3D12MA::Pool> uma_pool;

			template<typename T>
			struct RetiredObject
			{
				T object = {};
				SubmissionToken retire_after = {};
			};
			template<typename T>
			using RetireList = std::deque<RetiredObject<T>>;

			struct DescriptorAllocator
			{
				GraphicsDevice_DX12* device = nullptr;
				std::mutex locker;
				D3D12_DESCRIPTOR_HEAP_DESC desc = {};
				ID3D12DescriptorHeap** heaps = nullptr;
				uint32_t descriptor_size = 0;
				D3D12_CPU_DESCRIPTOR_HANDLE* freelist = nullptr;

				void init(GraphicsDevice_DX12* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptorsPerBlock)
				{
					this->device = device;
					desc.Type = type;
					desc.NumDescriptors = numDescriptorsPerBlock;
					descriptor_size = device->device->GetDescriptorHandleIncrementSize(type);
				}
				void block_allocate()
				{
					arrput(heaps, nullptr);
					dx12_check(device->device->CreateDescriptorHeap(&desc, PPV_ARGS(heaps[arrlenu(heaps) - 1])));
					D3D12_CPU_DESCRIPTOR_HANDLE heap_start = heaps[arrlenu(heaps) - 1]->GetCPUDescriptorHandleForHeapStart();
					for (UINT i = 0; i < desc.NumDescriptors; ++i)
					{
						D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_start;
						handle.ptr += i * descriptor_size;
						arrput(freelist, handle);
					}
				}
				D3D12_CPU_DESCRIPTOR_HANDLE allocate()
				{
					locker.lock();
					if (freelist == nullptr || arrlenu(freelist) == 0)
					{
						block_allocate();
					}
					WI_DX12_ASSERT(freelist != nullptr && arrlenu(freelist) > 0);
					D3D12_CPU_DESCRIPTOR_HANDLE handle = dx12_internal::pop_back_stb_array(freelist);
					locker.unlock();
					return handle;
				}
				void free(D3D12_CPU_DESCRIPTOR_HANDLE index)
				{
					locker.lock();
					arrput(freelist, index);
					locker.unlock();
				}

				~DescriptorAllocator()
				{
					if (heaps != nullptr)
					{
						for (size_t i = 0; i < arrlenu(heaps); ++i)
						{
							if (heaps[i] != nullptr)
							{
								heaps[i]->Release();
							}
						}
					}
					dx12_internal::destroy_stb_array(heaps);
					dx12_internal::destroy_stb_array(freelist);
				}
			};
			DescriptorAllocator descriptors_res;
			DescriptorAllocator descriptors_sam;
			DescriptorAllocator descriptors_rtv;
			DescriptorAllocator descriptors_dsv;

			// stb_ds arrays: bindless free-lists are rebuilt explicitly and released with arrfree().
			int* free_bindless_res = nullptr;
			int* free_bindless_sam = nullptr;

			RetireList<Microsoft::WRL::ComPtr<D3D12MA::Allocation>> destroyer_allocations;
			RetireList<Microsoft::WRL::ComPtr<ID3D12Resource>> destroyer_resources;
			RetireList<Microsoft::WRL::ComPtr<ID3D12QueryHeap>> destroyer_queryheaps;
			RetireList<Microsoft::WRL::ComPtr<ID3D12PipelineState>> destroyer_pipelines;
			RetireList<Microsoft::WRL::ComPtr<ID3D12RootSignature>> destroyer_rootSignatures;
			RetireList<Microsoft::WRL::ComPtr<ID3D12StateObject>> destroyer_stateobjects;
			RetireList<Microsoft::WRL::ComPtr<ID3D12VideoDecoderHeap>> destroyer_video_decoder_heaps;
			RetireList<Microsoft::WRL::ComPtr<ID3D12VideoDecoder>> destroyer_video_decoders;
			RetireList<int> destroyer_bindless_res;
			RetireList<int> destroyer_bindless_sam;

			SubmissionToken CaptureRetireToken() const
			{
				SubmissionToken token = {};
				for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
				{
					if (submitted_queue_values[q] == 0)
						continue;
					token.Merge(QueueSyncPoint{ (QUEUE_TYPE)q, submitted_queue_values[q] });
				}
				return token;
			}

			bool IsSubmissionComplete(const SubmissionToken& token) const
			{
				for (uint32_t q = 0; q < QUEUE_COUNT; ++q)
				{
					if ((token.queue_mask & (1u << q)) == 0)
						continue;
					ID3D12Fence* fence = queue_timeline_fences[q].Get();
					if (fence == nullptr)
						continue;
					if (fence->GetCompletedValue() < token.values[q])
						return false;
				}
				return true;
			}

			template<typename T, typename U>
			void Retire(RetireList<T>& list, U&& object)
			{
				RetiredObject<T> retired = {};
				retired.object = std::forward<U>(object);
				retired.retire_after = CaptureRetireToken();
				list.push_back(std::move(retired));
			}

			~AllocationHandler()
			{
				Update(true); // destroy all remaining
				dx12_internal::destroy_stb_array(free_bindless_res);
				dx12_internal::destroy_stb_array(free_bindless_sam);
			}

			// Deferred destroy of resources that the GPU is already finished with:
			void Update(bool force_all = false)
			{
				std::scoped_lock lck(destroylocker);

				auto collect = [&](auto& list, auto&& on_collect) {
					size_t write_index = 0;
					for (size_t i = 0; i < list.size(); ++i)
					{
						auto& item = list[i];
						if (!force_all && !IsSubmissionComplete(item.retire_after))
						{
							if (write_index != i)
							{
								list[write_index] = std::move(item);
							}
							++write_index;
							continue;
						}
						on_collect(item.object);
					}
					list.resize(write_index);
				};

				collect(destroyer_allocations, [](Microsoft::WRL::ComPtr<D3D12MA::Allocation>&) {});
				collect(destroyer_resources, [](Microsoft::WRL::ComPtr<ID3D12Resource>&) {});
				collect(destroyer_queryheaps, [](Microsoft::WRL::ComPtr<ID3D12QueryHeap>&) {});
				collect(destroyer_pipelines, [](Microsoft::WRL::ComPtr<ID3D12PipelineState>&) {});
				collect(destroyer_rootSignatures, [](Microsoft::WRL::ComPtr<ID3D12RootSignature>&) {});
				collect(destroyer_stateobjects, [](Microsoft::WRL::ComPtr<ID3D12StateObject>&) {});
				collect(destroyer_video_decoder_heaps, [](Microsoft::WRL::ComPtr<ID3D12VideoDecoderHeap>&) {});
				collect(destroyer_video_decoders, [](Microsoft::WRL::ComPtr<ID3D12VideoDecoder>&) {});
				collect(destroyer_bindless_res, [&](int& index) { arrput(free_bindless_res, index); });
				collect(destroyer_bindless_sam, [&](int& index) { arrput(free_bindless_sam, index); });
			}
		};
		wi::allocator::shared_ptr<AllocationHandler> allocationhandler;

	};

}

#endif // WICKEDENGINE_BUILD_DX12
