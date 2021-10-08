#pragma once

template <typename F>
struct function;

template <typename R, typename... Args>
struct function<R (Args...)>
{
    function() noexcept;

    function(function const& other);
    function(function&& other) noexcept;

    template <typename T>
    function(T val);

    function& operator=(function const& rhs);
    function& operator=(function&& rhs) noexcept;

    ~function();

    explicit operator bool() const noexcept;

    R operator()(Args... args) const;

    template <typename T>
    T* target() noexcept;

    template <typename T>
    T const* target() const noexcept;
};
