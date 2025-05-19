#include <iostream>


#if defined(__x86_64__) || defined(__amd64__) || defined(__i386__)
#define HAVE_GETCPUID

#include <cpuid.h>

#include <cstdint>

// We can't use cpuid.h's __get_cpuid as it does not support subleafs.
void static inline GetCPUID(uint32_t leaf, uint32_t subleaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
{
#ifdef __GNUC__
    __cpuid_count(leaf, subleaf, a, b, c, d);
#else
  __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(leaf), "2"(subleaf));
#endif
}

#endif // defined(__x86_64__) || defined(__amd64__) || defined(__i386__)


int main()
{
#if defined(__x86_64__)
    std::cout << "__x86_64__ is defined: " << __x86_64__ << "\n";
#else
    std::cout << "__x86_64__ is not defined\n";
#endif

#if defined(__amd64__)
    std::cout << "__amd64__ is defined: " << __amd64__ << "\n";
#else
    std::cout << "__amd64__ is not defined\n";
#endif

#if defined(__aarch64__)
    std::cout << "__aarch64__ is defined: " << __aarch64__ << "\n";
#else
    std::cout << "__aarch64__ is not defined\n";
#endif

#if defined(__GNUC__)
    std::cout << "__GNUC__ is defined: " << __GNUC__ << "\n";
#else
    std::cout << "__GNUC__ is not defined\n";
#endif

#if defined(_MSC_VER)
    std::cout << "_MSC_VER is defined: " << _MSC_VER << "\n";
#else
    std::cout << "_MSC_VER is not defined\n";
#endif

#if defined(__clang__)
    std::cout << "__clang__ is defined: " << __clang__ << "\n";
#else
    std::cout << "__clang__ is not defined\n";
#endif

#if defined(_GNU_SOURCE)
    std::cout << "_GNU_SOURCE is defined: " << _GNU_SOURCE << "\n";
#else
    std::cout << "_GNU_SOURCE is not defined\n";
#endif

#if defined(_POSIX_C_SOURCE)
    std::cout << "_POSIX_C_SOURCE is defined: " << _POSIX_C_SOURCE << "\n";
#else
    std::cout << "_POSIX_C_SOURCE is not defined\n";
#endif

    std::cout << "\n";

#if defined(HAVE_GETCPUID)
    uint32_t eax, ebx, ecx, edx;
    GetCPUID(1, 0, eax, ebx, ecx, edx);
    std::cout << "SSE4 support:      " << ((ecx >> 19) & 1) << "\n\n";
    std::cout << "'xsave' support:   " << ((ecx >> 27) & 1) << "\n\n";
    std::cout << "'avx' support:     " << ((ecx >> 28) & 1) << "\n\n";

    GetCPUID(7, 0, eax, ebx, ecx, edx);
    std::cout << "AVX2 support:      " << ((ebx >> 5) & 1) << "\n\n";
    std::cout << "x86_SHANI support: " << ((ebx >> 29) & 1) << "\n\n";
#endif
}
