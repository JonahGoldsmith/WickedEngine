#pragma once
#ifdef __APPLE__
#include "CommonInclude.h"
#include "wiPlatform.h"
#include "wiAllocator.h"
#include "wiGraphicsDevice.h"
#include "../stb_ds.h"

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#if __has_include(<SDL3/SDL_stdinc.h>)
#include <SDL3/SDL_stdinc.h>
#endif

#define IR_RUNTIME_METALCPP
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include <mutex>
#include <deque>

#if !defined(SDL_arraysize)
#define SDL_arraysize(array) (sizeof(array) / sizeof((array)[0]))
#endif

// There are crashes with this in graphics debugger, so this can be disabled:
//#define USE_TEXTURE_VIEW_POOL

namespace wi
{
	class GraphicsDevice_Metal final : public GraphicsDevice
	{
	public:
		struct RootLayout
		{
			uint32_t constants[PUSH_CONSTANT_COUNT];
			MTL::GPUAddress root_cbvs[ROOT_CBV_COUNT];
			MTL::GPUAddress resource_table_ptr;
			MTL::GPUAddress sampler_table_ptr;
		};
		struct ResourceTable
		{
			IRDescriptorTableEntry cbvs[SDL_arraysize(DescriptorBindingTable::CBV) - SDL_arraysize(RootLayout::root_cbvs)];
			IRDescriptorTableEntry srvs[SDL_arraysize(DescriptorBindingTable::SRV)];
			IRDescriptorTableEntry uavs[SDL_arraysize(DescriptorBindingTable::UAV)];
		};
		NS::SharedPtr<MTL::SamplerState> static_samplers[STATIC_SAMPLER_COUNT];
		struct StaticSamplerDescriptors
		{
			IRDescriptorTableEntry samplers[STATIC_SAMPLER_COUNT]; // workaround for static sampler, they are not supported by Metal Shader Converter
		} static_sampler_descriptors;
		struct SamplerTable
		{
			IRDescriptorTableEntry samplers[SDL_arraysize(DescriptorBindingTable::SAM)];
			StaticSamplerDescriptors static_samplers;
		};
		
		union ShaderAdditionalData
		{
			MTL::Size numthreads; // compute, mesh, amplification
			struct // vertex
			{
				uint32_t vertex_output_size_in_bytes;
				bool needs_draw_params;
			};
			uint32_t max_input_primitives_per_mesh_threadgroup; // geometry
		};
		
	private:
		NS::SharedPtr<MTL::Device> device;
		NS::SharedPtr<MTL4::CommandQueue> uploadqueue;
		
		struct Semaphore
		{
			MTL::Event* event = nullptr;
			uint64_t fenceValue = 0;
		};
		
		struct CommandQueue
		{
			NS::SharedPtr<MTL4::CommandQueue> queue;
			MTL4::CommandBuffer** submit_cmds = nullptr;
			
			void signal(const Semaphore& sema)
			{
				if (queue.get() == nullptr)
					return;
				queue->signalEvent(sema.event, sema.fenceValue);
			}
			void wait(const Semaphore& sema)
			{
				if (queue.get() == nullptr)
					return;
				queue->wait(sema.event, sema.fenceValue);
			}
			void submit()
			{
				if (queue.get() == nullptr)
					return;
				if (submit_cmds == nullptr || arrlenu(submit_cmds) == 0)
					return;
				queue->commit(submit_cmds, arrlenu(submit_cmds));
				arrsetlen(submit_cmds, 0);
			}
		} queues[QUEUE_COUNT];
		
		uint64_t frame_fence_values[BUFFERCOUNT] = {};
		NS::SharedPtr<MTL::SharedEvent> frame_fence[BUFFERCOUNT][QUEUE_COUNT];
		
		NS::SharedPtr<MTL4::ArgumentTableDescriptor> argument_table_desc;

		struct DrawCountICBEncoderState
		{
			bool initialized = false;
			bool failed = false;
			NS::SharedPtr<MTL::Library> library;
			NS::SharedPtr<MTL::Function> draw_function;
			NS::SharedPtr<MTL::Function> draw_indexed_function;
			NS::SharedPtr<MTL::Function> draw_mesh_function;
			NS::SharedPtr<MTL::ComputePipelineState> draw_pipeline;
			NS::SharedPtr<MTL::ComputePipelineState> draw_indexed_pipeline;
			NS::SharedPtr<MTL::ComputePipelineState> draw_mesh_pipeline;
			uint32_t draw_icb_argument_buffer_size = 0;
			uint32_t draw_indexed_icb_argument_buffer_size = 0;
			uint32_t draw_mesh_icb_argument_buffer_size = 0;
		} drawcount_icb_encoder;
		
		Semaphore* semaphore_pool = nullptr;
		std::mutex semaphore_pool_locker;
		Semaphore new_semaphore()
		{
			std::scoped_lock lck(semaphore_pool_locker);
			if (semaphore_pool == nullptr || arrlenu(semaphore_pool) == 0)
			{
				Semaphore sema = {};
				sema.event = device->newEvent();
				arrput(semaphore_pool, sema);
			}
			Semaphore sema = arrpop(semaphore_pool);
			sema.fenceValue++;
			return sema;
		}
		void free_semaphore(const Semaphore& sema)
		{
			std::scoped_lock lck(semaphore_pool_locker);
			arrput(semaphore_pool, sema);
		}
		
		struct JustInTimePSO
		{
			MTL::RenderPipelineState* pipeline = nullptr;
			MTL::DepthStencilState* depth_stencil_state = nullptr;
		};

		struct PipelineGlobalEntry
		{
			PipelineHash key = {};
			JustInTimePSO value = {};
		};
		
		struct CommandList_Metal
		{
			NS::SharedPtr<MTL4::CommandAllocator> commandallocators[BUFFERCOUNT];
			NS::SharedPtr<MTL4::CommandBuffer> commandbuffer;
			NS::SharedPtr<MTL4::ArgumentTable> argument_table;
			GPULinearAllocator frame_allocators[BUFFERCOUNT];
			RenderPassInfo renderpass_info;
			uint32_t id = 0;
			QUEUE_TYPE queue = QUEUE_COUNT;
			const PipelineState* active_pso = nullptr;
			bool dirty_pso = false;
			bool dirty_cs = false;
			const Shader* active_cs = nullptr;
			CA::MetalDrawable** presents = nullptr;
			NS::SharedPtr<MTL4::RenderCommandEncoder> render_encoder;
			NS::SharedPtr<MTL4::ComputeCommandEncoder> compute_encoder;
			MTL::PrimitiveType primitive_type = MTL::PrimitiveTypeTriangle;
			IRRuntimePrimitiveType ir_primitive_type = IRRuntimePrimitiveTypeTriangle;
			MTL4::BufferRange index_buffer = {};
			MTL::IndexType index_type = MTL::IndexTypeUInt32;
			PipelineGlobalEntry* pipelines_worker = nullptr;
			PipelineHash pipeline_hash;
			DescriptorBindingTable binding_table;
			bool dirty_root = false;
			bool dirty_resource = false;
			bool dirty_sampler = false;
			RootLayout root = {};
			uint32_t render_width = 0;
			uint32_t render_height = 0;
			bool dirty_scissor = false;
			uint32_t scissor_count = 0;
			MTL::ScissorRect scissors[16] = {};
			bool dirty_viewport = false;
			uint32_t viewport_count = 0;
			MTL::Viewport viewports[16] = {};
			Semaphore* waits = nullptr;
			Semaphore* signals = nullptr;
			bool drawargs_required = false;
			bool dirty_drawargs = false;
			MTL::Size numthreads_as = {};
			MTL::Size numthreads_ms = {};
			IRGeometryEmulationPipelineDescriptor gs_desc = {};
			struct TextureClearEntry
			{
				MTL::Texture* texture = nullptr;
				uint32_t value = 0;
			};
			TextureClearEntry* texture_clears = nullptr;
			
			struct VertexBufferBinding
			{
				MTL::GPUAddress gpu_address = 0;
				uint32_t stride = 0;
			};
			VertexBufferBinding vertex_buffers[16];
			bool dirty_vb = false;

			RenderPassImage* active_renderpass_images = nullptr;
			const GPUQueryHeap* active_renderpass_occlusionqueries = nullptr;
			const SwapChain* active_renderpass_swapchain = nullptr;
			bool active_renderpass_is_swapchain = false;
			bool active_renderpass_has_draws = false;

			struct DrawCountICBState
			{
				NS::SharedPtr<MTL::IndirectCommandBuffer> icb;
				NS::SharedPtr<MTL::Buffer> icb_argument_buffer;
				NS::SharedPtr<MTL::Buffer> icb_execution_range_buffer;
				uint32_t capacity = 0;
			};
			DrawCountICBState draw_count_icb;
			DrawCountICBState draw_indexed_count_icb;
			DrawCountICBState draw_mesh_count_icb;

			GPUBarrier* barriers = nullptr;

			void reset(uint32_t bufferindex)
			{
				frame_allocators[bufferindex].reset();
				renderpass_info = {};
				id = 0;
				queue = QUEUE_COUNT;
				active_pso = nullptr;
				active_cs = nullptr;
				dirty_pso = false;
				dirty_cs = false;
				arrsetlen(presents, 0);
				render_encoder.reset();
				compute_encoder.reset();
				primitive_type = MTL::PrimitiveTypeTriangle;
				ir_primitive_type = IRRuntimePrimitiveTypeTriangle;
				index_buffer = {};
				index_type = MTL::IndexTypeUInt32;
				arrsetlen(pipelines_worker, 0);
				pipeline_hash = {};
				binding_table = {};
				dirty_root = true;
				dirty_resource = true;
				dirty_sampler = true;
				root = {};
				for (auto& x : vertex_buffers)
				{
					x = {};
				}
				dirty_vb = false;
				arrsetlen(active_renderpass_images, 0);
				active_renderpass_occlusionqueries = nullptr;
				active_renderpass_swapchain = nullptr;
				active_renderpass_is_swapchain = false;
				active_renderpass_has_draws = false;
				render_width = 0;
				render_height = 0;
				dirty_viewport = false;
				dirty_scissor = false;
				scissor_count = 0;
				viewport_count = 0;
				for (auto& x : scissors)
				{
					x = {};
				}
				for (auto& x : viewports)
				{
					x = {};
				}
				SDL_assert(barriers == nullptr || arrlenu(barriers) == 0);
				SDL_assert(waits == nullptr || arrlenu(waits) == 0);
				SDL_assert(signals == nullptr || arrlenu(signals) == 0);
				drawargs_required = false;
				dirty_drawargs = false;
				numthreads_as = {};
				numthreads_ms = {};
			}
		};
		wi::allocator::BlockAllocator<CommandList_Metal, 64> cmd_allocator;
		CommandList_Metal** commandlists = nullptr;
		uint32_t cmd_count = 0;
		wi::SpinLock cmd_locker;
		
		PipelineGlobalEntry* pipelines_global = nullptr;
		
		NS::SharedPtr<MTL::Buffer> descriptor_heap_res;
		NS::SharedPtr<MTL::Buffer> descriptor_heap_sam;
		IRDescriptorTableEntry* descriptor_heap_res_data = nullptr;
		IRDescriptorTableEntry* descriptor_heap_sam_data = nullptr;
		
#ifdef USE_TEXTURE_VIEW_POOL
		NS::SharedPtr<MTL::TextureViewPool> texture_view_pool;
#endif // USE_TEXTURE_VIEW_POOL
		
		void binder_flush(CommandList cmd);
		void barrier_flush(CommandList cmd);
		void clear_flush(CommandList cmd);
		bool EnsureDrawCountICBEncoder();
		bool EnsureDrawCountICBResources(CommandList cmd, bool indexed, uint32_t max_count);
		bool EnsureMeshCountICBResources(CommandList cmd, uint32_t max_count);
		bool EndRenderPassForIndirectEncoding(CommandList cmd);
		bool ResumeRenderPassAfterIndirectEncoding(CommandList cmd, bool load_attachments);

		CommandList_Metal& GetCommandList(CommandList cmd) const
		{
			SDL_assert(wiGraphicsCommandListIsValid(cmd));
			return *(CommandList_Metal*)cmd.internal_state;
		}
		
		void pso_validate(CommandList cmd);
		void predraw(CommandList cmd);
		void predispatch(CommandList cmd);
		void precopy(CommandList cmd);

	public:
		GraphicsDevice_Metal(ValidationMode validationMode = ValidationMode::Disabled, GPUPreference preference = GPUPreference::Discrete);
		~GraphicsDevice_Metal() override;

		bool CreateSwapChain(const SwapChainDesc* desc, wi::platform::window_type window, SwapChain* swapchain) const override;
		bool CreateBuffer2(const GPUBufferDesc* desc, const std::function<void(void*)>& init_callback, GPUBuffer* buffer, const GPUResource* alias = nullptr, uint64_t alias_offset = 0ull) const override;
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
		void SetName(Shader* shader, const char* name) const override;

		CommandList BeginCommandList(QUEUE_TYPE queue = QUEUE_GRAPHICS) override;
		void SubmitCommandLists() override;

		void WaitForGPU() const override;
		void ClearPipelineStateCache() override;
		size_t GetActivePipelineCount() const override { return 0; }

		ShaderFormat GetShaderFormat() const override { return ShaderFormat::METAL; }

		Texture GetBackBuffer(const SwapChain* swapchain) const override;

		ColorSpace GetSwapChainColorSpace(const SwapChain* swapchain) const override;
		bool IsSwapChainSupportsHDR(const SwapChain* swapchain) const override;

		uint32_t GetMinOffsetAlignment(const GPUBufferDesc* desc) const override
		{
			uint32_t alignment = 256u;
			if (has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_BUFFER) || has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_NON_RT_DS) || has_flag(desc->misc_flags, ResourceMiscFlag::ALIASING_TEXTURE_RT_DS))
			{
				alignment = std::max(alignment, uint32_t(64 * 1024)); // 64KB safety to match DX12
			}
			return alignment;
		}

		MemoryUsage GetMemoryUsage() const override
		{
			MemoryUsage mem;
			mem.budget = device->recommendedMaxWorkingSetSize();
			mem.usage = device->currentAllocatedSize();
			return mem;
		}

		uint32_t GetMaxViewportCount() const override { return 16; };

		void SparseUpdate(QUEUE_TYPE queue, const SparseUpdateCommand* commands, uint32_t command_count) override;

		const char* GetTag() const override { return "[Metal]"; }

		///////////////Thread-sensitive////////////////////////

		void WaitCommandList(CommandList cmd, CommandList wait_for) override;
		void RenderPassBegin(const SwapChain* swapchain, CommandList cmd) override;
		void RenderPassBegin(const RenderPassImage* images, uint32_t image_count, CommandList cmd, RenderPassFlags flags = RenderPassFlags::RENDER_PASS_FLAG_NONE) override { RenderPassBegin(images, image_count, nullptr, cmd, flags); };
		void RenderPassBegin(const RenderPassImage* images, uint32_t image_count, const GPUQueryHeap* occlusionqueries, CommandList cmd, RenderPassFlags flags = RenderPassFlags::RENDER_PASS_FLAG_NONE) override;
		void RenderPassEnd(CommandList cmd) override;
		void BindScissorRects(uint32_t numRects, const Rect* rects, CommandList cmd) override;
		void BindViewports(uint32_t NumViewports, const Viewport *pViewports, CommandList cmd) override;
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
		void QueryReset(const GPUQueryHeap* heap, uint32_t index, uint32_t count, CommandList cmd) override {};
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

		struct AllocationHandler
		{
			std::mutex destroylocker;
			uint64_t framecount = 0;
			std::deque<std::pair<NS::SharedPtr<MTL::Resource>, uint64_t>> destroyer_resources;
			std::deque<std::pair<NS::SharedPtr<MTL::SamplerState>, uint64_t>> destroyer_samplers;
			std::deque<std::pair<NS::SharedPtr<MTL::Library>, uint64_t>> destroyer_libraries;
			std::deque<std::pair<NS::SharedPtr<MTL::Function>, uint64_t>> destroyer_functions;
			std::deque<std::pair<NS::SharedPtr<MTL::RenderPipelineState>, uint64_t>> destroyer_render_pipelines;
			std::deque<std::pair<NS::SharedPtr<MTL::ComputePipelineState>, uint64_t>> destroyer_compute_pipelines;
			std::deque<std::pair<NS::SharedPtr<MTL::DepthStencilState>, uint64_t>> destroyer_depth_stencil_states;
			std::deque<std::pair<NS::SharedPtr<MTL4::CounterHeap>, uint64_t>> destroyer_counter_heaps;
			std::deque<std::pair<NS::SharedPtr<CA::MetalDrawable>, uint64_t>> destroyer_drawables;
			std::deque<std::pair<int, uint64_t>> destroyer_bindless_res;
			std::deque<std::pair<int, uint64_t>> destroyer_bindless_sam;
			int* free_bindless_res = nullptr;
			int* free_bindless_sam = nullptr;

			void Update(uint64_t FRAMECOUNT, uint32_t BUFFERCOUNT)
			{
				std::scoped_lock lck(destroylocker);
				framecount = FRAMECOUNT;
				while (!destroyer_resources.empty() && destroyer_resources.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					remove_resident(destroyer_resources.front().first.get());
					destroyer_resources.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_samplers.empty() && destroyer_samplers.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_samplers.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_libraries.empty() && destroyer_libraries.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_libraries.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_functions.empty() && destroyer_functions.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_functions.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_render_pipelines.empty() && destroyer_render_pipelines.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_render_pipelines.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_compute_pipelines.empty() && destroyer_compute_pipelines.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_compute_pipelines.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_depth_stencil_states.empty() && destroyer_depth_stencil_states.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_depth_stencil_states.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_counter_heaps.empty() && destroyer_counter_heaps.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_counter_heaps.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_drawables.empty() && destroyer_drawables.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					destroyer_drawables.pop_front();
					// SharedPtr auto delete
				}
				while (!destroyer_bindless_res.empty() && destroyer_bindless_res.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					arrput(free_bindless_res, destroyer_bindless_res.front().first);
					destroyer_bindless_res.pop_front();
				}
				while (!destroyer_bindless_sam.empty() && destroyer_bindless_sam.front().second + BUFFERCOUNT < FRAMECOUNT)
				{
					arrput(free_bindless_sam, destroyer_bindless_sam.front().first);
					destroyer_bindless_sam.pop_front();
				}
			}
			
			int allocate_resource_index()
			{
				std::scoped_lock lck(destroylocker);
				if (free_bindless_res == nullptr || arrlenu(free_bindless_res) == 0)
					return -1;
				int index = arrpop(free_bindless_res);
				return index;
			}
			int allocate_sampler_index()
			{
				std::scoped_lock lck(destroylocker);
				if (free_bindless_sam == nullptr || arrlenu(free_bindless_sam) == 0)
					return -1;
				int index = arrpop(free_bindless_sam);
				return index;
			}
			
			NS::SharedPtr<MTL::ResidencySet> residency_set;
			void make_resident(const MTL::Resource* allocation)
			{
				if (allocation == nullptr)
					return;
				std::scoped_lock locker(destroylocker);
				residency_set->addAllocation(allocation);
			}
			void remove_resident(const MTL::Allocation* allocation)
			{
				if (allocation == nullptr)
					return;
				residency_set->removeAllocation(allocation);
			}
		};
		wi::allocator::shared_ptr<AllocationHandler> allocationhandler;
	};
}
#endif // __APPLE__
