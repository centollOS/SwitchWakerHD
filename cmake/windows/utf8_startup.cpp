// The application manifest selects UTF-8 for Win32's process ANSI code page.
// UCRT also needs its multibyte locale selected before user static initializers.
#include <clocale>
#include <windows.h>
namespace {
void initialize_utf8() {
    std::setlocale(LC_ALL, ".UTF-8");
    std::setlocale(LC_NUMERIC, "C"); // Shader/config numbers must keep a decimal point.
    // Redirected output remains UTF-8 bytes; only adjust an attached console.
    DWORD mode;
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) SetConsoleOutputCP(CP_UTF8);
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) SetConsoleCP(CP_UTF8);
}
}
#if defined(__GNUC__) && !defined(_MSC_VER)
// llvm-mingw: initialize after the CRT, before ordinary C++ global constructors.
__attribute__((constructor(101))) static void utf8_before_globals() { initialize_utf8(); }
#else
// MSVC/clang-cl: XIC initialization precedes the C++ XCU constructor segment.
#pragma section(".CRT$XIC", long, read)
static int __cdecl utf8_before_globals() { initialize_utf8(); return 0; }
__declspec(allocate(".CRT$XIC")) int (__cdecl* utf8_initializer)() = utf8_before_globals;
#endif
