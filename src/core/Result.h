#pragma once

#include <optional>
#include <utility>
#include <variant>

namespace OpenChat {

template<typename T, typename E>
class Result final
{
public:
    static Result success(T value)
    {
        return Result(ValueTag{}, std::move(value));
    }

    static Result failure(E error)
    {
        return Result(ErrorTag{}, std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return m_storage.index() == 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return hasValue();
    }

    [[nodiscard]] T &value() &
    {
        return std::get<0>(m_storage);
    }

    [[nodiscard]] const T &value() const &
    {
        return std::get<0>(m_storage);
    }

    [[nodiscard]] T &&value() &&
    {
        return std::get<0>(std::move(m_storage));
    }

    [[nodiscard]] E &error() &
    {
        return std::get<1>(m_storage);
    }

    [[nodiscard]] const E &error() const &
    {
        return std::get<1>(m_storage);
    }

private:
    struct ValueTag final { };
    struct ErrorTag final { };

    Result(ValueTag, T value)
        : m_storage(std::in_place_index<0>, std::move(value))
    {
    }

    Result(ErrorTag, E error)
        : m_storage(std::in_place_index<1>, std::move(error))
    {
    }

    std::variant<T, E> m_storage;
};

template<typename E>
class Result<void, E> final
{
public:
    static Result success()
    {
        return Result();
    }

    static Result failure(E error)
    {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return !m_error.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return hasValue();
    }

    [[nodiscard]] E &error() &
    {
        return m_error.value();
    }

    [[nodiscard]] const E &error() const &
    {
        return m_error.value();
    }

private:
    Result() = default;

    explicit Result(E error)
        : m_error(std::move(error))
    {
    }

    std::optional<E> m_error;
};

} // namespace OpenChat
