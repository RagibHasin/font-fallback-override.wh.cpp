#pragma once

#include <cstdint>
#include <cwchar>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <utility>

#ifdef LOG_MUTEX
#include <source_location>
#include <sstream>
#include <string>

#include <windhawk_api.h>
#endif

constexpr auto hiSurrogateBase = 0xD800UL;
constexpr auto loSurrogateBase = 0xDC00UL;
constexpr auto postSurrogateBase = 0xE000UL;
constexpr auto suppmentaryPlaneBase = 0x10000UL;

/// Unicode decoding.
///
/// GDI APIs expose UTF-16 strings. The fallback rules use Unicode scalar
/// values, so combine surrogate pairs before testing ranges.
///
/// Invalid/unpaired surrogates are returned as their UTF-16 value. This makes
/// the behavior deterministic rather than silently discarding them.
static std::pair<
    char32_t,
    size_t>
DecodeCodePoint(const std::wstring_view text) {
    auto ch = static_cast<uint32_t>(text[0]);

    if (ch >= hiSurrogateBase && ch < loSurrogateBase && 1 < text.size()) {
        auto loSurrogate = static_cast<uint32_t>(text[1]);

        constexpr auto hiSurrogateShiftMask = 10UL;

        if (loSurrogate >= loSurrogateBase && loSurrogate < postSurrogateBase) {
            return {
                suppmentaryPlaneBase + ((ch - hiSurrogateBase) << hiSurrogateShiftMask)
                    + (loSurrogate - loSurrogateBase),
                2
            };
        }
    }

    return {ch, 1};
}

static char32_t ParseHex(const WCHAR* str) {
    if (!str || !*str) {
        return 0;
    }

    try {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
        return std::wcstoul(str, nullptr, 16);
    } catch (...) {
        return 0;
    }
}

// MARK: Excerpt from https://github.com/dragazo/rustex begins

// a rust-style mutex - holds a value of type T and manages access via guards around SharedLockable.
// in order to ensure the soundness of the data contract, you should not access the stored value
// except directly via the handles.
template <typename T, typename SharedLockable = std::shared_mutex>
class Mutex {
  private: // -- detail -- //
    template <typename T_cv, template <typename> typename Locker>
    class guard_common {
      private: // -- detail -- //
        T_cv* ptr;
        Locker<SharedLockable> lock;
#ifdef LOG_MUTEX
        std::source_location _location;
#endif

        friend Mutex;
        explicit guard_common(
            const Mutex& m
#ifdef LOG_MUTEX
            ,
            std::source_location location = std::source_location::current()
#endif
        )
            : ptr(std::addressof(m.raw_val))
            , lock(m.raw_mut)
#ifdef LOG_MUTEX
            , _location(location)
#endif
        {
#ifdef LOG_MUTEX
            std::wostringstream trace;
            trace << "file: " << _location.file_name() << '(' << _location.line() << ':'
                  << _location.column() << ") `" << _location.function_name() << "`: ";
            auto traceStr = trace.str();
            Wh_Log(L"%s : locked mutex", traceStr.c_str());
#endif
        }

      public: // -- access -- //
        T_cv* operator->() const noexcept {
            return ptr;
        }
        T_cv& operator*() const noexcept {
            return *ptr;
        }

#ifdef LOG_MUTEX
        guard_common(const guard_common&) = delete;
        guard_common(guard_common&&) = delete;
        guard_common& operator=(const guard_common&) = delete;
        guard_common& operator=(guard_common&&) = delete;

        ~guard_common() {
            std::wostringstream trace;
            trace << "file: " << _location.file_name() << '(' << _location.line() << ':'
                  << _location.column() << ") `" << _location.function_name() << "`: ";
            auto traceStr = trace.str();
            Wh_Log(L"%s : unlocked mutex", traceStr.c_str());
        }
#endif
    };

    mutable SharedLockable raw_mut;
    mutable T raw_val;

  public: // -- guards -- //
    // when these guards are moved-from they are considered invalid and should not be used at all
    // afterwards (like rust)
    using mut_guard = guard_common<T, std::lock_guard>;
    using guard = guard_common<const T, std::shared_lock>;

    // -- intrinsics -- //
    // constructs the wrapped object of type T with the specified arguments
    template <typename... Args>
    constexpr explicit Mutex(Args&&... args)
        requires(std::is_constructible_v<
                 T,
                 Args...>)
        : raw_val(std::forward<Args>(args)...) {
    }

    Mutex(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    ~Mutex() = default;

    Mutex& operator=(const Mutex&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    // -- access -- //
    // returns a handle that facilitates shared readonly access of the wrapped value
    [[nodiscard]] guard lock(
#ifdef LOG_MUTEX
        std::source_location location = std::source_location::current()
#endif
    ) const {
        return guard{
            *this
#ifdef LOG_MUTEX
            ,
            location
#endif
        };
    }
    // returns a handle that facilitates unique read/write access of the wrapped value
    [[nodiscard]] mut_guard lock_mut(
#ifdef LOG_MUTEX
        std::source_location location = std::source_location::current()
#endif
    ) const {
        return mut_guard{
            *this
#ifdef LOG_MUTEX
            ,
            location
#endif
        };
    }
};

// MARK: Excerpt from https://github.com/dragazo/rustex ends
