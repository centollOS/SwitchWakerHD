#pragma once

// A test can make the allocations fail (runtime/tools/stringbuf_test.cpp)
#ifndef STRINGBUF_MALLOC
#define STRINGBUF_MALLOC malloc
#endif
#ifndef STRINGBUF_REALLOC
#define STRINGBUF_REALLOC realloc
#endif

class StringBuf
{
public:
	StringBuf(uint32 bufferSize)
	{
		this->str = (uint8*)STRINGBUF_MALLOC(bufferSize + 4);
		this->allocated = true;
		this->length = 0;
		this->limit = bufferSize;
		if (!this->str)
		{
			// out of memory: nothing is written (the shader comes out empty and fails to compile).
			// The '\0' that c_str() writes goes to this object's own byte, never to memory shared by threads.
			this->str = &this->fallback;
			this->allocated = false;
			this->limit = 0;
		}
	}

	// Not copyable: a copy would share (and free twice) the heap buffer, or point at the other
	// object's fallback byte. Every user passes StringBuf by pointer.
	StringBuf(const StringBuf&) = delete;
	StringBuf& operator=(const StringBuf&) = delete;

	~StringBuf()
	{
		if (this->allocated)
			free(this->str);
	}

	template<typename TFmt, typename ... TArgs>
	void addFmt(const TFmt& format, TArgs&&... args)
	{
		auto fmtStr = fmt::detail::to_string_view(format);
		auto fmtArgs = fmt::make_format_args(args...);
		auto r = fmt::vformat_to_n((char*)(this->str + this->length), (size_t)(this->limit - this->length), fmtStr, fmtArgs);
		// did not fit: grow like add(std::string_view) and format again
		if (this->length + r.size + 1 >= this->limit &&
			_reserve(std::max<uint32>(this->length + (uint32)r.size + 64, this->limit + this->limit / 2)))
			r = fmt::vformat_to_n((char*)(this->str + this->length), (size_t)(this->limit - this->length), fmtStr, fmtArgs);
		// only what was written: out of memory, the rest is dropped (the shader fails to compile)
		this->length += (uint32)std::min<size_t>(r.size, this->limit - this->length);
	}

	void add(const char* appendedStr)
	{
		add(std::string_view(appendedStr));  // grows like the others instead of cutting the text at the reservation
	}

	void add(std::string_view appendedStr)
	{
		size_t copyLen = appendedStr.size();
		if (this->length + copyLen + 1 >= this->limit &&
			!_reserve(std::max<uint32>(this->length + copyLen + 64, this->limit + this->limit / 2)))
			return;  // out of memory: dropped (the shader fails to compile)
		char* outputStart = (char*)(this->str + this->length);
		std::copy(appendedStr.data(), appendedStr.data() + copyLen, outputStart);
		length += copyLen;
		outputStart[copyLen] = '\0';
	}

	void reset()
	{
		length = 0;
	}

	uint32 getLen() const
	{
		return length;
	}

	const char* c_str() const
	{
		str[length] = '\0';
		return (const char*)str;
	}

	void shrink_to_fit()
	{
		if (!this->allocated)
			return;
		uint32 newLimit = this->length;
		if (uint8* shrunk = (uint8*)STRINGBUF_REALLOC(this->str, newLimit + 4))
		{
			this->str = shrunk;
			this->limit = newLimit;
		}
	}

private:
	bool _reserve(uint32 newLimit)
	{
		cemu_assert_debug(newLimit > length);
		if (!this->allocated)
			return false;
		uint8* grown = (uint8*)STRINGBUF_REALLOC(this->str, newLimit + 4);
		if (!grown)
			return false;
		this->str = grown;
		this->limit = newLimit;
		return true;
	}

	uint8*	str;
	uint32	length; /* in bytes */
	uint32	limit; /* in bytes */
	bool	allocated;
	uint8	fallback; /* str when the constructor could not allocate */
};