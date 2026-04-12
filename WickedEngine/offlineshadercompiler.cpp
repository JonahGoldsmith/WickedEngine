#include "WickedEngine.h"

#include <iostream>
#include <iomanip>
#include <mutex>
#include <string>
#include <cstdlib>

class nullbuf_t : public std::streambuf
{
protected:
	virtual int_type overflow(int_type ch) override
	{
		return traits_type::not_eof(ch);
	}
} nullbuf;

std::mutex locker;
struct ShaderEntry
{
	std::string name;
	wi::ShaderStage stage = wi::ShaderStage::Count;
	wi::ShaderModel minshadermodel = wi::ShaderModel::SM_5_0;
	struct Permutation
	{
		wi::vector<std::string> defines;
		Permutation() = default;
		Permutation(std::initializer_list<std::string> init)
		{
			for (auto& x : init)
			{
				defines.push_back(x);
			}
		}
	};
	wi::vector<Permutation> permutations;
	std::string entrypoint = "main";
};
wi::vector<ShaderEntry> shaders = {
	{"hairparticle_simulateCS", wi::ShaderStage::CS},
	{"emittedparticle_simulateCS", wi::ShaderStage::CS},
	{"generateMIPChainCubeCS_float4", wi::ShaderStage::CS},
	{"generateMIPChainCubeArrayCS_float4", wi::ShaderStage::CS},
	{"generateMIPChain3DCS_float4", wi::ShaderStage::CS},
	{"generateMIPChain2DCS_float4", wi::ShaderStage::CS},
	{"blockcompressCS_BC1", wi::ShaderStage::CS},
	{"blockcompressCS_BC3", wi::ShaderStage::CS},
	{"blockcompressCS_BC4", wi::ShaderStage::CS},
	{"blockcompressCS_BC5", wi::ShaderStage::CS},
	{"blockcompressCS_BC6H", wi::ShaderStage::CS},
	{"blockcompressCS_BC6H_cubemap", wi::ShaderStage::CS},
	{"blur_gaussian_float4CS", wi::ShaderStage::CS},
	{"bloomseparateCS", wi::ShaderStage::CS},
	{"depthoffield_mainCS", wi::ShaderStage::CS},
	{"depthoffield_neighborhoodMaxCOCCS", wi::ShaderStage::CS},
	{"depthoffield_prepassCS", wi::ShaderStage::CS},
	{"depthoffield_upsampleCS", wi::ShaderStage::CS},
	{"depthoffield_tileMaxCOC_verticalCS", wi::ShaderStage::CS},
	{"depthoffield_tileMaxCOC_horizontalCS", wi::ShaderStage::CS},
	{"vxgi_offsetprevCS", wi::ShaderStage::CS},
	{"vxgi_temporalCS", wi::ShaderStage::CS},
	{"vxgi_sdf_jumpfloodCS", wi::ShaderStage::CS},
	{"vxgi_resolve_diffuseCS", wi::ShaderStage::CS},
	{"vxgi_resolve_specularCS", wi::ShaderStage::CS},
	{"upsample_bilateral_float1CS", wi::ShaderStage::CS},
	{"upsample_bilateral_float4CS", wi::ShaderStage::CS},
	{"temporalaaCS", wi::ShaderStage::CS},
	{"tonemapCS", wi::ShaderStage::CS},
	{"underwaterCS", wi::ShaderStage::CS},
	{"mesh_blend_prepareCS", wi::ShaderStage::CS},
	{"mesh_blend_expandCS", wi::ShaderStage::CS},
	{"mesh_blendPS", wi::ShaderStage::PS},
	{"fsr_upscalingCS", wi::ShaderStage::CS},
	{"fsr_sharpenCS", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_autogen_reactive_pass", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_compute_luminance_pyramid_pass", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_prepare_input_color_pass", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_reconstruct_previous_depth_pass", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_depth_clip_pass", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_lock_pass", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_accumulate_pass", wi::ShaderStage::CS},
	{"ffx-fsr2/ffx_fsr2_rcas_pass", wi::ShaderStage::CS},
	{"ssaoCS", wi::ShaderStage::CS},
	{"ssgi_deinterleaveCS", wi::ShaderStage::CS},
	{"ssgiCS", wi::ShaderStage::CS},
	{"ssgi_upsampleCS", wi::ShaderStage::CS},
	{"rtdiffuseCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_5},
	{"rtdiffuse_spatialCS", wi::ShaderStage::CS},
	{"rtdiffuse_temporalCS", wi::ShaderStage::CS},
	{"rtdiffuse_upsampleCS", wi::ShaderStage::CS},
	{"rtreflectionCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_5},
	{"ssr_tileMaxRoughness_horizontalCS", wi::ShaderStage::CS},
	{"ssr_tileMaxRoughness_verticalCS", wi::ShaderStage::CS},
	{"ssr_depthHierarchyCS", wi::ShaderStage::CS},
	{"ssr_resolveCS", wi::ShaderStage::CS},
	{"ssr_temporalCS", wi::ShaderStage::CS},
	{"ssr_upsampleCS", wi::ShaderStage::CS},
	{"ssr_raytraceCS", wi::ShaderStage::CS},
	{"ssr_raytraceCS_cheap", wi::ShaderStage::CS},
	{"ssr_raytraceCS_earlyexit", wi::ShaderStage::CS},
	{"sharpenCS", wi::ShaderStage::CS},
	{"crt_screenCS", wi::ShaderStage::CS},
	{"skinningCS", wi::ShaderStage::CS},
	{"resolveMSAADepthStencilCS", wi::ShaderStage::CS},
	{"raytraceCS", wi::ShaderStage::CS},
	{"raytraceCS_rtapi", wi::ShaderStage::CS, wi::ShaderModel::SM_6_5},
	{"paint_textureCS", wi::ShaderStage::CS},
	{"oceanUpdateDisplacementMapCS", wi::ShaderStage::CS},
	{"oceanUpdateGradientFoldingCS", wi::ShaderStage::CS},
	{"oceanSimulatorCS", wi::ShaderStage::CS},
	{"msao_interleaveCS", wi::ShaderStage::CS},
	{"msao_preparedepthbuffers1CS", wi::ShaderStage::CS},
	{"msao_preparedepthbuffers2CS", wi::ShaderStage::CS},
	{"msao_blurupsampleCS", wi::ShaderStage::CS},
	{"msao_blurupsampleCS_blendout", wi::ShaderStage::CS},
	{"msao_blurupsampleCS_premin", wi::ShaderStage::CS},
	{"msao_blurupsampleCS_premin_blendout", wi::ShaderStage::CS},
	{"msaoCS", wi::ShaderStage::CS},
	{"motionblur_neighborhoodMaxVelocityCS", wi::ShaderStage::CS},
	{"motionblur_tileMaxVelocity_horizontalCS", wi::ShaderStage::CS},
	{"motionblur_tileMaxVelocity_verticalCS", wi::ShaderStage::CS},
	{"luminancePass2CS", wi::ShaderStage::CS},
	{"motionblurCS", wi::ShaderStage::CS},
	{"motionblurCS_cheap", wi::ShaderStage::CS},
	{"motionblurCS_earlyexit", wi::ShaderStage::CS},
	{"luminancePass1CS", wi::ShaderStage::CS},
	{"lightShaftsCS", wi::ShaderStage::CS},
	{"lightCullingCS_ADVANCED_DEBUG", wi::ShaderStage::CS},
	{"lightCullingCS_DEBUG", wi::ShaderStage::CS},
	{"lightCullingCS", wi::ShaderStage::CS},
	{"lightCullingCS_ADVANCED", wi::ShaderStage::CS},
	{"hbaoCS", wi::ShaderStage::CS},
	{"radix_sortCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_0, {{"FPS_COUNT"}}, "FPS_Count"},
	{"radix_sortCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_0, {{"FPS_COUNT_REDUCE"}}, "FPS_CountReduce"},
	{"radix_sortCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_0, {{"FPS_SCAN"}}, "FPS_Scan"},
	{"radix_sortCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_0, {{"FPS_SCAN_ADD"}}, "FPS_ScanAdd"},
	{"radix_sortCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_0, {{"FPS_SCATTER"}}, "FPS_Scatter"},
	{"radix_sortCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_0, {{"FPS_SCATTER", "kRS_ValueCopy"}}, "FPS_Scatter"},
	{"radix_sortCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_0, {{"FPS_INDIRECT"}}, "FPS_SetupIndirectParameters"},
	{"fxaaCS", wi::ShaderStage::CS},
	{"filterEnvMapCS", wi::ShaderStage::CS},
	{"fft_512x512_c2c_CS", wi::ShaderStage::CS},
	{"fft_512x512_c2c_v2_CS", wi::ShaderStage::CS},
	{"emittedparticle_sphpartitionCS", wi::ShaderStage::CS},
	{"emittedparticle_sphcellallocationCS", wi::ShaderStage::CS},
	{"emittedparticle_sphbinningCS", wi::ShaderStage::CS},
	{"emittedparticle_simulateCS_SORTING", wi::ShaderStage::CS},
	{"emittedparticle_simulateCS_SORTING_DEPTHCOLLISIONS", wi::ShaderStage::CS},
	{"emittedparticle_sphdensityCS", wi::ShaderStage::CS},
	{"emittedparticle_sphforceCS", wi::ShaderStage::CS},
	{"emittedparticle_kickoffUpdateCS", wi::ShaderStage::CS},
	{"emittedparticle_simulateCS_DEPTHCOLLISIONS", wi::ShaderStage::CS},
	{"emittedparticle_emitCS", wi::ShaderStage::CS},
	{"emittedparticle_emitCS_FROMMESH", wi::ShaderStage::CS},
	{"emittedparticle_emitCS_volume", wi::ShaderStage::CS},
	{"emittedparticle_finishUpdateCS", wi::ShaderStage::CS},
	{"downsample4xCS", wi::ShaderStage::CS},
	{"lineardepthCS", wi::ShaderStage::CS},
	{"depthoffield_prepassCS_earlyexit", wi::ShaderStage::CS},
	{"depthoffield_mainCS_cheap", wi::ShaderStage::CS},
	{"depthoffield_mainCS_earlyexit", wi::ShaderStage::CS },
	{"depthoffield_postfilterCS", wi::ShaderStage::CS },
	{"copytexture2D_float4_borderexpandCS", wi::ShaderStage::CS },
	{"copytexture2D_float4CS", wi::ShaderStage::CS },
	{"chromatic_aberrationCS", wi::ShaderStage::CS },
	{"bvh_hierarchyCS", wi::ShaderStage::CS },
	{"bvh_primitivesCS", wi::ShaderStage::CS },
	{"bvh_propagateaabbCS", wi::ShaderStage::CS },
	{"blur_gaussian_wide_float1CS", wi::ShaderStage::CS },
	{"blur_gaussian_wide_float4CS", wi::ShaderStage::CS },
	{"blur_gaussian_float1CS", wi::ShaderStage::CS },
	{"blur_bilateral_wide_float1CS", wi::ShaderStage::CS },
	{"blur_bilateral_wide_float4CS", wi::ShaderStage::CS },
	{"blur_bilateral_float1CS", wi::ShaderStage::CS },
	{"blur_bilateral_float4CS", wi::ShaderStage::CS },
	{"normalsfromdepthCS", wi::ShaderStage::CS },
	{"volumetricCloud_curlnoiseCS", wi::ShaderStage::CS },
	{"volumetricCloud_detailnoiseCS", wi::ShaderStage::CS },
	{"volumetricCloud_renderCS", wi::ShaderStage::CS },
	{"volumetricCloud_renderCS_capture", wi::ShaderStage::CS },
	{"volumetricCloud_reprojectCS", wi::ShaderStage::CS },
	{"volumetricCloud_shadow_renderCS", wi::ShaderStage::CS },
	{"volumetricCloud_shapenoiseCS", wi::ShaderStage::CS },
	{"volumetricCloud_upsamplePS", wi::ShaderStage::PS },
	{"volumetricCloud_weathermapCS", wi::ShaderStage::CS },
	{"shadingRateClassificationCS", wi::ShaderStage::CS },
	{"shadingRateClassificationCS_DEBUG", wi::ShaderStage::CS },
	{"aerialPerspectiveCS", wi::ShaderStage::CS },
	{"skyAtmosphere_cameraVolumeLutCS", wi::ShaderStage::CS },
	{"skyAtmosphere_transmittanceLutCS", wi::ShaderStage::CS },
	{"skyAtmosphere_skyViewLutCS", wi::ShaderStage::CS },
	{"skyAtmosphere_multiScatteredLuminanceLutCS", wi::ShaderStage::CS },
	{"skyAtmosphere_skyLuminanceLutCS", wi::ShaderStage::CS },
	{"screenspaceshadowCS", wi::ShaderStage::CS },
	{"rtshadowCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_5 },
	{"rtshadow_denoise_tileclassificationCS", wi::ShaderStage::CS },
	{"rtshadow_denoise_filterCS", wi::ShaderStage::CS },
	{"rtshadow_denoise_temporalCS", wi::ShaderStage::CS },
	{"rtshadow_upsampleCS", wi::ShaderStage::CS },
	{"rtaoCS", wi::ShaderStage::CS, wi::ShaderModel::SM_6_5 },
	{"rtao_denoise_tileclassificationCS", wi::ShaderStage::CS },
	{"rtao_denoise_filterCS", wi::ShaderStage::CS },
	{"visibility_resolveCS", wi::ShaderStage::CS },
	{"visibility_resolveCS_MSAA", wi::ShaderStage::CS },
	{"visibility_velocityCS", wi::ShaderStage::CS },
	{"visibility_skyCS", wi::ShaderStage::CS },
	{"surfel_coverageCS", wi::ShaderStage::CS },
	{"surfel_indirectprepareCS", wi::ShaderStage::CS },
	{"surfel_updateCS", wi::ShaderStage::CS },
	{"surfel_gridoffsetsCS", wi::ShaderStage::CS },
	{"surfel_binningCS", wi::ShaderStage::CS },
	{"surfel_raytraceCS_rtapi", wi::ShaderStage::CS, wi::ShaderModel::SM_6_5 },
	{"surfel_raytraceCS", wi::ShaderStage::CS },
	{"surfel_integrateCS", wi::ShaderStage::CS },
	{"ddgi_rayallocationCS", wi::ShaderStage::CS },
	{"ddgi_indirectprepareCS", wi::ShaderStage::CS },
	{"ddgi_raytraceCS", wi::ShaderStage::CS },
	{"ddgi_raytraceCS_rtapi", wi::ShaderStage::CS, wi::ShaderModel::SM_6_5 },
	{"ddgi_updateCS", wi::ShaderStage::CS },
	{"ddgi_updateCS_depth", wi::ShaderStage::CS },
	{"terrainVirtualTextureUpdateCS", wi::ShaderStage::CS },
	{"terrainVirtualTextureUpdateCS_normalmap", wi::ShaderStage::CS },
	{"terrainVirtualTextureUpdateCS_surfacemap", wi::ShaderStage::CS },
	{"terrainVirtualTextureUpdateCS_emissivemap", wi::ShaderStage::CS },
	{"meshlet_prepareCS", wi::ShaderStage::CS },
	{"impostor_prepareCS", wi::ShaderStage::CS },
	{"virtualTextureTileRequestsCS", wi::ShaderStage::CS },
	{"virtualTextureTileAllocateCS", wi::ShaderStage::CS },
	{"virtualTextureResidencyUpdateCS", wi::ShaderStage::CS },
	{"windCS", wi::ShaderStage::CS },
	{"yuv_to_rgbCS", wi::ShaderStage::CS },
	{"wetmap_updateCS", wi::ShaderStage::CS },
	{"causticsCS", wi::ShaderStage::CS },
	{"depth_reprojectCS", wi::ShaderStage::CS },
	{"depth_pyramidCS", wi::ShaderStage::CS },
	{"lightmap_expandCS", wi::ShaderStage::CS },
	{"gaussian_splatCS", wi::ShaderStage::CS },
	{"gaussian_splat_indirectCS", wi::ShaderStage::CS },


	{"imagePS", wi::ShaderStage::PS },
	{"emittedparticlePS_soft", wi::ShaderStage::PS },
	{"emittedparticlePS_shadow", wi::ShaderStage::PS },
	{"emittedparticlePS_soft_lighting", wi::ShaderStage::PS },
	{"oceanSurfacePS", wi::ShaderStage::PS },
	{"oceanSurfacePS_envmap", wi::ShaderStage::PS },
	{"hairparticlePS", wi::ShaderStage::PS },
	{"hairparticlePS_simple", wi::ShaderStage::PS },
	{"hairparticlePS_prepass", wi::ShaderStage::PS },
	{"hairparticlePS_prepass_depthonly", wi::ShaderStage::PS },
	{"hairparticlePS_shadow", wi::ShaderStage::PS },
	{"volumetricLight_SpotPS", wi::ShaderStage::PS },
	{"volumetricLight_PointPS", wi::ShaderStage::PS },
	{"volumetricLight_DirectionalPS", wi::ShaderStage::PS },
	{"volumetriclight_rectanglePS", wi::ShaderStage::PS },
	{"voxelPS", wi::ShaderStage::PS },
	{"vertexcolorPS", wi::ShaderStage::PS },
	{"upsample_bilateralPS", wi::ShaderStage::PS },
	{"sunPS", wi::ShaderStage::PS },
	{"skyPS_dynamic", wi::ShaderStage::PS },
	{"skyPS_static", wi::ShaderStage::PS },
	{"shadowPS_transparent", wi::ShaderStage::PS },
	{"shadowPS_water", wi::ShaderStage::PS },
	{"shadowPS_alphatest", wi::ShaderStage::PS },
	{"paintdecalPS", wi::ShaderStage::PS },
	{"renderlightmapPS", wi::ShaderStage::PS },
	{"renderlightmapPS_rtapi", wi::ShaderStage::PS, wi::ShaderModel::SM_6_5 },
	{"raytrace_debugbvhPS", wi::ShaderStage::PS },
	{"outlinePS", wi::ShaderStage::PS },
	{"oceanSurfaceSimplePS", wi::ShaderStage::PS },
	{"objectPS_voxelizer", wi::ShaderStage::PS },
	{"objectPS_hologram", wi::ShaderStage::PS },
	{"objectPS_paintradius", wi::ShaderStage::PS },
	{"objectPS_simple", wi::ShaderStage::PS },
	{"objectPS_debug", wi::ShaderStage::PS },
	{"objectPS_prepass", wi::ShaderStage::PS },
	{"objectPS_prepass_alphatest", wi::ShaderStage::PS },
	{"objectPS_prepass_depthonly_alphatest", wi::ShaderStage::PS },
	{"lightVisualizerPS", wi::ShaderStage::PS },
	{"vRectLightPS", wi::ShaderStage::PS },
	{"lensFlarePS", wi::ShaderStage::PS },
	{"impostorPS", wi::ShaderStage::PS },
	{"impostorPS_simple", wi::ShaderStage::PS },
	{"impostorPS_prepass", wi::ShaderStage::PS },
	{"impostorPS_prepass_depthonly", wi::ShaderStage::PS },
	{"forceFieldVisualizerPS", wi::ShaderStage::PS },
	{"fontPS", wi::ShaderStage::PS },
	{"envMap_skyPS_static", wi::ShaderStage::PS },
	{"envMap_skyPS_dynamic", wi::ShaderStage::PS },
	{"envMapPS", wi::ShaderStage::PS },
	{"emittedparticlePS_soft_distortion", wi::ShaderStage::PS },
	{"downsampleDepthBuffer4xPS", wi::ShaderStage::PS },
	{"emittedparticlePS_simple", wi::ShaderStage::PS },
	{"cubeMapPS", wi::ShaderStage::PS },
	{"circlePS", wi::ShaderStage::PS },
	{"captureImpostorPS", wi::ShaderStage::PS },
	{"ddgi_debugPS", wi::ShaderStage::PS },
	{"copyDepthPS", wi::ShaderStage::PS },
	{"copyStencilBitPS", wi::ShaderStage::PS },
	{"extractStencilBitPS", wi::ShaderStage::PS },
	{"trailPS", wi::ShaderStage::PS },
	{"waveeffectPS", wi::ShaderStage::PS },
	{"gaussian_splatPS", wi::ShaderStage::PS },


	{"hairparticleVS", wi::ShaderStage::VS },
	{"emittedparticleVS", wi::ShaderStage::VS },
	{"emittedparticleVS_shadow", wi::ShaderStage::VS },
	{"imageVS", wi::ShaderStage::VS },
	{"fontVS", wi::ShaderStage::VS },
	{"voxelVS", wi::ShaderStage::VS },
	{"vertexcolorVS", wi::ShaderStage::VS },
	{"volumetriclight_directionalVS", wi::ShaderStage::VS },
	{"volumetriclight_pointVS", wi::ShaderStage::VS },
	{"volumetriclight_spotVS", wi::ShaderStage::VS },
	{"volumetriclight_rectangleVS", wi::ShaderStage::VS },
	{"vSpotLightVS", wi::ShaderStage::VS },
	{"vPointLightVS", wi::ShaderStage::VS },
	{"vRectLightVS", wi::ShaderStage::VS },
	{"sphereVS", wi::ShaderStage::VS },
	{"skyVS", wi::ShaderStage::VS },
	{"postprocessVS", wi::ShaderStage::VS },
	{"paintdecalVS", wi::ShaderStage::VS },
	{"renderlightmapVS", wi::ShaderStage::VS },
	{"raytrace_screenVS", wi::ShaderStage::VS },
	{"oceanSurfaceVS", wi::ShaderStage::VS },
	{"objectVS_debug", wi::ShaderStage::VS },
	{"objectVS_voxelizer", wi::ShaderStage::VS },
	{"lensFlareVS", wi::ShaderStage::VS },
	{"impostorVS", wi::ShaderStage::VS },
	{"forceFieldPointVisualizerVS", wi::ShaderStage::VS },
	{"forceFieldPlaneVisualizerVS", wi::ShaderStage::VS },
	{"envMap_skyVS", wi::ShaderStage::VS },
	{"envMapVS", wi::ShaderStage::VS },
	{"occludeeVS", wi::ShaderStage::VS },
	{"ddgi_debugVS", wi::ShaderStage::VS },
	{"voxelGS", wi::ShaderStage::GS },
	{"objectGS_voxelizer", wi::ShaderStage::GS },
	{"objectVS_simple", wi::ShaderStage::VS },
	{"objectVS_common", wi::ShaderStage::VS },
	{"objectVS_common_tessellation", wi::ShaderStage::VS },
	{"objectVS_prepass", wi::ShaderStage::VS },
	{"objectVS_prepass_alphatest", wi::ShaderStage::VS },
	{"objectVS_prepass_tessellation", wi::ShaderStage::VS },
	{"objectVS_simple_tessellation", wi::ShaderStage::VS },
	{"shadowVS", wi::ShaderStage::VS },
	{"shadowVS_alphatest", wi::ShaderStage::VS },
	{"shadowVS_transparent", wi::ShaderStage::VS },
	{"screenVS", wi::ShaderStage::VS },
	{"voxelgridVS", wi::ShaderStage::VS },
	{"trailVS", wi::ShaderStage::VS },
	{"gaussian_splatVS", wi::ShaderStage::VS },



	{"objectDS", wi::ShaderStage::DS },
	{"objectDS_prepass", wi::ShaderStage::DS },
	{"objectDS_simple", wi::ShaderStage::DS },


	{"objectHS", wi::ShaderStage::HS },
	{"objectHS_prepass", wi::ShaderStage::HS },
	{"objectHS_simple", wi::ShaderStage::HS },

	{"emittedparticleMS", wi::ShaderStage::MS },

	{"objectMS", wi::ShaderStage::MS },
	{"objectMS_prepass", wi::ShaderStage::MS },
	{"objectMS_prepass_alphatest", wi::ShaderStage::MS },
	{"objectMS_simple", wi::ShaderStage::MS },
	{"shadowMS", wi::ShaderStage::MS },
	{"shadowMS_alphatest", wi::ShaderStage::MS },
	{"shadowMS_transparent", wi::ShaderStage::MS },

	{"objectAS", wi::ShaderStage::AS },


	//{"rtreflectionLIB", wi::ShaderStage::LIB },
};

struct Target
{
	wi::ShaderFormat format;
	std::string dir;
};
wi::vector<Target> targets;
wi::unordered_map<std::string, wi::shadercompiler::CompilerOutput> results;
bool rebuild = false;
bool shaderdump_enabled = false;

using namespace wi;

std::ostream nullout(&nullbuf);

int main(int argc, char* argv[])
{
	wi::shadercompiler::Flags compile_flags = wi::shadercompiler::Flags::NONE;

	wi::arguments::Parse(argc, argv);
	std::ostream* out;

	if (wi::arguments::HasArgument("quiet"))
	{
		wi::backlog::SetLogLevel(wi::backlog::LogLevel::Error);
		out = &nullout;
	}
	else
	{
		out = &std::cout;
	}

	*out << "[Wicked Engine Offline Shader Compiler]\n";
	*out << "Available command arguments:\n";
	*out << "\thlsl5 : \t\tCompile shaders to hlsl5 (dx11) format (using d3dcompiler)\n";
	*out << "\thlsl6 : \t\tCompile shaders to hlsl6 (dx12) format (using dxcompiler)\n";
	*out << "\tspirv : \t\tCompile shaders to spirv (vulkan) format (using dxcompiler)\n";
	*out << "\tmetal : \t\tCompile shaders to Apple Metal format (using dxcompiler and metal shader converter)\n";
	*out << "\thlsl6_xs : \t\tCompile shaders to hlsl6 Xbox Series native (dx12) format (requires Xbox SDK)\n";
	*out << "\tps5 : \t\t\tCompile shaders to PlayStation 5 native format (requires PlayStation 5 SDK)\n";
	*out << "\trebuild : \t\tAll shaders will be rebuilt, regardless if they are outdated or not\n";
	*out << "\tdisable_optimization : \tShaders will be compiled without optimizations\n";
	*out << "\tstrip_reflection : \tReflection will be stripped from shader binary to reduce file size\n";
	*out << "\tshaderdump : \t\tShaders will be saved to wiShaderDump.h C++ header file (can be combined with \"rebuild\")\n";
	*out << "\tdebuginfo : \t\tKeep symbol data for shader debugging\n";
	*out << "\tquiet : \t\tOnly print errors\n";
	*out << "\tsm6.1 : \t\tIncrease all compilations to shader model 6.1\n";
	*out << "\tsm6.2 : \t\tIncrease all compilations to shader model 6.2\n";
	*out << "\tsm6.3 : \t\tIncrease all compilations to shader model 6.3\n";
	*out << "\tsm6.4 : \t\tIncrease all compilations to shader model 6.4\n";
	*out << "\tsm6.5 : \t\tIncrease all compilations to shader model 6.5\n";
	*out << "\tsm6.6 : \t\tIncrease all compilations to shader model 6.6\n";
	*out << "Command arguments used: ";

	if (wi::arguments::HasArgument("hlsl5"))
	{
		targets.push_back({ ShaderFormat::HLSL5, "shaders/hlsl5/" });
		*out << "hlsl5 ";
	}
	if (wi::arguments::HasArgument("hlsl6"))
	{
		targets.push_back({ ShaderFormat::HLSL6, "shaders/hlsl6/" });
		*out << "hlsl6 ";
	}
	if (wi::arguments::HasArgument("spirv"))
	{
		targets.push_back({ ShaderFormat::SPIRV, "shaders/spirv/" });
		*out << "spirv ";
	}
	if (wi::arguments::HasArgument("metal"))
	{
		targets.push_back({ ShaderFormat::METAL, "shaders/metal/" });
		*out << "metal ";
	}
	if (wi::arguments::HasArgument("hlsl6_xs"))
	{
		targets.push_back({ ShaderFormat::HLSL6_XS, "shaders/hlsl6_xs/" });
		*out << "hlsl6_xs ";
	}
	if (wi::arguments::HasArgument("ps5"))
	{
		targets.push_back({ ShaderFormat::PS5, "shaders/ps5/" });
		*out << "ps5 ";
	}

	if (wi::arguments::HasArgument("shaderdump"))
	{
		shaderdump_enabled = true;
		*out << "shaderdump ";
	}

	if (wi::arguments::HasArgument("rebuild"))
	{
		rebuild = true;
		*out << "rebuild ";
	}

	if (wi::arguments::HasArgument("disable_optimization"))
	{
		compile_flags |= wi::shadercompiler::Flags::DISABLE_OPTIMIZATION;
		*out << "disable_optimization ";
	}

	if (wi::arguments::HasArgument("debuginfo"))
	{
		compile_flags |= wi::shadercompiler::Flags::KEEP_DEBUG_INFORMATION;
		*out << "debuginfo ";
	}

	if (wi::arguments::HasArgument("strip_reflection"))
	{
		compile_flags |= wi::shadercompiler::Flags::STRIP_REFLECTION;
		*out << "strip_reflection ";
	}

	ShaderModel minshadermodel_override = ShaderModel::SM_5_0;
	if (wi::arguments::HasArgument("sm6.1"))
	{
		minshadermodel_override = ShaderModel::SM_6_1;
		*out << "sm6.1 ";
	}
	if (wi::arguments::HasArgument("sm6.2"))
	{
		minshadermodel_override = ShaderModel::SM_6_2;
		*out << "sm6.2 ";
	}
	if (wi::arguments::HasArgument("sm6.3"))
	{
		minshadermodel_override = ShaderModel::SM_6_3;
		*out << "sm6.3 ";
	}
	if (wi::arguments::HasArgument("sm6.4"))
	{
		minshadermodel_override = ShaderModel::SM_6_4;
		*out << "sm6.4 ";
	}
	if (wi::arguments::HasArgument("sm6.5"))
	{
		minshadermodel_override = ShaderModel::SM_6_5;
		*out << "sm6.5 ";
	}
	if (wi::arguments::HasArgument("sm6.6"))
	{
		minshadermodel_override = ShaderModel::SM_6_6;
		*out << "sm6.6 ";
	}
	if (wi::arguments::HasArgument("sm6.7"))
	{
		minshadermodel_override = ShaderModel::SM_6_7;
		*out << "sm6.7 ";
	}

	*out << "\n";

	if (targets.empty())
	{
		targets = {
#ifdef __APPLE__
			{ ShaderFormat::METAL, "shaders/metal/" },
#else
			//{ ShaderFormat::HLSL5, "shaders/hlsl5/" },
			{ ShaderFormat::HLSL6, "shaders/hlsl6/" },
			{ ShaderFormat::SPIRV, "shaders/spirv/" },
#endif // __APPLE__
		};
		*out << "No shader formats were specified, assuming command arguments: spirv hlsl6\n";
	}

	// permutations for objectPS:
	shaders.push_back({ "objectPS", wi::ShaderStage::PS });
	for (auto& x : wi::scene::MaterialComponent::shaderTypeDefines)
	{
		shaders.back().permutations.emplace_back().defines = x;

		// same but with TRANSPARENT:
		shaders.back().permutations.emplace_back().defines = x;
		shaders.back().permutations.back().defines.push_back("TRANSPARENT");
	}

	// permutations for visibility_surfaceCS:
	shaders.push_back({ "visibility_surfaceCS", wi::ShaderStage::CS });
	for (auto& x : wi::scene::MaterialComponent::shaderTypeDefines)
	{
		shaders.back().permutations.emplace_back().defines = x;
	}

	// permutations for visibility_surfaceCS REDUCED:
	shaders.push_back({ "visibility_surfaceCS", wi::ShaderStage::CS });
	for (auto& x : wi::scene::MaterialComponent::shaderTypeDefines)
	{
		auto defines = x;
		defines.push_back("REDUCED");
		shaders.back().permutations.emplace_back().defines = defines;
	}

	// permutations for visibility_shadeCS:
	shaders.push_back({ "visibility_shadeCS", wi::ShaderStage::CS });
	for (auto& x : wi::scene::MaterialComponent::shaderTypeDefines)
	{
		shaders.back().permutations.emplace_back().defines = x;
	}

	// permutations for ssgiCS:
	shaders.push_back({ "ssgiCS", wi::ShaderStage::CS });
	shaders.back().permutations.emplace_back().defines = { "WIDE" };
	// permutations for ssgi_upsampleCS:
	shaders.push_back({ "ssgi_upsampleCS", wi::ShaderStage::CS });
	shaders.back().permutations.emplace_back().defines = { "WIDE" };

	// permutations for copyStencilBitPS:
	shaders.push_back({ "copyStencilBitPS", wi::ShaderStage::PS });
	shaders.back().permutations.emplace_back().defines = { "MSAA" };

	// permutations for yuv_to_rgbCS:
	shaders.push_back({ "yuv_to_rgbCS", wi::ShaderStage::CS });
	shaders.back().permutations.emplace_back().defines = { "ARRAY" };

	// Simplify permutation iteration:
	for (auto& shader : shaders)
	{
		if (shader.permutations.empty())
		{
			shader.permutations.emplace_back();
		}
	}

	wi::jobsystem::Initialize();
	wi::jobsystem::context ctx;

	static std::string SHADERSOURCEPATH = wi::renderer::GetShaderSourcePath();
	wi::helper::MakePathAbsolute(SHADERSOURCEPATH);

	*out << "[Wicked Engine Offline Shader Compiler] Searching for outdated shaders...\n";
	wi::Timer timer;
	static int errors = 0;

	for (auto& target : targets)
	{
		std::string SHADERPATH = target.dir;
		wi::helper::DirectoryCreate(SHADERPATH);

		for (auto& shader : shaders)
		{
			if (target.format == ShaderFormat::HLSL5)
			{
				if (
					shader.stage == ShaderStage::MS ||
					shader.stage == ShaderStage::AS ||
					shader.stage == ShaderStage::LIB
					)
				{
					// shader stage not applicable to HLSL5
					continue;
				}
			}

			for (auto& permutation : shader.permutations)
			{
				wi::jobsystem::Execute(ctx, [&target, &permutation, &shader, &out, SHADERPATH, compile_flags, minshadermodel_override](wi::jobsystem::JobArgs args) {

					std::string shaderbinaryfilename = SHADERPATH + shader.name;
					for (auto& def : permutation.defines)
					{
						shaderbinaryfilename += "_" + def;
					}
					shaderbinaryfilename += ".cso";

					wi::shadercompiler::CompilerOutput output;

					if (!wi::shadercompiler::IsShaderOutdated(shaderbinaryfilename))
					{
						if (!rebuild)
						{
							if (shaderdump_enabled)
							{
								auto vec = wi::allocator::make_shared<std::vector<uint8_t>>();

								if (wi::helper::FileRead(shaderbinaryfilename, *vec))
								{
									output.internal_state = vec;
									output.shaderdata = vec->data();
									output.shadersize = vec->size();
									locker.lock();
									results[shaderbinaryfilename] = output;
									*out << "up-to-date: " << shaderbinaryfilename << std::endl;
									locker.unlock();
								}
								else
								{
									locker.lock();
									std::cerr << "ERROR reading binary shader: " << shaderbinaryfilename << std::endl;
									locker.unlock();
								}
							}
							return;
						}
					}

					wi::shadercompiler::CompilerInput input;
					input.entrypoint = shader.entrypoint;
					input.flags = compile_flags;
					input.format = target.format;
					input.stage = shader.stage;
					input.shadersourcefilename = SHADERSOURCEPATH + shader.name + ".hlsl";
					input.include_directories.push_back(SHADERSOURCEPATH);
					input.include_directories.push_back(SHADERSOURCEPATH + wi::helper::GetDirectoryFromPath(shader.name));
					input.minshadermodel = std::max(shader.minshadermodel, minshadermodel_override);
					input.defines = permutation.defines;

					if (input.minshadermodel > ShaderModel::SM_5_0 && target.format == ShaderFormat::HLSL5)
					{
						// if shader format cannot support shader model, then we cancel the task without returning error
						return;
					}
					if (target.format == ShaderFormat::PS5 && (input.minshadermodel >= ShaderModel::SM_6_5 || input.stage == ShaderStage::MS || input.stage == ShaderStage::AS))
					{
						// TODO PS5 raytracing, mesh shader
						return;
					}
					if (target.format == ShaderFormat::HLSL6_XS && (input.stage == ShaderStage::MS || input.stage == ShaderStage::AS))
					{
						// TODO Xbox mesh shader
						return;
					}

					wi::shadercompiler::Compile(input, output);

					if (output.IsValid())
					{
						wi::shadercompiler::SaveShaderAndMetadata(shaderbinaryfilename, output);

						locker.lock();
						if (!output.error_message.empty())
						{
							std::cerr << output.error_message << "\n";
						}
						*out << "shader compiled: " << shaderbinaryfilename << "\n";
						if (shaderdump_enabled)
						{
							results[shaderbinaryfilename] = output;
						}
						locker.unlock();
					}
					else
					{
						locker.lock();
						std::cerr << "shader compile FAILED: " << shaderbinaryfilename << "\n" << output.error_message;
						errors++;
						locker.unlock();
					}

				});
			}
		}
	}
	wi::jobsystem::Wait(ctx);

	*out << "[Wicked Engine Offline Shader Compiler] Finished in " << std::setprecision(4) << timer.elapsed_seconds() << " seconds with " << errors << " errors\n";

	if (shaderdump_enabled)
	{
		*out << "[Wicked Engine Offline Shader Compiler] Creating ShaderDump...\n";
		timer.record();
		size_t total_raw = 0;
		size_t total_compressed = 0;
		std::string ss;
		ss += "namespace wiShaderDump {\n";
		for (auto& x : results)
		{
			auto& name = x.first;
			auto& output = x.second;

			wi::vector<uint8_t> compressed;
			bool success = wi::helper::Compress(output.shaderdata, output.shadersize, compressed, 9);
			if (success) {
				total_raw += output.shadersize;
				total_compressed += compressed.size();
			}
			else
			{
				wi::helper::DebugOut("Compression failed while creating shader dump!", wi::helper::DebugLevel::Error);
				continue;
			}

			std::string name_repl = name;
			std::replace(name_repl.begin(), name_repl.end(), '/', '_');
			std::replace(name_repl.begin(), name_repl.end(), '.', '_');
			std::replace(name_repl.begin(), name_repl.end(), '-', '_');
			ss += "static const uint8_t " + name_repl + "[] = {";
			for (size_t i = 0; i < compressed.size(); ++i)
			{
				ss += std::to_string((uint32_t)compressed[i]) + ",";
			}
			ss += "};\n";
		}
		*out << "[Wicked Engine Offline Shader Compiler] Compressed shaders: " << total_raw << " -> " << total_compressed << " (" << std::setprecision(3) << (100. * total_compressed / total_raw) << "%)" << std::endl;
		ss += "struct ShaderDumpEntry{const uint8_t* data; size_t size;};\n";
		ss += "static const wi::unordered_map<std::string, ShaderDumpEntry> shaderdump = {\n";
		for (auto& x : results)
		{
			auto& name = x.first;

			std::string name_repl = name;
			std::replace(name_repl.begin(), name_repl.end(), '/', '_');
			std::replace(name_repl.begin(), name_repl.end(), '.', '_');
			std::replace(name_repl.begin(), name_repl.end(), '-', '_');
			ss += "{\"" + name + "\", {" + name_repl + ",sizeof(" + name_repl + ")}},\n";
		}
		ss += "};\n"; // map end
		ss += "}\n"; // namespace end
		wi::helper::FileWrite("wiShaderDump.h", (uint8_t*)ss.c_str(), ss.length());
		*out << "[Wicked Engine Offline Shader Compiler] ShaderDump written to wiShaderDump.h in " << std::setprecision(4) << timer.elapsed_seconds() << " seconds\n";
	}

	wi::jobsystem::ShutDown();

	return errors;
}
