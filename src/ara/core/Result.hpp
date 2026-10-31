/**
 * @file      Result.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     A value or an error, as specified by ara::core.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. See Span.hpp for why this directory
 *            exists.
 *
 * @details   This is the error channel of the Adaptive Platform, and it replaces both exceptions
 *            and the string-message @c outcome<T> of the C++ code standard. Use it when an
 *            operation can FAIL and the caller needs to know why. When a value is merely absent
 *            and that is normal, use ara::core::Optional instead.
 *
 * @details   Deviations from the specification, all deliberate: the monadic helpers
 *            (@c AndThen, @c OrElse, @c Map), @c ValueOrThrow and @c CheckError are not
 *            implemented, and neither is the @c Result<void, E> specialisation, because nothing
 *            in the bench needs them yet.
 */
#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "src/ara/core/ErrorCode.hpp"

/** @addtogroup AraCoreResult ara::core::Result
 *  @ingroup AraCore
 *  @{
 */

namespace ara
{
namespace core
{

/**
 * @brief Holds either a value of type @p T or an error of type @p E, never both and never neither.
 * @tparam T Value type, which need not be default constructible.
 * @tparam E Error type, ara::core::ErrorCode by default.
 * @details Both alternatives live in aligned storage inside the object, so there is no allocation.
 */
template <typename T, typename E = ErrorCode>
class Result final
{
  public:
    using value_type = T; ///< Type held on success.
    using error_type = E; ///< Type held on failure.

    /**
     * @brief Construct holding a copy of a value.
     * @param[in] value Value to hold.
     * @return None.
     */
    Result(const T &value) : m_hasValue(false)
    {
        ConstructValue(value);
    }

    /**
     * @brief Construct holding a moved value.
     * @param[in,out] value Value to move from.
     * @return None.
     */
    Result(T &&value) : m_hasValue(false)
    {
        ConstructValue(std::move(value));
    }

    /**
     * @brief Construct holding an error.
     * @param[in] error Error to hold.
     * @return None.
     */
    explicit Result(const E &error) : m_hasValue(false)
    {
        ConstructError(error);
    }

    /**
     * @brief Build a successful result.
     * @param[in] value Value to hold.
     * @return Result holding @p value.
     */
    static Result FromValue(const T &value)
    {
        return Result(value);
    }

    /**
     * @brief Build a successful result from a moved value.
     * @param[in,out] value Value to move from.
     * @return Result holding the moved value.
     */
    static Result FromValue(T &&value)
    {
        return Result(std::move(value));
    }

    /**
     * @brief Build a failed result.
     * @param[in] error Error to hold.
     * @return Result holding @p error.
     */
    static Result FromError(const E &error)
    {
        return Result(error);
    }

    /**
     * @brief Copy constructor.
     * @param[in] other Object to copy.
     * @return None.
     */
    Result(const Result &other) : m_hasValue(false)
    {
        if (other.m_hasValue)
        {
            ConstructValue(*other.ValuePtr());
        }
        else
        {
            ConstructError(*other.ErrorPtr());
        }
    }

    /**
     * @brief Move constructor.
     * @param[in,out] other Object to move from, which keeps holding the same alternative.
     * @return None.
     */
    Result(Result &&other) : m_hasValue(false)
    {
        if (other.m_hasValue)
        {
            ConstructValue(std::move(*other.ValuePtr()));
        }
        else
        {
            ConstructError(std::move(*other.ErrorPtr()));
        }
    }

    /**
     * @brief Copy assignment.
     * @param[in] other Object to copy.
     * @return Reference to this object.
     */
    Result &operator=(const Result &other)
    {
        if (this != &other)
        {
            Destroy();
            if (other.m_hasValue)
            {
                ConstructValue(*other.ValuePtr());
            }
            else
            {
                ConstructError(*other.ErrorPtr());
            }
        }
        else
        {
            // Self assignment: nothing to do.
        }
        return *this;
    }

    /**
     * @brief Move assignment.
     * @param[in,out] other Object to move from.
     * @return Reference to this object.
     */
    Result &operator=(Result &&other)
    {
        if (this != &other)
        {
            Destroy();
            if (other.m_hasValue)
            {
                ConstructValue(std::move(*other.ValuePtr()));
            }
            else
            {
                ConstructError(std::move(*other.ErrorPtr()));
            }
        }
        else
        {
            // Self assignment: nothing to do.
        }
        return *this;
    }

    /**
     * @brief Destroy whichever alternative is held.
     * @return None.
     */
    ~Result()
    {
        Destroy();
    }

    /**
     * @brief Whether the operation succeeded.
     * @return Whether a value is held.
     * @retval true  A value is held and Value() may be read.
     * @retval false An error is held and Error() may be read.
     */
    bool HasValue() const noexcept
    {
        return m_hasValue;
    }

    /**
     * @brief Whether the operation succeeded, for use in a condition.
     * @return Same as HasValue().
     */
    explicit operator bool() const noexcept
    {
        return m_hasValue;
    }

    /**
     * @brief Held value.
     * @return Reference to the value.
     * @warning Reading this while HasValue() is false is undefined behaviour. Check first.
     */
    const T &Value() const
    {
        return *ValuePtr();
    }

    /**
     * @brief Held value.
     * @return Reference to the value.
     * @warning Reading this while HasValue() is false is undefined behaviour. Check first.
     */
    T &Value()
    {
        return *ValuePtr();
    }

    /**
     * @brief Held value.
     * @return Reference to the value.
     * @warning Reading this while HasValue() is false is undefined behaviour. Check first.
     */
    const T &operator*() const
    {
        return *ValuePtr();
    }

    /**
     * @brief Held error.
     * @return Reference to the error.
     * @warning Reading this while HasValue() is true is undefined behaviour. Check first.
     */
    const E &Error() const
    {
        return *ErrorPtr();
    }

    /**
     * @brief Held value, or a fallback on failure.
     * @param[in] fallback Value returned when an error is held.
     * @return The held value, or @p fallback.
     */
    T ValueOr(const T &fallback) const
    {
        return (m_hasValue ? *ValuePtr() : fallback);
    }

  private:
    /**
     * @brief Construct the value alternative in place.
     * @tparam U Type of the initialiser.
     * @param[in,out] value Initialiser forwarded to the constructor of @p T.
     * @return None.
     */
    template <typename U>
    void ConstructValue(U &&value)
    {
        new (static_cast<void *>(&m_storage)) T(std::forward<U>(value));
        m_hasValue = true;
    }

    /**
     * @brief Construct the error alternative in place.
     * @tparam U Type of the initialiser.
     * @param[in,out] error Initialiser forwarded to the constructor of @p E.
     * @return None.
     */
    template <typename U>
    void ConstructError(U &&error)
    {
        new (static_cast<void *>(&m_storage)) E(std::forward<U>(error));
        m_hasValue = false;
    }

    /**
     * @brief Destroy whichever alternative is held.
     * @return None.
     */
    void Destroy() noexcept
    {
        if (m_hasValue)
        {
            ValuePtr()->~T();
        }
        else
        {
            ErrorPtr()->~E();
        }
    }

    /**
     * @brief Typed pointer to the value alternative.
     * @return Pointer into the internal storage.
     */
    T *ValuePtr() noexcept
    {
        return static_cast<T *>(static_cast<void *>(&m_storage));
    }

    /**
     * @brief Typed pointer to the value alternative.
     * @return Pointer into the internal storage.
     */
    const T *ValuePtr() const noexcept
    {
        return static_cast<const T *>(static_cast<const void *>(&m_storage));
    }

    /**
     * @brief Typed pointer to the error alternative.
     * @return Pointer into the internal storage.
     */
    E *ErrorPtr() noexcept
    {
        return static_cast<E *>(static_cast<void *>(&m_storage));
    }

    /**
     * @brief Typed pointer to the error alternative.
     * @return Pointer into the internal storage.
     */
    const E *ErrorPtr() const noexcept
    {
        return static_cast<const E *>(static_cast<const void *>(&m_storage));
    }

    /// Storage large enough and aligned for either alternative.
    typename std::aligned_storage<((sizeof(T) > sizeof(E)) ? sizeof(T) : sizeof(E)),
                                  ((alignof(T) > alignof(E)) ? alignof(T) : alignof(E))>::type m_storage;

    bool m_hasValue; ///< Whether m_storage holds a value rather than an error.
};

} // namespace core
} // namespace ara

/** @}*/ // End of AraCoreResult
