/**
 * @file      Optional.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Value that may be absent, as specified by ara::core.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. See Span.hpp for why this directory
 *            exists.
 *
 * @details   Use this when absence is a NORMAL outcome, not a failure: the end of a packet stream,
 *            or a packet that produces no feature vector. When the operation failed, use
 *            ara::core::Result instead, which carries an error code saying why.
 *
 * @details   Deviations from the specification, all deliberate:
 *            - the member names mirror @c std::optional, in lower case, which is what the
 *              specification does for types it borrows from the standard library. This is why they
 *              do not follow the PascalCase rule of the C++ code standard;
 *            - @c ara::core::nullopt and the @c in_place construction tags are not implemented; an
 *              absent value is the default-constructed state.
 */
#pragma once

#include <new>
#include <type_traits>
#include <utility>

/** @addtogroup AraCoreOptional ara::core::Optional
 *  @ingroup AraCore
 *  @{
 */

namespace ara
{
namespace core
{

/**
 * @brief Holds either a value of type @p T or nothing.
 * @tparam T Contained type, which need not be default constructible.
 * @details The value is kept in aligned storage inside the object, so there is no allocation.
 */
template <typename T>
class Optional final
{
  public:
    using value_type = T; ///< Contained type.

    /**
     * @brief Construct without a value.
     * @return None.
     */
    Optional() noexcept : m_storage(), m_hasValue(false)
    {
    }

    /**
     * @brief Construct holding a copy of a value.
     * @param[in] value Value to hold.
     * @return None.
     */
    Optional(const T &value) : m_storage(), m_hasValue(false)
    {
        Construct(value);
    }

    /**
     * @brief Construct holding a moved value.
     * @param[in,out] value Value to move from.
     * @return None.
     */
    Optional(T &&value) : m_storage(), m_hasValue(false)
    {
        Construct(std::move(value));
    }

    /**
     * @brief Copy constructor.
     * @param[in] other Object to copy.
     * @return None.
     */
    Optional(const Optional &other) : m_storage(), m_hasValue(false)
    {
        if (other.m_hasValue)
        {
            Construct(*other.Ptr());
        }
        else
        {
            // Nothing to copy: the new object stays empty.
        }
    }

    /**
     * @brief Move constructor.
     * @param[in,out] other Object to move from, which is left empty.
     * @return None.
     */
    Optional(Optional &&other) : m_storage(), m_hasValue(false)
    {
        if (other.m_hasValue)
        {
            Construct(std::move(*other.Ptr()));
            other.reset();
        }
        else
        {
            // Nothing to move: the new object stays empty.
        }
    }

    /**
     * @brief Copy assignment.
     * @param[in] other Object to copy.
     * @return Reference to this object.
     */
    Optional &operator=(const Optional &other)
    {
        if (this != &other)
        {
            reset();
            if (other.m_hasValue)
            {
                Construct(*other.Ptr());
            }
            else
            {
                // Source is empty, so this object stays empty.
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
     * @param[in,out] other Object to move from, which is left empty.
     * @return Reference to this object.
     */
    Optional &operator=(Optional &&other)
    {
        if (this != &other)
        {
            reset();
            if (other.m_hasValue)
            {
                Construct(std::move(*other.Ptr()));
                other.reset();
            }
            else
            {
                // Source is empty, so this object stays empty.
            }
        }
        else
        {
            // Self assignment: nothing to do.
        }
        return *this;
    }

    /**
     * @brief Destroy the contained value, if any.
     * @return None.
     */
    ~Optional()
    {
        reset();
    }

    /**
     * @brief Whether a value is held.
     * @return Whether the object holds a value.
     * @retval true  A value is held and may be read.
     * @retval false The object is empty.
     */
    bool has_value() const noexcept
    {
        return m_hasValue;
    }

    /**
     * @brief Whether a value is held, for use in a condition.
     * @return Same as has_value().
     */
    explicit operator bool() const noexcept
    {
        return m_hasValue;
    }

    /**
     * @brief Contained value.
     * @return Reference to the contained value.
     * @warning Reading this on an empty object is undefined behaviour. Check has_value() first.
     */
    const T &value() const
    {
        return *Ptr();
    }

    /**
     * @brief Contained value.
     * @return Reference to the contained value.
     * @warning Reading this on an empty object is undefined behaviour. Check has_value() first.
     */
    T &value()
    {
        return *Ptr();
    }

    /**
     * @brief Contained value.
     * @return Reference to the contained value.
     * @warning Reading this on an empty object is undefined behaviour. Check has_value() first.
     */
    const T &operator*() const
    {
        return *Ptr();
    }

    /**
     * @brief Contained value, or a fallback when empty.
     * @param[in] fallback Value returned when the object is empty.
     * @return The contained value, or @p fallback.
     */
    T value_or(const T &fallback) const
    {
        return (m_hasValue ? *Ptr() : fallback);
    }

    /**
     * @brief Destroy the contained value and leave the object empty.
     * @return None.
     */
    void reset() noexcept
    {
        if (m_hasValue)
        {
            Ptr()->~T();
            m_hasValue = false;
        }
        else
        {
            // Already empty: nothing to destroy.
        }
    }

  private:
    /**
     * @brief Construct the contained value in place.
     * @tparam U Type of the initialiser.
     * @param[in,out] value Initialiser forwarded to the constructor of @p T.
     * @return None.
     */
    template <typename U>
    void Construct(U &&value)
    {
        new (static_cast<void *>(&m_storage)) T(std::forward<U>(value));
        m_hasValue = true;
    }

    /**
     * @brief Typed pointer into the internal storage.
     * @return Pointer to the storage, valid to dereference only while has_value() is true.
     */
    T *Ptr() noexcept
    {
        return static_cast<T *>(static_cast<void *>(&m_storage));
    }

    /**
     * @brief Typed pointer into the internal storage.
     * @return Pointer to the storage, valid to dereference only while has_value() is true.
     */
    const T *Ptr() const noexcept
    {
        return static_cast<const T *>(static_cast<const void *>(&m_storage));
    }

    typename std::aligned_storage<sizeof(T), alignof(T)>::type m_storage;  ///< Storage for the contained value.
    bool                                                       m_hasValue; ///< Whether m_storage holds a live value.
};

} // namespace core
} // namespace ara

/** @}*/ // End of AraCoreOptional
