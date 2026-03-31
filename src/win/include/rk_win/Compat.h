#pragma once

#if __has_include(<filesystem>)
#include <filesystem>
#endif

#if __has_include(<optional>)
#include <optional>
#endif

#if defined(__cpp_lib_filesystem)
#define RK_WIN_HAS_FILESYSTEM 1
#else
#define RK_WIN_HAS_FILESYSTEM 0
#endif

#if !RK_WIN_HAS_FILESYSTEM
#ifndef RK_WIN_STUB_FILESYSTEM_PATH_DEFINED
#define RK_WIN_STUB_FILESYSTEM_PATH_DEFINED 1
namespace std {
namespace filesystem {
struct path {
    path() = default;
    path(const char*) {}
    path(const wchar_t*) {}
    path& operator=(const char*) { return *this; }
    path& operator=(const wchar_t*) { return *this; }
};
}
}
#endif
#endif

#if defined(__cpp_lib_optional)
#define RK_WIN_HAS_OPTIONAL 1
#else
#define RK_WIN_HAS_OPTIONAL 0
#endif

#if !RK_WIN_HAS_OPTIONAL
#include <new>
#include <type_traits>
#include <utility>
#endif

namespace rk_win {

#if RK_WIN_HAS_OPTIONAL
template <class T>
using Optional = std::optional<T>;
using NulloptT = std::nullopt_t;
constexpr std::nullopt_t nullopt = std::nullopt;
#else
struct NulloptT {
    explicit constexpr NulloptT(int) {}
};

constexpr NulloptT nullopt{0};

template <class T>
class Optional {
public:
    Optional() = default;
    Optional(NulloptT) {}

    Optional(const T& v) { emplace(v); }
    Optional(T&& v) { emplace(std::move(v)); }

    Optional(const Optional& o) {
        if (o.has_) emplace(*o);
    }

    Optional(Optional&& o) {
        if (o.has_) emplace(std::move(*o));
    }

    Optional& operator=(NulloptT) {
        reset();
        return *this;
    }

    Optional& operator=(const Optional& o) {
        if (this == &o) return *this;
        if (o.has_) {
            if (has_) {
                **this = *o;
            } else {
                emplace(*o);
            }
        } else {
            reset();
        }
        return *this;
    }

    Optional& operator=(Optional&& o) {
        if (this == &o) return *this;
        if (o.has_) {
            if (has_) {
                **this = std::move(*o);
            } else {
                emplace(std::move(*o));
            }
        } else {
            reset();
        }
        return *this;
    }

    ~Optional() { reset(); }

    bool has_value() const { return has_; }
    explicit operator bool() const { return has_; }

    T& value() { return **this; }
    const T& value() const { return **this; }

    T& operator*() { return *ptr(); }
    const T& operator*() const { return *ptr(); }

    T* operator->() { return ptr(); }
    const T* operator->() const { return ptr(); }

    void reset() {
        if (!has_) return;
        ptr()->~T();
        has_ = false;
    }

    template <class... Args>
    T& emplace(Args&&... args) {
        reset();
        new (&storage_) T(std::forward<Args>(args)...);
        has_ = true;
        return *ptr();
    }

private:
    T* ptr() { return reinterpret_cast<T*>(&storage_); }
    const T* ptr() const { return reinterpret_cast<const T*>(&storage_); }

    bool has_ = false;
    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_{};
};
#endif

}  // namespace rk_win
