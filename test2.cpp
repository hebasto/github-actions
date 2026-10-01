#include <cstring>
#include <locale>
#include <new>

alignas(4096) static unsigned char stale[8192]; // zeroed: on_list == 0

int main()
{
    void* g = stale + 8;                         // misaligned for lfree()
    constexpr std::size_t n = sizeof(std::ctype<char>);
    for (int i = 0; i < 1000; ++i) {
        void* p = ::operator new(n);
        for (std::size_t o = 0; o + sizeof g <= n; o += sizeof g)
            std::memcpy(static_cast<char*>(p) + o, &g, sizeof g);
        ::operator delete(p);
        std::locale("");                          // ctype<char> likely reuses p
    }
}
