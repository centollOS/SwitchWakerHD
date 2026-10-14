// StringBuf (Cemu's shader source buffer) when an allocation fails: every write stays in bounds, and the length
// only counts what was written. Synthetic, no GPU. Also meant to run under ASan/TSan.
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static std::atomic<bool> g_failAlloc{false};
static void* test_malloc(size_t size) { return g_failAlloc ? nullptr : malloc(size); }
static void* test_realloc(void* ptr, size_t size) { return g_failAlloc ? nullptr : realloc(ptr, size); }
#define STRINGBUF_MALLOC test_malloc
#define STRINGBUF_REALLOC test_realloc
#include "util/helpers/StringBuf.h"

// c_str() is '\0'-terminated right after the length
static void check(const StringBuf& buf, const char* expected)
{
	assert(buf.getLen() == strlen(expected));
	assert(strcmp(buf.c_str(), expected) == 0);
}

// The constructor could not allocate: nothing is stored, and nothing is written past the object.
static void constructorFails()
{
	g_failAlloc = true;
	StringBuf buf(1024);
	check(buf, "");
	buf.add("const char");
	buf.add(std::string_view("string_view"));
	buf.addFmt("{} {}", 12345, "fmt");
	check(buf, "");
	buf.shrink_to_fit();
	buf.addFmt("{}", std::string(4096, 'x'));
	check(buf, "");
	g_failAlloc = false;
}

// A full buffer that cannot grow: addFmt keeps what fits and counts only that.
static void growthFails()
{
	StringBuf buf(8);
	g_failAlloc = true;
	buf.addFmt("{}", "0123456789abcdef");
	check(buf, "01234567");
	buf.add(std::string_view("dropped"));
	buf.add("dropped");
	buf.addFmt("{}", 42);
	check(buf, "01234567");
	g_failAlloc = false;
	buf.shrink_to_fit();
	check(buf, "01234567");
}

// When it can grow, addFmt and add(const char*) grow like add(std::string_view) instead of cutting the text.
static void growthWorks()
{
	StringBuf buf(8);
	buf.addFmt("{}-{}", "0123456789abcdef", 99);
	check(buf, "0123456789abcdef-99");
	std::string big(10000, 'y');
	buf.addFmt("{}", big);
	assert(buf.getLen() == 19 + big.size());
	assert(memcmp(buf.c_str() + 19, big.data(), big.size()) == 0);
	buf.shrink_to_fit();
	assert(buf.getLen() == 19 + big.size());

	StringBuf small(4);
	small.add("const char beyond the reservation");
	check(small, "const char beyond the reservation");
}

// Shader translation runs on several threads: buffers that could not allocate share no memory (TSan).
static void threadsWithoutMemory()
{
	g_failAlloc = true;
	auto work = [] {
		for (int i = 0; i < 1000; i++)
		{
			StringBuf buf(1 << 20);
			buf.add("x");
			buf.addFmt("{}", i);
			check(buf, "");
		}
	};
	std::thread a(work), b(work);
	a.join();
	b.join();
	g_failAlloc = false;
}

int main()
{
	constructorFails();
	growthFails();
	growthWorks();
	threadsWithoutMemory();
	return 0;
}
