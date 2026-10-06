#pragma once

class StringBuf
{
public:
	StringBuf(uint32 bufferSize)
	{
		this->str = (uint8*)malloc(bufferSize + 4);
		this->allocated = true;
		this->length = 0;
		this->limit = bufferSize;
		if (!this->str)
		{
			// out of memory: nothing is written (the shader comes out empty and fails to compile)
			static uint8 empty[8];
			this->str = empty;
			this->allocated = false;
			this->limit = 0;
		}
	}

	~StringBuf()
	{
		if (this->allocated)
			free(this->str);
	}

	template<typename TFmt, typename ... TArgs>
	void addFmt(const TFmt& format, TArgs&&... args)
	{
		auto r = fmt::vformat_to_n((char*)(this->str + this->length), (size_t)(this->limit - this->length), fmt::detail::to_string_view(format), fmt::make_format_args(args...));
		this->length += (uint32)r.size;
	}

	void add(const char* appendedStr)
	{
		const char* outputStart = (char*)(this->str + this->length);
		char* output = (char*)outputStart;
		const char* outputEnd = (char*)(this->str + this->limit - 1);
		while (output < outputEnd)
		{
			char c = *appendedStr;
			if (c == '\0')
				break;
			*output = c;
			appendedStr++;
			output++;
		}
		this->length += (uint32)(output - outputStart);
		*output = '\0';
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
		if (uint8* shrunk = (uint8*)realloc(this->str, newLimit + 4))
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
		uint8* grown = (uint8*)realloc(this->str, newLimit + 4);
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
};