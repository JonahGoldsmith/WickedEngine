#include "wiShaderCompiler.h"
#include "wiPlatform.h"
#include "wiArchive.h"

#include <fstream>
#include <mutex>
#include <cstdlib>
#include <clocale>
#include <filesystem>

#if __has_include(<SDL3/SDL_filesystem.h>) && __has_include(<SDL3/SDL_iostream.h>) && __has_include(<SDL3/SDL_stdinc.h>)
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#endif

#if !defined(SDL_arraysize)
#define SDL_arraysize(array) (sizeof(array) / sizeof((array)[0]))
#endif

#ifdef PLATFORM_WINDOWS_DESKTOP
#define SHADERCOMPILER_ENABLED
#define SHADERCOMPILER_ENABLED_DXCOMPILER
#define SHADERCOMPILER_ENABLED_D3DCOMPILER
#include <wrl/client.h>
using namespace Microsoft::WRL;
#endif // _WIN32

#if defined(PLATFORM_LINUX) || defined(PLATFORM_APPLE)
#define SHADERCOMPILER_ENABLED
#define SHADERCOMPILER_ENABLED_DXCOMPILER
#define __RPC_FAR
#define ComPtr CComPtr
#include "Utility/dxc/WinAdapter.h"
#endif // defined(PLATFORM_LINUX) || defined(PLATFORM_APPLE)

#ifdef SHADERCOMPILER_ENABLED_DXCOMPILER
#include "Utility/dxc/dxcapi.h"
#include "wiGraphicsDevice_Vulkan.h"
#endif // SHADERCOMPILER_ENABLED_DXCOMPILER

#ifdef SHADERCOMPILER_ENABLED_D3DCOMPILER
#include <d3dcompiler.h>
#endif // SHADERCOMPILER_ENABLED_D3DCOMPILER

#if __has_include("wiShaderCompiler_XBOX.h")
#include "wiShaderCompiler_XBOX.h"
#define SHADERCOMPILER_XBOX_INCLUDED
#endif // __has_include("wiShaderCompiler_XBOX.h")

#if __has_include("wiShaderCompiler_PS5.h") && !defined(PLATFORM_PS5)
#include "wiShaderCompiler_PS5.h"
#define SHADERCOMPILER_PS5_INCLUDED
#endif // __has_include("wiShaderCompiler_PS5.h") && !PLATFORM_PS5

#ifdef PLATFORM_APPLE
#define SHADERCOMPILER_APPLE_INCLUDED
#include <metal_irconverter/metal_irconverter.h>
#include "wiGraphicsDevice_Metal.h"
#endif // PLATFORM_APPLE

#if defined(WICKED_MMGR_ENABLED)
// MMGR include must come after standard/project includes to avoid macro collisions in system headers.
#include "../forge-mmgr/FluidStudios/MemoryManager/mmgr.h"
#endif

using namespace wi;

namespace
{
	static bool ConvertUTF8ToWString(const std::string& input, std::wstring& output)
	{
		output.clear();
		char* converted = SDL_iconv_string("WCHAR_T", "UTF-8", input.c_str(), SDL_strlen(input.c_str()) + 1);
		if (converted == nullptr)
		{
			return false;
		}
		output = reinterpret_cast<const wchar_t*>(converted);
		SDL_free(converted);
		return true;
	}
	static bool ConvertUTF8ToWString(const char* input, std::wstring& output)
	{
		if (input == nullptr)
		{
			output.clear();
			return false;
		}
		return ConvertUTF8ToWString(std::string(input), output);
	}
	static bool ConvertWStringToUTF8(const wchar_t* input, std::string& output)
	{
		output.clear();
		if (input == nullptr)
		{
			return false;
		}
		char* converted = SDL_iconv_wchar_utf8(input);
		if (converted == nullptr)
		{
			return false;
		}
		output = converted;
		SDL_free(converted);
		return true;
	}
	static bool FileExistsSDL(const std::string& path)
	{
		SDL_PathInfo info = {};
		return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
	}
	static uint64_t FileTimestampSDL(const std::string& path)
	{
		SDL_PathInfo info = {};
		if (!SDL_GetPathInfo(path.c_str(), &info))
		{
			return 0;
		}
		return (uint64_t)info.modify_time;
	}
	static bool FileWriteSDL(const std::string& path, const uint8_t* data, size_t size)
	{
		SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "wb");
		if (stream == nullptr)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "wi::shadercompiler: failed to open output for writing: %s", path.c_str());
			return false;
		}
		const size_t written = size > 0 ? SDL_WriteIO(stream, data, size) : 0;
		const bool closed = SDL_CloseIO(stream);
		return written == size && closed;
	}
	static std::string GetDirectoryFromPathStd(const std::string& path)
	{
		return std::filesystem::path(path).parent_path().string();
	}
	static std::string GetFilenameFromPathStd(const std::string& path)
	{
		return std::filesystem::path(path).filename().string();
	}
	static std::string ReplaceExtensionStd(const std::string& path, const char* extension)
	{
		std::filesystem::path fs_path(path);
		fs_path.replace_extension(extension);
		return fs_path.string();
	}
	static bool DirectoryCreateSDL(const std::string& path)
	{
		if (path.empty())
		{
			return true;
		}
		return SDL_CreateDirectory(path.c_str());
	}
	static void MakePathAbsoluteStd(std::string& path)
	{
		std::error_code ec;
		const std::filesystem::path absolute_path = std::filesystem::absolute(std::filesystem::path(path), ec);
		if (!ec)
		{
			path = absolute_path.string();
		}
	}
	static void MakePathRelativeStd(const std::string& root, std::string& path)
	{
		std::error_code ec;
		const std::filesystem::path relative_path = std::filesystem::relative(std::filesystem::path(path), std::filesystem::path(root), ec);
		if (!ec && !relative_path.empty())
		{
			path = relative_path.string();
		}
	}

	// Use unqualified malloc/free to allow MMGR macro overrides when enabled.
	static char* DuplicateCStringMMGRCompatible(const char* source)
	{
		if (source == nullptr)
		{
			return nullptr;
		}
		const size_t length = std::strlen(source) + 1;
#if defined(WICKED_MMGR_ENABLED)
		char* duplicated = (char*)mmgrAllocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_malloc, sizeof(void*), length);
#else
		char* duplicated = (char*)malloc(length);
#endif
		if (duplicated != nullptr)
		{
			std::memcpy(duplicated, source, length);
		}
		return duplicated;
	}
	static void FreeMMGRCompatible(void* memory)
	{
		if (memory == nullptr)
		{
			return;
		}
#if defined(WICKED_MMGR_ENABLED)
		mmgrDeallocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_free, memory);
#else
		free(memory);
#endif
	}

	static bool ReadFileBytes(const std::string& fileName, wi::shadercompiler::ByteArray& data, size_t max_read = ~0ull, size_t offset = 0)
	{
		std::ifstream file(fileName, std::ios::binary | std::ios::ate);
		if (!file.is_open())
		{
			return false;
		}

		const std::streampos file_end = file.tellg();
		if (file_end < 0)
		{
			return false;
		}

		const size_t file_size = static_cast<size_t>(file_end);
		if (offset > file_size)
		{
			return false;
		}

		size_t read_size = file_size - offset;
		if (max_read != ~0ull && read_size > max_read)
		{
			read_size = max_read;
		}

		wi::shadercompiler::ByteArray temp = {};
		temp.resize(read_size);
		file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		if (read_size > 0)
		{
			file.read(reinterpret_cast<char*>(temp.data), static_cast<std::streamsize>(read_size));
			if (!file && !file.eof())
			{
				wi::shadercompiler::Destroy(temp);
				return false;
			}
		}

		wi::shadercompiler::Move(data, temp);
		wi::shadercompiler::Destroy(temp);
		return true;
	}

	// stb_ds array storage: each slot owns a std::wstring* and must be deleted explicitly on teardown.
	struct WideStringList
	{
		std::wstring** items = nullptr;

		void clear()
		{
			if (items != nullptr)
			{
				const size_t count = size();
				for (size_t i = 0; i < count; ++i)
				{
					delete items[i];
				}
				arrfree(items);
				items = nullptr;
			}
		}
		template<typename... Args>
		std::wstring& emplace_back(Args&&... args)
		{
			auto* value = new std::wstring(std::forward<Args>(args)...);
			items = (std::wstring**)stbds_arrgrowf(items, sizeof(*items), 1, 0);
			stbds_header(items)->length++;
			items[stbds_header(items)->length - 1] = value;
			return *value;
		}
		void push_back(const wchar_t* value)
		{
			emplace_back(value);
		}
		void push_back(const std::wstring& value)
		{
			emplace_back(value);
		}
		size_t size() const
		{
			return items != nullptr ? arrlenu(items) : 0;
		}
		const wchar_t* c_str(size_t index) const
		{
			WI_SC_ASSERT(index < size());
			return items[index]->c_str();
		}
	};
	inline void Destroy(WideStringList& list)
	{
		list.clear();
	}
}

namespace wi::shadercompiler
{

#ifdef SHADERCOMPILER_ENABLED_DXCOMPILER
	struct InternalState_DXC
	{
		DxcCreateInstanceProc DxcCreateInstance = nullptr;

		InternalState_DXC(const std::string& modifier = "")
		{
#ifdef _WIN32
#ifdef SHADERCOMPILER_XBOX_INCLUDED
			wi::shadercompiler::xbox::GetSetSDKPath(); // Sets Xbox SDK path globally for DLL loading
#endif // SHADERCOMPILER_XBOX_INCLUDED
			const std::string library = "dxcompiler" + modifier + ".dll";
			HMODULE dxcompiler = wiLoadLibrary(library.c_str());
#elif defined(PLATFORM_LINUX)
			const std::string library = "./libdxcompiler" + modifier + ".so";
			HMODULE dxcompiler = wiLoadLibrary(library.c_str());
#elif defined(PLATFORM_APPLE)
			const std::string library = "./libdxcompiler" + modifier + ".dylib";
			HMODULE dxcompiler = wiLoadLibrary(library.c_str());
#endif
			if (dxcompiler != nullptr)
			{
				DxcCreateInstance = (DxcCreateInstanceProc)wiGetProcAddress(dxcompiler, "DxcCreateInstance");
				if (DxcCreateInstance != nullptr)
				{
					ComPtr<IDxcCompiler3> dxcCompiler;
					HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
					WI_SC_ASSERT(SUCCEEDED(hr));
					ComPtr<IDxcVersionInfo> info;
					hr = dxcCompiler->QueryInterface(IID_PPV_ARGS(&info));
					WI_SC_ASSERT(SUCCEEDED(hr));
					uint32_t minor = 0;
					uint32_t major = 0;
					hr = info->GetVersion(&major, &minor);
					WI_SC_ASSERT(SUCCEEDED(hr));
					SDL_Log("wi::shadercompiler: loaded %s (version: %u.%u)", library.c_str(), major, minor);
				}
			}
			else
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "wi::shadercompiler: could not load library %s", library.c_str());
#ifdef PLATFORM_LINUX
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", dlerror()); // print dlopen() error detail: https://linux.die.net/man/3/dlerror
#endif // PLATFORM_LINUX
			}

		}
	};
	inline InternalState_DXC& dxc_compiler()
	{
		static InternalState_DXC internal_state;
		return internal_state;
	}
	inline InternalState_DXC& dxc_compiler_xs()
	{
		static InternalState_DXC internal_state("_xs");
		return internal_state;
	}

	void Compile_DXCompiler(const CompilerInput& input, CompilerOutput& output)
	{
		InternalState_DXC& compiler_internal = input.format == ShaderFormat::HLSL6_XS ? dxc_compiler_xs() : dxc_compiler();
		if (compiler_internal.DxcCreateInstance == nullptr)
		{
			return;
		}

		ComPtr<IDxcUtils> dxcUtils;
		ComPtr<IDxcCompiler3> dxcCompiler;
		wi::shadercompiler::ByteArray shadersourcedata;
		WideStringList args;
		// stb_ds array storage: command-line arguments are collected here and released by cleanup().
		const wchar_t** args_raw = nullptr;

		HRESULT hr = compiler_internal.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils));
		WI_SC_ASSERT(SUCCEEDED(hr));
		hr = compiler_internal.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
		WI_SC_ASSERT(SUCCEEDED(hr));

		auto cleanup = [&]()
		{
			arrfree(args_raw);
			args_raw = nullptr;
			Destroy(args);
			wi::shadercompiler::Destroy(shadersourcedata);
		};

		if (dxcCompiler == nullptr)
		{
			cleanup();
			return;
		}

		if (!ReadFileBytes(input.shadersourcefilename, shadersourcedata))
		{
			cleanup();
			return;
		}

		// https://github.com/microsoft/DirectXShaderCompiler/wiki/Using-dxc.exe-and-dxcompiler.dll#dxcompiler-dll-interface

		args.emplace_back(
			//L"-res-may-alias",
			//L"-flegacy-macro-expansion",
			//L"-no-legacy-cbuf-layout",
			//L"-pack-optimized", // this has problem with tessellation shaders: https://github.com/microsoft/DirectXShaderCompiler/issues/3362
			//L"-all-resources-bound",
			//L"-Gis", // Force IEEE strictness
			//L"-Gec", // Enable backward compatibility mode
			//L"-Ges", // Enable strict mode
			//L"-O0", // Optimization Level 0
			//L"-enable-16bit-types",
			L"-Wno-conversion");

		if (has_flag(input.flags, Flags::DISABLE_OPTIMIZATION))
		{
			args.push_back(L"-Od");
		}
		
		if (has_flag(input.flags, Flags::KEEP_DEBUG_INFORMATION))
		{
			args.push_back(L"-Zi");
			args.push_back(L"-Qembed_debug");
		}

		switch (input.format)
		{
		case ShaderFormat::HLSL6:
		case ShaderFormat::HLSL6_XS:
			args.push_back(L"-rootsig-define"); args.push_back(L"WICKED_ENGINE_DEFAULT_ROOTSIGNATURE");
			if (has_flag(input.flags, Flags::STRIP_REFLECTION))
			{
				args.push_back(L"-Qstrip_reflect"); // only valid in HLSL6 compiler
			}
			break;
		case ShaderFormat::SPIRV:
			args.push_back(L"-spirv");
			args.push_back(L"-fspv-target-env=vulkan1.3");
			args.push_back(L"-fvk-use-dx-layout");
			args.push_back(L"-fvk-use-dx-position-w");
			args.push_back(L"-fvk-b-shift"); args.push_back(std::to_wstring((int)wi::GraphicsDevice_Vulkan::VULKAN_BINDING_SHIFT_B)); args.push_back(L"0");
			args.push_back(L"-fvk-t-shift"); args.push_back(std::to_wstring((int)wi::GraphicsDevice_Vulkan::VULKAN_BINDING_SHIFT_T)); args.push_back(L"0");
			args.push_back(L"-fvk-u-shift"); args.push_back(std::to_wstring((int)wi::GraphicsDevice_Vulkan::VULKAN_BINDING_SHIFT_U)); args.push_back(L"0");
			args.push_back(L"-fvk-s-shift"); args.push_back(std::to_wstring((int)wi::GraphicsDevice_Vulkan::VULKAN_BINDING_SHIFT_S)); args.push_back(L"0");

			// These require mutable descriptor extension which have issues on linux so it's not used:
			//args.push_back(L"-fvk-bind-sampler-heap"); args.push_back(L"0"); args.push_back(std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_SAMPLER));
			//args.push_back(L"-fvk-bind-resource-heap"); args.push_back(L"0"); args.push_back(std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_RESOURCE));

			// Non-mutable bindless descriptor heap bind points:
			args.push_back(L"-D"); args.push_back(L"DESCRIPTOR_SET_BINDLESS_SAMPLER=" + std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_SAMPLER));
			args.push_back(L"-D"); args.push_back(L"DESCRIPTOR_SET_BINDLESS_STORAGE_BUFFER=" + std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_STORAGE_BUFFER));
			args.push_back(L"-D"); args.push_back(L"DESCRIPTOR_SET_BINDLESS_UNIFORM_TEXEL_BUFFER=" + std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_UNIFORM_TEXEL_BUFFER));
			args.push_back(L"-D"); args.push_back(L"DESCRIPTOR_SET_BINDLESS_SAMPLED_IMAGE=" + std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_SAMPLED_IMAGE));
			args.push_back(L"-D"); args.push_back(L"DESCRIPTOR_SET_BINDLESS_STORAGE_IMAGE=" + std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_STORAGE_IMAGE));
			args.push_back(L"-D"); args.push_back(L"DESCRIPTOR_SET_BINDLESS_STORAGE_TEXEL_BUFFER=" + std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_STORAGE_TEXEL_BUFFER));
			args.push_back(L"-D"); args.push_back(L"DESCRIPTOR_SET_BINDLESS_ACCELERATION_STRUCTURE=" + std::to_wstring((int)wi::GraphicsDevice_Vulkan::DESCRIPTOR_SET_BINDLESS_ACCELERATION_STRUCTURE));

			break;
			case ShaderFormat::METAL:
				args.push_back(L"-D"); args.push_back(L"__metal__");
				break;
			default:
				WI_SC_ASSERT(0);
				cleanup();
				return;
			}

		ShaderModel minshadermodel = input.minshadermodel;

		// global minshadermodel override:
		//minshadermodel = std::max(minshadermodel, ShaderModel::SM_6_2);

		if (input.format == ShaderFormat::HLSL6_XS)
		{
			minshadermodel = ShaderModel::SM_6_2;
			args.push_back(L"-enable-16bit-types");
		}
		
		if (input.format == ShaderFormat::METAL)
		{
			minshadermodel = std::max(minshadermodel, ShaderModel::SM_6_6);
		}

		args.push_back(L"-T");
		switch (input.stage)
		{
		case ShaderStage::MS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"ms_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"ms_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"ms_6_7");
				break;
			}
			break;
		case ShaderStage::AS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"as_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"as_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"as_6_7");
				break;
			}
			break;
		case ShaderStage::VS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"vs_6_0");
				break;
			case ShaderModel::SM_6_1:
				args.push_back(L"vs_6_1");
				break;
			case ShaderModel::SM_6_2:
				args.push_back(L"vs_6_2");
				break;
			case ShaderModel::SM_6_3:
				args.push_back(L"vs_6_3");
				break;
			case ShaderModel::SM_6_4:
				args.push_back(L"vs_6_4");
				break;
			case ShaderModel::SM_6_5:
				args.push_back(L"vs_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"vs_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"vs_6_7");
				break;
			}
			break;
		case ShaderStage::HS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"hs_6_0");
				break;
			case ShaderModel::SM_6_1:
				args.push_back(L"hs_6_1");
				break;
			case ShaderModel::SM_6_2:
				args.push_back(L"hs_6_2");
				break;
			case ShaderModel::SM_6_3:
				args.push_back(L"hs_6_3");
				break;
			case ShaderModel::SM_6_4:
				args.push_back(L"hs_6_4");
				break;
			case ShaderModel::SM_6_5:
				args.push_back(L"hs_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"hs_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"hs_6_7");
				break;
			}
			break;
		case ShaderStage::DS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"ds_6_0");
				break;
			case ShaderModel::SM_6_1:
				args.push_back(L"ds_6_1");
				break;
			case ShaderModel::SM_6_2:
				args.push_back(L"ds_6_2");
				break;
			case ShaderModel::SM_6_3:
				args.push_back(L"ds_6_3");
				break;
			case ShaderModel::SM_6_4:
				args.push_back(L"ds_6_4");
				break;
			case ShaderModel::SM_6_5:
				args.push_back(L"ds_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"ds_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"ds_6_7");
				break;
			}
			break;
		case ShaderStage::GS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"gs_6_0");
				break;
			case ShaderModel::SM_6_1:
				args.push_back(L"gs_6_1");
				break;
			case ShaderModel::SM_6_2:
				args.push_back(L"gs_6_2");
				break;
			case ShaderModel::SM_6_3:
				args.push_back(L"gs_6_3");
				break;
			case ShaderModel::SM_6_4:
				args.push_back(L"gs_6_4");
				break;
			case ShaderModel::SM_6_5:
				args.push_back(L"gs_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"gs_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"gs_6_7");
				break;
			}
			break;
		case ShaderStage::PS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"ps_6_0");
				break;
			case ShaderModel::SM_6_1:
				args.push_back(L"ps_6_1");
				break;
			case ShaderModel::SM_6_2:
				args.push_back(L"ps_6_2");
				break;
			case ShaderModel::SM_6_3:
				args.push_back(L"ps_6_3");
				break;
			case ShaderModel::SM_6_4:
				args.push_back(L"ps_6_4");
				break;
			case ShaderModel::SM_6_5:
				args.push_back(L"ps_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"ps_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"ps_6_7");
				break;
			}
			break;
		case ShaderStage::CS:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"cs_6_0");
				break;
			case ShaderModel::SM_6_1:
				args.push_back(L"cs_6_1");
				break;
			case ShaderModel::SM_6_2:
				args.push_back(L"cs_6_2");
				break;
			case ShaderModel::SM_6_3:
				args.push_back(L"cs_6_3");
				break;
			case ShaderModel::SM_6_4:
				args.push_back(L"cs_6_4");
				break;
			case ShaderModel::SM_6_5:
				args.push_back(L"cs_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"cs_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"cs_6_7");
				break;
			}
			break;
		case ShaderStage::LIB:
			switch (minshadermodel)
			{
			default:
				args.push_back(L"lib_6_5");
				break;
			case ShaderModel::SM_6_6:
				args.push_back(L"lib_6_6");
				break;
			case ShaderModel::SM_6_7:
				args.push_back(L"lib_6_7");
				break;
			}
			break;
		default:
			WI_SC_ASSERT(0);
			return;
		}

		for (size_t i = 0; i < input.defines.size(); ++i)
		{
			args.push_back(L"-D");
			ConvertUTF8ToWString(input.defines[i], args.emplace_back());
		}

		for (size_t i = 0; i < input.include_directories.size(); ++i)
		{
			args.push_back(L"-I");
			ConvertUTF8ToWString(input.include_directories[i], args.emplace_back());
		}

#ifdef SHADERCOMPILER_XBOX_INCLUDED
		if (input.format == ShaderFormat::HLSL6_XS)
		{
			wi::shadercompiler::xbox::AddArguments(input, args);
		}
#endif // SHADERCOMPILER_XBOX_INCLUDED

		// Entry point parameter:
		std::wstring wentry;
		ConvertUTF8ToWString(input.entrypoint, wentry);
		args.push_back(L"-E");
		args.push_back(wentry.c_str());

		// Add source file name as last parameter. This will be displayed in error messages
		std::wstring wsource;
		ConvertUTF8ToWString(GetFilenameFromPathStd(input.shadersourcefilename), wsource);
		args.push_back(wsource.c_str());

			DxcBuffer Source;
			Source.Ptr = shadersourcedata.data;
			Source.Size = shadersourcedata.size();
			Source.Encoding = DXC_CP_ACP;

		struct IncludeHandler final : public IDxcIncludeHandler
		{
			const CompilerInput* input = nullptr;
			CompilerOutput* output = nullptr;
			ComPtr<IDxcIncludeHandler> dxcIncludeHandler;

			HRESULT STDMETHODCALLTYPE LoadSource(
				_In_z_ LPCWSTR pFilename,                                 // Candidate filename.
				_COM_Outptr_result_maybenull_ IDxcBlob** ppIncludeSource  // Resultant source object for included file, nullptr if not found.
			) override
			{
				HRESULT hr = dxcIncludeHandler->LoadSource(pFilename, ppIncludeSource);
				if (SUCCEEDED(hr))
				{
					std::string& filename = output->dependencies.emplace_back();
					ConvertWStringToUTF8(pFilename, filename);
				}
				return hr;
			}
			HRESULT STDMETHODCALLTYPE QueryInterface(
				/* [in] */ REFIID riid,
				/* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppvObject) override
			{
				return dxcIncludeHandler->QueryInterface(riid, ppvObject);
			}

			ULONG STDMETHODCALLTYPE AddRef(void) override
			{
				return 0;
			}
			ULONG STDMETHODCALLTYPE Release(void) override
			{
				return 0;
			}
		} includehandler;
		includehandler.input = &input;
		includehandler.output = &output;

		hr = dxcUtils->CreateDefaultIncludeHandler(&includehandler.dxcIncludeHandler);
		WI_SC_ASSERT(SUCCEEDED(hr));

			for (size_t i = 0; i < args.size(); ++i)
			{
				args_raw = (const wchar_t**)stbds_arrgrowf((void*)args_raw, sizeof(*args_raw), 1, 0);
				stbds_header(args_raw)->length++;
				args_raw[stbds_header(args_raw)->length - 1] = args.c_str(i);
		}

#ifndef _WIN32
		// work around https://github.com/microsoft/DirectXShaderCompiler/issues/7869
		static std::mutex locale_mut;
		static char* prev_locale;
		// we need to use a mutex anyway, so no point in using atomic_int
		static int scope = 0;

		{
			std::scoped_lock lock(locale_mut);
			if (scope++ == 0) {
				prev_locale = DuplicateCStringMMGRCompatible(setlocale(LC_ALL, nullptr));
				setlocale(LC_ALL, "en_US.UTF-8");
			}
		}
#endif
		ComPtr<IDxcResult> pResults;
		hr = dxcCompiler->Compile(
			&Source,						// Source buffer.
			args_raw,					// Array of pointers to arguments.
			(uint32_t)args.size(),		// Number of arguments.
			&includehandler,		// User-provided interface to handle #include directives (optional).
			IID_PPV_ARGS(&pResults)	// Compiler output status, buffer, and errors.
		);
#ifndef _WIN32
		{
			std::scoped_lock lock(locale_mut);
			if (--scope == 0) {
				setlocale(LC_ALL, prev_locale);
				FreeMMGRCompatible(prev_locale);
			}
		}
#endif
			cleanup();
			WI_SC_ASSERT(SUCCEEDED(hr));

		ComPtr<IDxcBlobUtf8> pErrors = nullptr;
		hr = pResults->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
		WI_SC_ASSERT(SUCCEEDED(hr));
		if (pErrors != nullptr && pErrors->GetStringLength() != 0)
		{
			output.error_message = pErrors->GetStringPointer();
		}

			HRESULT hrStatus;
			hr = pResults->GetStatus(&hrStatus);
			WI_SC_ASSERT(SUCCEEDED(hr));
			if (FAILED(hrStatus))
			{
				cleanup();
				return;
			}

		ComPtr<IDxcBlob> pShader = nullptr;
		hr = pResults->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pShader), nullptr);
		WI_SC_ASSERT(SUCCEEDED(hr));
		if (pShader != nullptr)
		{
			output.dependencies.push_back(input.shadersourcefilename);
			output.shaderdata = (const uint8_t*)pShader->GetBufferPointer();
			output.shadersize = pShader->GetBufferSize();
			
			// keep the blob alive == keep shader pointer valid!
			auto internal_state = wi::allocator::make_shared<ComPtr<IDxcBlob>>();
			*internal_state = pShader;
			output.internal_state = internal_state;
			
#ifdef SHADERCOMPILER_APPLE_INCLUDED
			if (input.format == ShaderFormat::METAL)
			{
				static HMODULE irconverter = wiLoadLibrary("libmetalirconverter.dylib");
				WI_SC_ASSERT(irconverter); // You must install the metal shader converter
				if (irconverter != nullptr)
				{
#define LINK_IR(name) using PFN_##name = decltype(&name); static PFN_##name name = (PFN_##name)wiGetProcAddress(irconverter, #name);
					LINK_IR(IRCompilerCreate)
					LINK_IR(IRCompilerSetMinimumGPUFamily)
					LINK_IR(IRCompilerSetCompatibilityFlags)
					LINK_IR(IRCompilerSetValidationFlags)
					LINK_IR(IRCompilerSetEntryPointName)
					LINK_IR(IRCompilerSetStageInGenerationMode)
					LINK_IR(IRObjectCreateFromDXIL)
					LINK_IR(IRCompilerAllocCompileAndLink)
					LINK_IR(IRErrorDestroy)
					LINK_IR(IRMetalLibBinaryCreate)
					LINK_IR(IRObjectGetMetalLibBinary)
					LINK_IR(IRMetalLibGetBytecodeSize)
					LINK_IR(IRMetalLibGetBytecode)
					LINK_IR(IRMetalLibBinaryDestroy)
					LINK_IR(IRObjectDestroy)
					LINK_IR(IRCompilerDestroy)
					LINK_IR(IRRootSignatureCreateFromDescriptor)
					LINK_IR(IRCompilerSetGlobalRootSignature)
					LINK_IR(IRCompilerEnableGeometryAndTessellationEmulation)
					//LINK_IR(IRRootSignatureDestroy)
					
					static IRDescriptorRange1 binding_resources[] =
					{
						{ .RangeType = IRDescriptorRangeTypeCBV, .BaseShaderRegister = SDL_arraysize(GraphicsDevice_Metal::RootLayout::root_cbvs), .RegisterSpace = 0, .OffsetInDescriptorsFromTableStart = IRDescriptorRangeOffsetAppend, .NumDescriptors = (SDL_arraysize(DescriptorBindingTable::CBV) - SDL_arraysize(GraphicsDevice_Metal::RootLayout::root_cbvs)), .Flags = IRDescriptorRangeFlagDataStaticWhileSetAtExecute },
						{ .RangeType = IRDescriptorRangeTypeSRV, .BaseShaderRegister = 0, .RegisterSpace = 0, .OffsetInDescriptorsFromTableStart = IRDescriptorRangeOffsetAppend, .NumDescriptors = SDL_arraysize(DescriptorBindingTable::SRV), .Flags = IRDescriptorRangeFlagDataStaticWhileSetAtExecute },
						{ .RangeType = IRDescriptorRangeTypeUAV, .BaseShaderRegister = 0, .RegisterSpace = 0, .OffsetInDescriptorsFromTableStart = IRDescriptorRangeOffsetAppend, .NumDescriptors = SDL_arraysize(DescriptorBindingTable::UAV), .Flags = IRDescriptorRangeFlagDataStaticWhileSetAtExecute },
					};
					static IRDescriptorRange1 binding_samplers[] =
					{
						{ .RangeType = IRDescriptorRangeTypeSampler, .BaseShaderRegister = 0, .RegisterSpace = 0, .OffsetInDescriptorsFromTableStart = IRDescriptorRangeOffsetAppend, .NumDescriptors = SDL_arraysize(DescriptorBindingTable::SAM), .Flags = IRDescriptorRangeFlagDescriptorsVolatile },
						{ .RangeType = IRDescriptorRangeTypeSampler, .BaseShaderRegister = wi::STATIC_SAMPLER_SLOT_BEGIN, .RegisterSpace = 0, .OffsetInDescriptorsFromTableStart = IRDescriptorRangeOffsetAppend, .NumDescriptors = SDL_arraysize(GraphicsDevice_Metal::StaticSamplerDescriptors::samplers), .Flags = IRDescriptorRangeFlagDescriptorsVolatile }, // static samplers workaround (**)
					};
					static IRRootParameter1 root_parameters[] =
					{
						{
							.ParameterType = IRRootParameterType32BitConstants,
							.Constants = { .ShaderRegister = 999, .Num32BitValues = SDL_arraysize(GraphicsDevice_Metal::RootLayout::constants), .RegisterSpace = 0 },
							.ShaderVisibility = IRShaderVisibilityAll
						},
						{
							.ParameterType = IRRootParameterTypeCBV,
							.Descriptor = { .ShaderRegister = 0, .RegisterSpace = 0, .Flags = IRRootDescriptorFlagNone },
							.ShaderVisibility = IRShaderVisibilityAll
						},
						{
							.ParameterType = IRRootParameterTypeCBV,
							.Descriptor = { .ShaderRegister = 1, .RegisterSpace = 0, .Flags = IRRootDescriptorFlagNone },
							.ShaderVisibility = IRShaderVisibilityAll
						},
						{
							.ParameterType = IRRootParameterTypeCBV,
							.Descriptor = { .ShaderRegister = 2, .RegisterSpace = 0, .Flags = IRRootDescriptorFlagNone },
							.ShaderVisibility = IRShaderVisibilityAll
						},
						{
							.ParameterType = IRRootParameterTypeDescriptorTable,
							.DescriptorTable = { .NumDescriptorRanges = SDL_arraysize(binding_resources), .pDescriptorRanges = binding_resources },
							.ShaderVisibility = IRShaderVisibilityAll
						},
						{
							.ParameterType = IRRootParameterTypeDescriptorTable,
							.DescriptorTable = { .NumDescriptorRanges = SDL_arraysize(binding_samplers), .pDescriptorRanges = binding_samplers },
							.ShaderVisibility = IRShaderVisibilityAll
						},
					};
					
					// Actually the static samplers don't work in Metal so I don't know why there is an API to describe them:
					//	Instead they will be put into the binding sampler table as a workaround (**)
					
					//static IRStaticSamplerDescriptor static_samplers[] =
					//{
					//	{ .ShaderRegister = 100, .RegisterSpace = 0, .Filter = IRFilterMinMagMipLinear, .AddressU = IRTextureAddressModeClamp, .AddressV = IRTextureAddressModeClamp, .AddressW = IRTextureAddressModeClamp, .MipLODBias = 0, .MaxAnisotropy = 1, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//	{ .ShaderRegister = 101, .RegisterSpace = 0, .Filter = IRFilterMinMagMipLinear, .AddressU = IRTextureAddressModeWrap, .AddressV = IRTextureAddressModeWrap, .AddressW = IRTextureAddressModeWrap, .MipLODBias = 0, .MaxAnisotropy = 1, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//	{ .ShaderRegister = 102, .RegisterSpace = 0, .Filter = IRFilterMinMagMipLinear, .AddressU = IRTextureAddressModeMirror, .AddressV = IRTextureAddressModeMirror, .AddressW = IRTextureAddressModeMirror, .MipLODBias = 0, .MaxAnisotropy = 1, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//
					//	{ .ShaderRegister = 103, .RegisterSpace = 0, .Filter = IRFilterMinMagMipPoint, .AddressU = IRTextureAddressModeClamp, .AddressV = IRTextureAddressModeClamp, .AddressW = IRTextureAddressModeClamp, .MipLODBias = 0, .MaxAnisotropy = 1, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//	{ .ShaderRegister = 104, .RegisterSpace = 0, .Filter = IRFilterMinMagMipPoint, .AddressU = IRTextureAddressModeWrap, .AddressV = IRTextureAddressModeWrap, .AddressW = IRTextureAddressModeWrap, .MipLODBias = 0, .MaxAnisotropy = 1, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//	{ .ShaderRegister = 105, .RegisterSpace = 0, .Filter = IRFilterMinMagMipPoint, .AddressU = IRTextureAddressModeMirror, .AddressV = IRTextureAddressModeMirror, .AddressW = IRTextureAddressModeMirror, .MipLODBias = 0, .MaxAnisotropy = 1, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//
					//	{ .ShaderRegister = 106, .RegisterSpace = 0, .Filter = IRFilterAnisotropic, .AddressU = IRTextureAddressModeClamp, .AddressV = IRTextureAddressModeClamp, .AddressW = IRTextureAddressModeClamp, .MipLODBias = 0, .MaxAnisotropy = 16, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//	{ .ShaderRegister = 107, .RegisterSpace = 0, .Filter = IRFilterAnisotropic, .AddressU = IRTextureAddressModeWrap, .AddressV = IRTextureAddressModeWrap, .AddressW = IRTextureAddressModeWrap, .MipLODBias = 0, .MaxAnisotropy = 16, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//	{ .ShaderRegister = 108, .RegisterSpace = 0, .Filter = IRFilterAnisotropic, .AddressU = IRTextureAddressModeMirror, .AddressV = IRTextureAddressModeMirror, .AddressW = IRTextureAddressModeMirror, .MipLODBias = 0, .MaxAnisotropy = 16, .ComparisonFunc = IRComparisonFunctionNever, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = FLT_MAX, .ShaderVisibility = IRShaderVisibilityAll },
					//
					//	{ .ShaderRegister = 109, .RegisterSpace = 0, .Filter = IRFilterComparisonMinMagLinearMipPoint, .AddressU = IRTextureAddressModeClamp, .AddressV = IRTextureAddressModeClamp, .AddressW = IRTextureAddressModeClamp, .MipLODBias = 0, .MaxAnisotropy = 1, .ComparisonFunc = IRComparisonFunctionGreaterEqual, .BorderColor = IRStaticBorderColorOpaqueBlack, .MinLOD = 0, .MaxLOD = 0, .ShaderVisibility = IRShaderVisibilityAll },
					//};
					
					static const IRVersionedRootSignatureDescriptor desc = {
						.version = IRRootSignatureVersion_1_1,
						.desc_1_1.Flags = IRRootSignatureFlags(IRRootSignatureFlagAllowInputAssemblerInputLayout | IRRootSignatureFlagCBVSRVUAVHeapDirectlyIndexed | IRRootSignatureFlagSamplerHeapDirectlyIndexed),
						.desc_1_1.NumParameters = SDL_arraysize(root_parameters),
						.desc_1_1.pParameters = root_parameters,
						//.desc_1_1.NumStaticSamplers = SDL_arraysize(static_samplers),
						//.desc_1_1.pStaticSamplers = static_samplers,
					};
					static IRError* pRootSigError = nullptr;
					static IRRootSignature* pRootSig = IRRootSignatureCreateFromDescriptor(&desc, &pRootSigError);
					if (pRootSig == nullptr)
					{
						WI_SC_ASSERT(0);
						IRErrorDestroy(pRootSigError);
					}
					
					IRCompiler* pCompiler = IRCompilerCreate();
					IRCompilerSetMinimumGPUFamily(pCompiler, IRGPUFamilyMetal3);
					IRCompilerSetCompatibilityFlags(pCompiler, IRCompatibilityFlags(IRCompatibilityFlagBoundsCheck | IRCompatibilityFlagPositionInvariance | IRCompatibilityFlagTextureMinLODClamp | IRCompatibilityFlagSamplerLODBias));
					IRCompilerSetValidationFlags(pCompiler, IRCompilerValidationFlagAll);
					IRCompilerSetEntryPointName(pCompiler, input.entrypoint.c_str());
					IRCompilerSetGlobalRootSignature(pCompiler, pRootSig);
					IRCompilerSetStageInGenerationMode(pCompiler, IRStageInCodeGenerationModeUseMetalVertexFetch);
					//IRCompilerEnableGeometryAndTessellationEmulation(pCompiler, true);
					IRObject* pDXIL = IRObjectCreateFromDXIL(output.shaderdata, output.shadersize, IRBytecodeOwnershipNone);
					IRError* pError = nullptr;
					IRObject* pOutIR = IRCompilerAllocCompileAndLink(pCompiler, NULL, pDXIL, &pError);
					if (pOutIR == nullptr)
					{
						WI_SC_ASSERT(0);
						IRErrorDestroy(pError);
					}
					IRMetalLibBinary* pMetallib = IRMetalLibBinaryCreate();
					IRShaderStage irstage = {};
					switch (input.stage) {
						case ShaderStage::VS:
							irstage = IRShaderStageVertex;
							break;
						case ShaderStage::PS:
							irstage = IRShaderStageFragment;
							break;
						case ShaderStage::GS:
							irstage = IRShaderStageGeometry;
							break;
						case ShaderStage::HS:
							irstage = IRShaderStageHull;
							break;
						case ShaderStage::DS:
							irstage = IRShaderStageDomain;
							break;
						case ShaderStage::MS:
							irstage = IRShaderStageMesh;
							break;
						case ShaderStage::AS:
							irstage = IRShaderStageAmplification;
							break;
						case ShaderStage::CS:
							irstage = IRShaderStageCompute;
							break;
						default:
							WI_SC_ASSERT(0);
							break;
					}
					IRObjectGetMetalLibBinary(pOutIR, irstage, pMetallib);
					size_t metallibSize = IRMetalLibGetBytecodeSize(pMetallib);
						auto internal_state = wi::allocator::make_shared<ByteArray>(); // lifetime storage of pointer
						internal_state->resize(metallibSize);
						IRMetalLibGetBytecode(pMetallib, internal_state->data);
					if (
						input.stage == ShaderStage::VS ||
						input.stage == ShaderStage::GS ||
						input.stage == ShaderStage::CS ||
						input.stage == ShaderStage::MS ||
						input.stage == ShaderStage::AS
						)
					{
						LINK_IR(IRShaderReflectionCreate)
						LINK_IR(IRObjectGetReflection)
						LINK_IR(IRShaderReflectionCopyVertexInfo)
						LINK_IR(IRShaderReflectionCopyGeometryInfo)
						LINK_IR(IRShaderReflectionCopyComputeInfo)
						LINK_IR(IRShaderReflectionCopyMeshInfo)
						LINK_IR(IRShaderReflectionCopyAmplificationInfo)
						LINK_IR(IRShaderReflectionDestroy)
						
						// Add the numthreads information to end of the shader:
						IRShaderReflection* reflection = IRShaderReflectionCreate();
						bool success = IRObjectGetReflection(pOutIR, irstage, reflection);
						WI_SC_ASSERT(success);
						wi::GraphicsDevice_Metal::ShaderAdditionalData reflection_append = {};
						if (input.stage == ShaderStage::VS)
						{
							IRVersionedVSInfo vs_info = {};
							success = IRShaderReflectionCopyVertexInfo(reflection, IRReflectionVersion_1_0, &vs_info);
							WI_SC_ASSERT(success);
							reflection_append.needs_draw_params = vs_info.info_1_0.needs_draw_params ? 1 : 0;
							reflection_append.vertex_output_size_in_bytes = vs_info.info_1_0.vertex_output_size_in_bytes;
						}
						if (input.stage == ShaderStage::GS)
						{
							IRVersionedGSInfo gs_info = {};
							success = IRShaderReflectionCopyGeometryInfo(reflection, IRReflectionVersion_1_0, &gs_info);
							WI_SC_ASSERT(success);
							reflection_append.max_input_primitives_per_mesh_threadgroup = gs_info.info_1_0.max_input_primitives_per_mesh_threadgroup;
						}
						if (input.stage == ShaderStage::CS)
						{
							IRVersionedCSInfo cs_info = {};
							success = IRShaderReflectionCopyComputeInfo(reflection, IRReflectionVersion_1_0, &cs_info);
							WI_SC_ASSERT(success);
							reflection_append.numthreads.width = cs_info.info_1_0.tg_size[0];
							reflection_append.numthreads.height = cs_info.info_1_0.tg_size[1];
							reflection_append.numthreads.depth = cs_info.info_1_0.tg_size[2];
						}
						else if (input.stage == ShaderStage::MS)
						{
							IRVersionedMSInfo ms_info = {};
							success = IRShaderReflectionCopyMeshInfo(reflection, IRReflectionVersion_1_0, &ms_info);
							WI_SC_ASSERT(success);
							reflection_append.numthreads.width = ms_info.info_1_0.num_threads[0];
							reflection_append.numthreads.height = ms_info.info_1_0.num_threads[1];
							reflection_append.numthreads.depth = ms_info.info_1_0.num_threads[2];
						}
						else if (input.stage == ShaderStage::AS)
						{
							IRVersionedASInfo as_info = {};
							success = IRShaderReflectionCopyAmplificationInfo(reflection, IRReflectionVersion_1_0, &as_info);
							WI_SC_ASSERT(success);
							reflection_append.numthreads.width = as_info.info_1_0.num_threads[0];
							reflection_append.numthreads.height = as_info.info_1_0.num_threads[1];
							reflection_append.numthreads.depth = as_info.info_1_0.num_threads[2];
						}
						internal_state->append(reinterpret_cast<const uint8_t*>(&reflection_append), sizeof(reflection_append));
						IRShaderReflectionDestroy(reflection);
					}
					output.internal_state = internal_state;
						output.shaderdata = internal_state->data;
					output.shadersize = internal_state->size();
					IRMetalLibBinaryDestroy(pMetallib);
					IRObjectDestroy(pDXIL);
					IRObjectDestroy(pOutIR);
					IRCompilerDestroy(pCompiler);
					//IRRootSignatureDestroy(pRootSig);
				}
			}
#endif // SHADERCOMPILER_APPLE_INCLUDED
			
		}

		if (input.format == ShaderFormat::HLSL6)
		{
			ComPtr<IDxcBlob> pHash = nullptr;
			hr = pResults->GetOutput(DXC_OUT_SHADER_HASH, IID_PPV_ARGS(&pHash), nullptr);
			WI_SC_ASSERT(SUCCEEDED(hr));
				if (pHash != nullptr)
				{
					DxcShaderHash* pHashBuf = (DxcShaderHash*)pHash->GetBufferPointer();
					output.shaderhash.resize(_countof(pHashBuf->HashDigest));
					std::memcpy(output.shaderhash.data, pHashBuf->HashDigest, output.shaderhash.size());
				}
				}
				cleanup();
			}
#endif // SHADERCOMPILER_ENABLED_DXCOMPILER

#ifdef SHADERCOMPILER_ENABLED_D3DCOMPILER
	struct InternalState_D3DCompiler
	{
		using PFN_D3DCOMPILE = decltype(&D3DCompile);
		PFN_D3DCOMPILE D3DCompile = nullptr;

		InternalState_D3DCompiler()
		{
			if (D3DCompile != nullptr)
			{
				return; // already initialized
			}

			HMODULE d3dcompiler = wiLoadLibrary("d3dcompiler_47.dll");
			if (d3dcompiler != nullptr)
			{
					D3DCompile = (PFN_D3DCOMPILE)wiGetProcAddress(d3dcompiler, "D3DCompile");
					if (D3DCompile != nullptr)
					{
						SDL_Log("wi::shadercompiler: loaded d3dcompiler_47.dll");
					}
				}
			}
	};
	inline InternalState_D3DCompiler& d3d_compiler()
	{
		static InternalState_D3DCompiler internal_state;
		return internal_state;
	}

	void Compile_D3DCompiler(const CompilerInput& input, CompilerOutput& output)
	{
		if (d3d_compiler().D3DCompile == nullptr)
		{
			return;
		}

		wi::shadercompiler::ByteArray shadersourcedata;
		if (!ReadFileBytes(input.shadersourcefilename, shadersourcedata))
		{
			return;
		}

		if (input.minshadermodel > ShaderModel::SM_5_0)
		{
			output.error_message = "SHADERFORMAT_HLSL5 cannot support specified minshadermodel!";
			wi::shadercompiler::Destroy(shadersourcedata);
			return;
		}

		D3D_SHADER_MACRO defines[] = {
			{"HLSL5", "1"},
			{"DISABLE_WAVE_INTRINSICS", "1"},
			{NULL, NULL},
		};

		const char* target = nullptr;
		switch (input.stage)
		{
			default:
			case ShaderStage::MS:
			case ShaderStage::AS:
			case ShaderStage::LIB:
				// not applicable
				wi::shadercompiler::Destroy(shadersourcedata);
				return;
		case ShaderStage::VS:
			target = "vs_5_0";
			break;
		case ShaderStage::HS:
			target = "hs_5_0";
			break;
		case ShaderStage::DS:
			target = "ds_5_0";
			break;
		case ShaderStage::GS:
			target = "gs_5_0";
			break;
		case ShaderStage::PS:
			target = "ps_5_0";
			break;
		case ShaderStage::CS:
			target = "cs_5_0";
			break;
		}

			struct IncludeHandler final : public ID3DInclude
			{
				const CompilerInput* input = nullptr;
				CompilerOutput* output = nullptr;
				struct FileData
				{
					uint8_t* data = nullptr;
					size_t size = 0;
				};
				// stb_ds array storage: included files are tracked in a heap-owned list and released explicitly.
				FileData* filedatas = nullptr;

				void Destroy()
				{
					if (filedatas != nullptr)
					{
						const size_t count = arrlenu(filedatas);
						for (size_t i = 0; i < count; ++i)
					{
						delete[] filedatas[i].data;
					}
					arrfree(filedatas);
					filedatas = nullptr;
				}
			}

			HRESULT Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override
			{
				for (size_t i = 0; i < input->include_directories.size(); ++i)
				{
					std::string filename = input->include_directories[i] + pFileName;
					if (!FileExistsSDL(filename))
						continue;
					wi::shadercompiler::ByteArray filedata;
					if (ReadFileBytes(filename, filedata))
					{
						FileData loaded;
						loaded.size = filedata.size();
						// Ownership note: each include copy is heap-owned until Destroy() releases it.
						loaded.data = loaded.size > 0 ? new uint8_t[loaded.size] : nullptr;
							if (loaded.size > 0)
							{
								std::memcpy(loaded.data, filedata.data, loaded.size);
							}
						filedatas = (FileData*)stbds_arrgrowf(filedatas, sizeof(*filedatas), 1, 0);
						stbds_header(filedatas)->length++;
						filedatas[stbds_header(filedatas)->length - 1] = loaded;
						output->dependencies.push_back(filename);
						*ppData = loaded.data;
						*pBytes = (UINT)loaded.size;
						return S_OK;
					}
				}
				return E_FAIL;
			}

			HRESULT Close(LPCVOID pData) override
			{
				return S_OK;
			}
		} includehandler;
		includehandler.input = &input;
		includehandler.output = &output;

		// https://docs.microsoft.com/en-us/windows/win32/direct3dhlsl/d3dcompile-constants
		UINT Flags1 = 0;
		if (has_flag(input.flags, Flags::DISABLE_OPTIMIZATION))
		{
			Flags1 |= D3DCOMPILE_SKIP_OPTIMIZATION;
		}


			ComPtr<ID3DBlob> code;
			ComPtr<ID3DBlob> errors;
			HRESULT hr = d3d_compiler().D3DCompile(
				shadersourcedata.data,
				shadersourcedata.size(),
				input.shadersourcefilename.c_str(),
				defines,
				&includehandler, //D3D_COMPILE_STANDARD_FILE_INCLUDE,
			input.entrypoint.c_str(),
			target,
			Flags1,
			0,
			&code,
			&errors
		);

		if (errors)
		{
			output.error_message = (const char*)errors->GetBufferPointer();
		}

			if (SUCCEEDED(hr))
			{
				output.dependencies.push_back(input.shadersourcefilename);
				output.shaderdata = (const uint8_t*)code->GetBufferPointer();
				output.shadersize = code->GetBufferSize();

			// keep the blob alive == keep shader pointer valid!
			auto internal_state = wi::allocator::make_shared<ComPtr<ID3D10Blob>>();
				*internal_state = code;
				output.internal_state = internal_state;
			}
			includehandler.Destroy();
			wi::shadercompiler::Destroy(shadersourcedata);
		}
#endif // SHADERCOMPILER_ENABLED_D3DCOMPILER

	void Compile(const CompilerInput& input, CompilerOutput& output)
	{
		wi::shadercompiler::Destroy(output);
		wi::shadercompiler::Init(output);

#ifdef SHADERCOMPILER_ENABLED
		switch (input.format)
		{
		default:
			break;

#ifdef SHADERCOMPILER_ENABLED_DXCOMPILER
		case ShaderFormat::HLSL6:
		case ShaderFormat::SPIRV:
		case ShaderFormat::HLSL6_XS:
		case ShaderFormat::METAL:
			Compile_DXCompiler(input, output);
			break;
#endif // SHADERCOMPILER_ENABLED_DXCOMPILER

#ifdef SHADERCOMPILER_ENABLED_D3DCOMPILER
		case ShaderFormat::HLSL5:
			Compile_D3DCompiler(input, output);
			break;
#endif // SHADERCOMPILER_ENABLED_D3DCOMPILER

#ifdef SHADERCOMPILER_PS5_INCLUDED
		case ShaderFormat::PS5:
			wi::shadercompiler::ps5::Compile(input, output);
			break;
#endif // SHADERCOMPILER_PS5_INCLUDED

		}
#endif // SHADERCOMPILER_ENABLED
	}

	constexpr const char* shadermetaextension = "wishadermeta";
	bool SaveShaderAndMetadata(const std::string& shaderfilename, const CompilerOutput& output)
	{
	#ifdef SHADERCOMPILER_ENABLED
		bool success = false;
		StringList dependencies = wi::shadercompiler::Clone(output.dependencies);
		DirectoryCreateSDL(GetDirectoryFromPathStd(shaderfilename));

		wi::Archive dependencyLibrary(ReplaceExtensionStd(shaderfilename, shadermetaextension), false);
		if (dependencyLibrary.IsOpen())
		{
			std::string rootdir = dependencyLibrary.GetSourceDirectory();
			for (size_t i = 0; i < dependencies.size(); ++i)
			{
				MakePathRelativeStd(rootdir, dependencies[i]);
			}
			const uint64_t dependency_count = (uint64_t)dependencies.size();
			dependencyLibrary << dependency_count;
			for (size_t i = 0; i < dependencies.size(); ++i)
			{
				dependencyLibrary << dependencies[i];
			}
		}

		if (FileWriteSDL(shaderfilename, output.shaderdata, output.shadersize))
		{
			success = true;
		}
		wi::shadercompiler::Destroy(dependencies);
		return success;
	#endif // SHADERCOMPILER_ENABLED

		return false;
	}
	bool IsShaderOutdated(const std::string& shaderfilename)
	{
#ifdef SHADERCOMPILER_ENABLED
		std::string filepath = shaderfilename;
		MakePathAbsoluteStd(filepath);
		if (!FileExistsSDL(filepath))
		{
			return true; // no shader file = outdated shader, apps can attempt to rebuild it
		}
		std::string dependencylibrarypath = ReplaceExtensionStd(shaderfilename, shadermetaextension);
		if (!FileExistsSDL(dependencylibrarypath))
		{
			return false; // no metadata file = no dependency, up to date (for example packaged builds)
		}

		const uint64_t tim = FileTimestampSDL(filepath);

		wi::Archive dependencyLibrary(dependencylibrarypath);
		StringList dependencies = {};
		if (dependencyLibrary.IsOpen())
		{
			std::string rootdir = dependencyLibrary.GetSourceDirectory();
			uint64_t dependency_count = 0;
			dependencyLibrary >> dependency_count;
			dependencies.reserve((size_t)dependency_count);
			for (uint64_t i = 0; i < dependency_count; ++i)
			{
				std::string dependency;
				dependencyLibrary >> dependency;
				dependencies.push_back(std::move(dependency));
			}

			for (size_t i = 0; i < dependencies.size(); ++i)
			{
				std::string dependencypath = rootdir + dependencies[i];
				MakePathAbsoluteStd(dependencypath);
				if (FileExistsSDL(dependencypath))
				{
					const uint64_t dep_tim = FileTimestampSDL(dependencypath);

					if (tim < dep_tim)
					{
						wi::shadercompiler::Destroy(dependencies);
						return true;
					}
				}
			}
			wi::shadercompiler::Destroy(dependencies);
		}
#endif // SHADERCOMPILER_ENABLED

		return false;
	}

	std::mutex locker;
	// stb_ds array storage: unique shader filenames are tracked explicitly and freed on teardown.
	StringList registered_shaders = {};
	void RegisterShader(const std::string& shaderfilename)
	{
	#ifdef SHADERCOMPILER_ENABLED
		std::scoped_lock lock(locker);
		for (size_t i = 0; i < registered_shaders.size(); ++i)
		{
			if (registered_shaders[i] == shaderfilename)
			{
				return;
			}
		}
		registered_shaders.push_back(shaderfilename);
	#endif // SHADERCOMPILER_ENABLED
	}
	size_t GetRegisteredShaderCount()
	{
		std::scoped_lock lock(locker);
		return registered_shaders.size();
	}
	bool CheckRegisteredShadersOutdated()
	{
	#ifdef SHADERCOMPILER_ENABLED
		std::scoped_lock lock(locker);
		for (size_t i = 0; i < registered_shaders.size(); ++i)
		{
			if (IsShaderOutdated(registered_shaders[i]))
			{
				return true;
			}
		}
	#endif // SHADERCOMPILER_ENABLED
		return false;
	}
	void DestroyRegisteredShaders()
	{
		std::scoped_lock lock(locker);
		wi::shadercompiler::Destroy(registered_shaders);
	}
}

#define STB_DS_IMPLEMENTATION
#include "../stb_ds.h"
#undef STB_DS_IMPLEMENTATION
