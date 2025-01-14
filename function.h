#pragma once

#include <cassert>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

class bad_function_call : public std::runtime_error {
public:
  explicit bad_function_call(const char* str) noexcept
      : std::runtime_error(str) {}
};

namespace function_impl {

using container_t = std::aligned_storage_t<sizeof(void*), alignof(void*)>;

template <typename T>
static constexpr bool fits_small = (sizeof(T) <= sizeof(container_t) &&
                                    alignof(void*) % alignof(T) == 0 &&
                                    std::is_nothrow_move_constructible_v<T> &&
                                    std::is_nothrow_move_assignable_v<T>);

template <typename R, typename... Args>
struct storage;

template <typename R, typename... Args>
struct type_descriptor {
  using storage_t = storage<R, Args...>;

  void (*copy)(storage_t*, storage_t const*);
  void (*move)(storage_t*, storage_t*) noexcept;
  R (*invoke)(storage_t*, Args...);
  void (*destroy)(storage_t*) noexcept;

  static type_descriptor<R, Args...> const*
  get_empty_func_descriptor() noexcept {
    constexpr static type_descriptor<R, Args...> result = {
        /* copy */
        [](storage_t* dst, storage_t const* src) {
          // Invariant: src & dst have empty descriptor
          assert(dst->desc == get_empty_func_descriptor());
          assert(src->desc == get_empty_func_descriptor());
        },
        /* move */
        [](storage_t* dst, storage_t* src) noexcept {
          // Invariant: src & dst have empty descriptor
          assert(dst->desc == get_empty_func_descriptor());
          assert(src->desc == get_empty_func_descriptor());
        },
        /* invoke */
        [](storage_t*, Args...) -> R {
          throw bad_function_call{"empty function ivocation"};
        },
        /* destroy */
        [](storage_t*) noexcept { /* noop */ }};

    return &result;
  }

  template <typename T>
  static type_descriptor<R, Args...> const* get_descriptor() {
    static constexpr type_descriptor<R, Args...> descriptor = {
        /* copy */
        [](storage_t* dst, storage_t const* src) {
          // Pre: dst has empty descriptor
          assert(dst->desc == get_empty_func_descriptor());
          if constexpr (fits_small<T>) {
            new (&dst->small) T(*src->template get<T>());
          } else {
            dst->set(new T(*src->template get<T>()));
          }
          dst->desc = src->desc;
        },
        /* move */
        [](storage_t* dst, storage_t* src) noexcept {
          // Pre: dst has empty descriptor
          // Post: src has empty descriptor
          assert(dst->desc == get_empty_func_descriptor());
          if constexpr (fits_small<T>) {
            new (&dst->small) T(std::move(*src->template get<T>()));
            src->desc->destroy(src);
          } else {
            dst->set((void*)src->template get<T>());
          }
          dst->desc = src->desc;
          src->desc = get_empty_func_descriptor();
          assert(src->desc == get_empty_func_descriptor());
        },
        /* invoke */
        [](storage_t* dst, Args... args) -> R {
          return (*(dst->template get<T>()))(std::forward<Args>(args)...);
        },
        /* destroy */
        [](storage_t* dst) noexcept {
          if constexpr (fits_small<T>) {
            dst->template get<T>()->~T();
          } else {
            delete dst->template get<T>();
          }
        }};

    return &descriptor;
  }

  template <typename T>
  static void init(storage_t& storage, T&& func) {
    storage.desc = get_descriptor<T>();
    if constexpr (fits_small<T>) {
      new (&storage.small) T(std::forward<T>(func));
    } else {
      storage.set(new T(std::forward<T>(func)));
    }
  }
};

template <typename R, typename... Args>
struct storage {

  storage() : desc{type_descriptor<R, Args...>::get_empty_func_descriptor()} {}

  template <typename T>
  T* get() {
    if constexpr (fits_small<T>) {
      return reinterpret_cast<T*>(&small);
    } else {
      return *reinterpret_cast<T**>(&small);
    }
  }

  template <typename T>
  T const* get() const {
    if constexpr (fits_small<T>) {
      return reinterpret_cast<T const*>(&small);
    } else {
      return *reinterpret_cast<T* const*>(&small);
    }
  }

  void set(void* t) {
    new (&small)(void*)(t);
  }

  void swap(storage& other) {
    storage tmp;

    this->desc->move(&tmp, this);
    other.desc->move(this, &other);
    tmp.desc->move(&other, &tmp);
  }

  ~storage() {
    desc->destroy(this);
  }

  type_descriptor<R, Args...> const* desc{nullptr};
  container_t small;
};
} // namespace function_impl

template <typename T>
struct function;

template <typename R, typename... Args>
struct function<R(Args...)> {
  function() = default;

  function(function const& other) : function() {
    other.storage.desc->copy(&storage, &other.storage);
  }

  function(function&& other) noexcept : function() {
    other.storage.desc->move(&storage, &other.storage);
  }

  function& operator=(function const& other) {
    if (this == &other) {
      return *this;
    }
    function tmp(other);
    swap(tmp);
    return *this;
  }

  function& operator=(function&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    function tmp(std::move(other));
    swap(tmp);
    return *this;
  }

  template <typename F>
  function(F f) {
    desc_t::template init<F>(storage, std::move(f));
  }

  R apply(Args... args) {
    return storage.desc->invoke(&storage, std::forward<Args>(args)...);
  }

  template <typename F>
  F* target() noexcept {
    return const_cast<F*>(std::as_const(*this).template target<F>());
  }

  template <typename F>
  F const* target() const noexcept {
    if (storage.desc == desc_t::template get_descriptor<F>()) {
      return storage.template get<F>();
    } else {
      return nullptr;
    }
  }

  R operator()(Args... args) {
    return apply(std::forward<Args>(args)...);
  }

  explicit operator bool() const noexcept {
    return storage.desc != desc_t::get_empty_func_descriptor();
  }

  ~function() = default;

  void swap(function& other) noexcept {
    storage.swap(other.storage);
  }

private:
  // This declaration is solely for shortenning some expressions
  using desc_t = function_impl::type_descriptor<R, Args...>;

  function_impl::storage<R, Args...> storage;
};
