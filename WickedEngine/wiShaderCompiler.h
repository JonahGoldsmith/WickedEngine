#pragma once

#include "wiGraphics.h"
#include "wiAllocator.h"
#include "../stb_ds.h"

#if defined(__has_include)
#if __has_include(<SDL3/SDL_assert.h>) && __has_include(<SDL3/SDL_log.h>)
#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_log.h>
#define WI_SHADERCOMPILER_HAS_SDL 1
#elif __has_include(<SDL_assert.h>) && __has_include(<SDL_log.h>)
#include <SDL_assert.h>
#include <SDL_log.h>
#define WI_SHADERCOMPILER_HAS_SDL 1
#endif
#endif

#ifndef WI_SHADERCOMPILER_HAS_SDL
#include <cassert>
#include <cstdio>
#define SDL_Log(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define SDL_LogError(category, ...) do { (void)(category); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#endif

#if defined(WI_SHADERCOMPILER_HAS_SDL)
#define WI_SC_ASSERT(cond) SDL_assert(cond)
#else
#define WI_SC_ASSERT(cond) assert(cond)
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace wi::shadercompiler
{
		// stb_ds array storage: each entry is a heap-owned std::string* that must be deleted explicitly.
		struct StringList
	{
		std::string** data = nullptr;

		size_t size() const noexcept
		{
			return data != nullptr ? arrlenu(data) : 0;
		}
		bool empty() const noexcept
		{
			return size() == 0;
		}
		std::string& operator[](size_t index)
		{
			WI_SC_ASSERT(index < size());
			return *data[index];
		}
		const std::string& operator[](size_t index) const
		{
			WI_SC_ASSERT(index < size());
			return *data[index];
		}
		void clear()
		{
			if (data != nullptr)
			{
				for (size_t i = 0; i < arrlenu(data); ++i)
				{
					delete data[i];
				}
				arrfree(data);
				data = nullptr;
			}
		}
		template<typename... Args>
		std::string& emplace_back(Args&&... args)
		{
			auto* item = new std::string(std::forward<Args>(args)...);
			arrput(data, item);
			return *item;
		}
		std::string& push_back(const std::string& value)
		{
			return emplace_back(value);
		}
		std::string& push_back(std::string&& value)
		{
			return emplace_back(std::move(value));
		}
		void reserve(size_t amount)
		{
			data = (std::string**)stbds_arrgrowf(data, sizeof(*data), 0, amount);
		}
		void resize(size_t amount)
		{
			const size_t old_size = size();
			if (amount < old_size)
			{
				for (size_t i = amount; i < old_size; ++i)
				{
					delete data[i];
				}
				if (data != nullptr)
				{
					arrsetlen(data, amount);
				}
				return;
			}
			if (amount > old_size)
			{
				const size_t grow_count = amount - old_size;
				reserve(amount);
				for (size_t i = 0; i < grow_count; ++i)
				{
					arrput(data, new std::string());
				}
			}
		}
	};
	inline void Init(StringList& list)
	{
		list.data = nullptr;
	}
	inline void Destroy(StringList& list)
	{
		if (list.data != nullptr)
		{
			list.clear();
		}
	}
	inline StringList Clone(const StringList& other)
	{
		StringList clone = {};
		for (size_t i = 0; i < other.size(); ++i)
		{
			clone.push_back(*other.data[i]);
		}
		return clone;
	}
	inline void Move(StringList& dst, StringList& src)
	{
		dst.clear();
		dst.data = src.data;
		src.data = nullptr;
	}

		// stb_ds array storage: raw bytes are held in a dynamic array and freed explicitly.
		struct ByteArray
	{
		uint8_t* data = nullptr;

		size_t size() const noexcept
		{
			return data != nullptr ? arrlenu(data) : 0;
		}
		bool empty() const noexcept
		{
			return size() == 0;
		}
		uint8_t& operator[](size_t index)
		{
			WI_SC_ASSERT(index < size());
			return data[index];
		}
		const uint8_t& operator[](size_t index) const
		{
			WI_SC_ASSERT(index < size());
			return data[index];
		}
		uint8_t* data_ptr() noexcept
		{
			return data;
		}
		const uint8_t* data_ptr() const noexcept
		{
			return data;
		}
		void clear()
		{
			if (data != nullptr)
			{
				arrfree(data);
				data = nullptr;
			}
		}
		void reserve(size_t amount)
		{
			data = (uint8_t*)stbds_arrgrowf(data, sizeof(*data), 0, amount);
		}
		void resize(size_t amount)
		{
			const size_t old_size = size();
			if (stbds_arrcap(data) < amount)
			{
				reserve(amount);
			}
			if (data != nullptr)
			{
				stbds_header(data)->length = amount;
			}
			if (amount > old_size && data != nullptr)
			{
				std::memset(data + old_size, 0, (amount - old_size) * sizeof(uint8_t));
			}
		}
		void push_back(uint8_t value)
		{
			arrput(data, value);
		}
		void append(const uint8_t* src, size_t count)
		{
			if (count == 0)
			{
				return;
			}
			const size_t offset = size();
			resize(offset + count);
			std::memcpy(data + offset, src, count);
		}
	};
	inline void Init(ByteArray& bytes)
	{
		bytes.data = nullptr;
	}
	inline void Destroy(ByteArray& bytes)
	{
		bytes.clear();
	}
	inline ByteArray Clone(const ByteArray& other)
	{
		ByteArray clone = {};
		clone.resize(other.size());
		if (other.size() > 0)
		{
			std::memcpy(clone.data, other.data, other.size() * sizeof(uint8_t));
		}
		return clone;
	}
	inline void Move(ByteArray& dst, ByteArray& src)
	{
		dst.clear();
		dst.data = src.data;
		src.data = nullptr;
	}

	enum class Flags
	{
		NONE = 0,
		DISABLE_OPTIMIZATION = 1 << 0,
		STRIP_REFLECTION = 1 << 1,
		KEEP_DEBUG_INFORMATION = 1 << 2,
	};
	struct CompilerInput
	{
		Flags flags = Flags::NONE;
		wi::ShaderFormat format = wi::ShaderFormat::SHADER_FORMAT_NONE;
		wi::ShaderStage stage = wi::ShaderStage::Count;
		// if the shader relies on a higher shader model feature, it must be declared here.
		// But the compiler can also choose a higher one internally, if needed
		wi::ShaderModel minshadermodel = wi::ShaderModel::SM_5_0;
		std::string shadersourcefilename;
		std::string entrypoint = "main";
		StringList include_directories;
		StringList defines;
	};
	struct CompilerOutput
	{
		wi::allocator::shared_ptr<void> internal_state;
		constexpr bool IsValid() const { return internal_state.IsValid(); }

		const uint8_t* shaderdata = nullptr;
		size_t shadersize = 0;
		ByteArray shaderhash;
		std::string error_message;
		StringList dependencies;
	};
		inline void Init(CompilerInput& input)
		{
			input = {};
		}
		// Ownership note: CompilerOutput contains stb_ds-backed arrays that must be Destroy()'d before reuse.
		inline void Init(CompilerOutput& output)
		{
			output = {};
		}
	inline void Destroy(CompilerInput& input)
	{
		Destroy(input.include_directories);
		Destroy(input.defines);
	}
	inline void Destroy(CompilerOutput& output)
	{
		Destroy(output.shaderhash);
		Destroy(output.dependencies);
		output.internal_state = {};
		output.shaderdata = nullptr;
		output.shadersize = 0;
		output.error_message.clear();
	}
	void Compile(const CompilerInput& input, CompilerOutput& output);

	bool SaveShaderAndMetadata(const std::string& shaderfilename, const CompilerOutput& output);
	bool IsShaderOutdated(const std::string& shaderfilename);

	void RegisterShader(const std::string& shaderfilename);
	size_t GetRegisteredShaderCount();
	bool CheckRegisteredShadersOutdated();
}

template<>
struct enable_bitmask_operators<wi::shadercompiler::Flags> {
	static const bool enable = true;
};
