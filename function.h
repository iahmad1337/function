#pragma once

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

  template <typename T>
  static constexpr bool
      fits_small = (sizeof(T) <= sizeof(void*) &&
                            alignof(void*) % alignof(T) == 0 &&
                            std::is_nothrow_move_constructible<T>::value);

  template <typename R, typename... Args>
  struct storage;

  template <typename R, typename... Args>
  struct type_descriptor {
    using storage_t = function_impl::storage<R, Args...>;

    void (*copy)(storage_t*, storage_t const*);
    void (*move)(storage_t*, storage_t*) noexcept;
    R (*invoke)(storage_t*, Args...);
    void (*destroy)(storage_t*) noexcept;

    static type_descriptor<R, Args...> const*
    get_empty_func_descriptor() noexcept {
      constexpr static type_descriptor<R, Args...> result = {
          /* copy */
          [](storage_t* dst, storage_t const*) {
            dst->desc = get_empty_func_descriptor();
          },
          /* move */
          [](storage_t* dst, storage_t*) noexcept {
            dst->desc = get_empty_func_descriptor();
          },
          /* invoke */
          [](storage_t*, Args...) -> R {
            throw bad_function_call{"empty function ivocation"};
          },
          /* destroy */
          [](storage_t*) noexcept { /* noop */ }
      };

      return &result;
    }

    template<typename T>
    static type_descriptor<R, Args...> const* get_descriptor() {
      static constexpr type_descriptor<R, Args...> descriptor = {
          /* copy */
          [](storage_t* dst, storage_t const* src) {
            dst->desc = src->desc;
            if constexpr (fits_small<T>) {
              new (&dst->small) T(*src->template get<T>());
            } else {
              dst->set(new T(*src->template get<T>()));
            }
          },
          /* move */
          [](storage_t* dst, storage_t* src) noexcept {
            dst->desc = src->desc;
            if constexpr (fits_small<T>) {
              new (&dst->small) T(std::move(*src->template get<T>()));
            } else {
              dst->set((void*)src->template get<T>());
              src->desc = get_empty_func_descriptor();
            }
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

    template<typename T>
    static void init(storage<R, Args...>& storage, T&& func) {
      if constexpr (fits_small<T>) {
        new (&storage.small) T(std::forward<T>(func));
      } else {
        storage.set(new T(std::forward<T>(func)));
      }
    }
  };

  template <typename R, typename... Args>
  struct storage {

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
      using std::swap;
      swap(desc, other.desc);
      swap(small, other.small);
    }

    type_descriptor<R, Args...> const* desc{nullptr};
    std::aligned_storage_t<sizeof(void*), alignof(void*)> small;
  };
}  // namespace function_impl

template <typename T>
struct function;

template <typename R, typename... Args>
struct function<R(Args...)> {
  function() {
    storage.desc = desc_t::get_empty_func_descriptor();
  }

  function(function const& other) {
    storage.desc = other.storage.desc;
    storage.desc->copy(&storage, &other.storage);
  }

  function(function&& other) noexcept {
    storage.desc = other.storage.desc;
    storage.desc->move(&storage, &other.storage);
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
    storage.desc = desc_t::template get_descriptor<F>();
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

  ~function() {
    storage.desc->destroy(&storage);
  }

  void swap(function& other) noexcept {
    storage.swap(other.storage);
  }

private:
  // This declaration is solely for shotenning some expressions
  using desc_t = function_impl::type_descriptor<R, Args...>;

  function_impl::storage<R, Args...> storage;
};
