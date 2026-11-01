// Probe for the C++20 std::atomic<std::shared_ptr<T>> specialization, used by
// bitcoin/bitcoin#36011 behind `#ifdef __cpp_lib_atomic_shared_ptr`.
//
// The feature-test macro is defined in <version> (and <memory>). When it is
// present the primary path is compiled; otherwise the fallback mirrors the
// substitute class from src/util/btcsignals.h.

#include <version>

#include <atomic>
#include <cstdio>
#include <memory>

namespace detail {
#ifdef __cpp_lib_atomic_shared_ptr
template <typename T>
using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;
#else
template <typename T>
class atomic_shared_ptr
{
    std::shared_ptr<T> m_ptr;

public:
    explicit atomic_shared_ptr(std::shared_ptr<T> ptr) noexcept : m_ptr{std::move(ptr)} {}
    atomic_shared_ptr(const atomic_shared_ptr&) = delete;
    atomic_shared_ptr& operator=(const atomic_shared_ptr&) = delete;
    std::shared_ptr<T> load() const noexcept { return std::atomic_load(&m_ptr); }
    void store(std::shared_ptr<T> ptr) noexcept { std::atomic_store(&m_ptr, std::move(ptr)); }
};
#endif
} // namespace detail

int main()
{
    std::printf("__cplusplus            = %ldL\n", static_cast<long>(__cplusplus));
#ifdef _LIBCPP_VERSION
    std::printf("_LIBCPP_VERSION        = %d\n", _LIBCPP_VERSION);
#endif
#ifdef __clang_version__
    std::printf("__clang_version__      = %s\n", __clang_version__);
#endif
#ifdef __cpp_lib_atomic_shared_ptr
    std::printf("__cpp_lib_atomic_shared_ptr = %ldL -> using std::atomic<std::shared_ptr<T>>\n",
                static_cast<long>(__cpp_lib_atomic_shared_ptr));
#else
    std::printf("__cpp_lib_atomic_shared_ptr is NOT defined -> using the atomic_load/atomic_store fallback\n");
#endif

    detail::atomic_shared_ptr<int> p{std::make_shared<int>(41)};
    auto held{p.load()};
    ++*held;
    p.store(nullptr);
    const bool ok{*held == 42 && p.load() == nullptr};
    std::printf("load()/store() check: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
