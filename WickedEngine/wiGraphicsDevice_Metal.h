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
		
		struct CommandQueue
		{
			NS::SharedPtr<MTL4::CommandQueue> queue;
			MTL4::CommandBuffer** submit_cmds = nullptr;
			
			void signal(MTL::SharedEvent* event, uint64_t value)
			{
				if (queue.get() == nullptr)
					return;
				if (event == nullptr)
					return;
				queue->signalEvent(event, value);
			}
			void wait(MTL::SharedEvent* event, uint64_t value)
			{
				if (queue.get() == nullptr)
					return;
				if (event == nullptr)
					return;
				queue->wait(event, value);
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
		mutable std::mutex submission_token_locker;
		mutable uint64_t submission_token_values[QUEUE_COUNT] = {};
		NS::SharedPtr<MTL::SharedEvent> submission_token_events[QUEUE_COUNT];
		mutable std::mutex submission_stats_mutex;
		QueueSubmissionStats last_submission_stats = {};
		struct UploadJob
		{
			UploadTicket ticket = {};
			NS::SharedPtr<MTL::Buffer> staging_buffer;
			NS::SharedPtr<MTL4::CommandBuffer> commandbuffer;
			NS::SharedPtr<MTL4::CommandAllocator> commandallocator;
		};
		mutable std::mutex upload_locker;
		mutable std::deque<UploadJob> inflight_uploads;
		mutable SubmissionToken pending_implicit_uploads = {};
		
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
			uint32_t* wait_for_cmd_ids = nullptr;
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
				SDL_assert(wait_for_cmd_ids == nullptr || arrlenu(wait_for_cmd_ids) == 0);
				drawargs_required = false;
				dirty_drawargs = false;
				numthreads_as = {};
				numthreads_ms = {};
			}
		};
		wi::allocator::BlockAllocator<CommandList_Metal, 64> cmd_allocator;
		CommandList_Metal** commandlists = nullptr;
		CommandList_Metal** open_commandlists = nullptr;
		struct RetiredCommandContext
		{
			CommandList_Metal* context = nullptr;
			QueueSyncPoint retire_after = {};
		};
		RetiredCommandContext* retired_contexts[QUEUE_COUNT] = {};
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
		SubmissionToken allocate_submission_token(QUEUE_TYPE queue) const;
		void retire_completed_uploads() const;
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
		};
		SubmissionToken consume_pending_implicit_uploads() const;
		SubmissionToken SubmitCommandListsInternal(const SubmitDesc& desc);
		UploadTicket UploadAsyncInternal(const UploadDescInternal& upload, bool implicit_dependency) const;

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
		SubmissionToken SubmitCommandListsEx(const SubmitDesc& desc) override;
		bool IsQueuePointComplete(QueueSyncPoint point) const override;
		void WaitQueuePoint(QueueSyncPoint point) const override;
		QueueSyncPoint GetLastSubmittedQueuePoint(QUEUE_TYPE queue) const override;
		QueueSyncPoint GetLastCompletedQueuePoint(QUEUE_TYPE queue) const override;
		UploadTicket EnqueueBufferUpload(const BufferUploadDesc& upload) override;
		UploadTicket EnqueueTextureUpload(const TextureUploadDesc& upload) override;
		bool IsUploadComplete(const UploadTicket& ticket) const override;
		void WaitUpload(const UploadTicket& ticket) const override;
		bool GetQueueSubmissionStats(QueueSubmissionStats& out) const override;

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
			template<typename T>
			struct RetiredObject
			{
				T object = {};
				SubmissionToken retire_after = {};
			};
			template<typename T>
			using RetireList = std::deque<RetiredObject<T>>;

			std::mutex destroylocker;
			RetireList<NS::SharedPtr<MTL::Resource>> destroyer_resources;
			RetireList<NS::SharedPtr<MTL::SamplerState>> destroyer_samplers;
			RetireList<NS::SharedPtr<MTL::Library>> destroyer_libraries;
			RetireList<NS::SharedPtr<MTL::Function>> destroyer_functions;
			RetireList<NS::SharedPtr<MTL::RenderPipelineState>> destroyer_render_pipelines;
			RetireList<NS::SharedPtr<MTL::ComputePipelineState>> destroyer_compute_pipelines;
			RetireList<NS::SharedPtr<MTL::DepthStencilState>> destroyer_depth_stencil_states;
			RetireList<NS::SharedPtr<MTL4::CounterHeap>> destroyer_counter_heaps;
			RetireList<NS::SharedPtr<CA::MetalDrawable>> destroyer_drawables;
			RetireList<int> destroyer_bindless_res;
			RetireList<int> destroyer_bindless_sam;
			int* free_bindless_res = nullptr;
			int* free_bindless_sam = nullptr;
			NS::SharedPtr<MTL::SharedEvent> queue_timeline_events[QUEUE_COUNT];
			uint64_t submitted_queue_values[QUEUE_COUNT] = {};

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
					if (queue_timeline_events[q].get() == nullptr)
						continue;
					if (queue_timeline_events[q]->signaledValue() < token.values[q])
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

			void Update()
			{
				std::scoped_lock lck(destroylocker);

				auto collect = [this](auto& list, auto&& on_collect) {
					for (auto it = list.begin(); it != list.end();)
					{
						if (!IsSubmissionComplete(it->retire_after))
						{
							++it;
							continue;
						}
						on_collect(it->object);
						it = list.erase(it);
					}
				};

				collect(destroyer_resources, [this](NS::SharedPtr<MTL::Resource>& resource) {
					remove_resident(resource.get());
				});
				collect(destroyer_samplers, [](NS::SharedPtr<MTL::SamplerState>&) {});
				collect(destroyer_libraries, [](NS::SharedPtr<MTL::Library>&) {});
				collect(destroyer_functions, [](NS::SharedPtr<MTL::Function>&) {});
				collect(destroyer_render_pipelines, [](NS::SharedPtr<MTL::RenderPipelineState>&) {});
				collect(destroyer_compute_pipelines, [](NS::SharedPtr<MTL::ComputePipelineState>&) {});
				collect(destroyer_depth_stencil_states, [](NS::SharedPtr<MTL::DepthStencilState>&) {});
				collect(destroyer_counter_heaps, [](NS::SharedPtr<MTL4::CounterHeap>&) {});
				collect(destroyer_drawables, [](NS::SharedPtr<CA::MetalDrawable>&) {});
				collect(destroyer_bindless_res, [this](int& index) { arrput(free_bindless_res, index); });
				collect(destroyer_bindless_sam, [this](int& index) { arrput(free_bindless_sam, index); });
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
