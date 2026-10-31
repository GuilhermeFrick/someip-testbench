/**
 * @file      Span.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Non-owning view over a contiguous sequence, as specified by ara::core.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   BENCH SUBSET of the AUTOSAR Adaptive Platform. There is no off-the-shelf open
 *            @c ara::core, and vsomeip only covers the communication layer, so this directory
 *            implements the small part of the specification that the bench actually uses. It keeps
 *            the specified names and signatures, so that porting onto a real Adaptive Platform
 *            stack is a matter of deleting this subset.
 *
 * @details   Deviations from the specification, all deliberate:
 *            - dynamic extent only. The static extent form and @c ara::core::dynamic_extent are
 *              not implemented, because nothing here needs a compile-time size;
 *            - the member names mirror @c std::span, in lower case, which is what the
 *              specification itself does for types it borrows from the standard library. This is
 *              why they do not follow the PascalCase rule of the C++ code standard.
 */
#pragma once

#include <cstddef>
#include <type_traits>

/** @addtogroup AraCoreSpan ara::core::Span
 *  @ingroup AraCore
 *  @{
 */

namespace ara
{
namespace core
{

/**
 * @brief View over a contiguous sequence of objects.
 * @tparam T Element type, including any @c const qualification.
 * @warning Non-owning. The view must not outlive the sequence it refers to.
 */
template <typename T>
class Span final
{
  public:
    using element_type    = T;                                       ///< Element type, qualifiers included.
    using value_type      = typename std::remove_cv<T>::type;        ///< Element type without qualifiers.
    using size_type       = std::size_t;                             ///< Size and index type.
    using difference_type = std::ptrdiff_t;                          ///< Distance between two iterators.
    using pointer         = T *;                                     ///< Pointer to an element.
    using reference       = T &;                                     ///< Reference to an element.
    using iterator        = T *;                                     ///< Iterator over the elements.
    using const_iterator  = const T *;                               ///< Iterator that cannot modify elements.

    /**
     * @brief Construct an empty view.
     * @return None.
     */
    constexpr Span() noexcept : m_data(nullptr), m_size(0U)
    {
    }

    /**
     * @brief Construct from a pointer and a count.
     * @param[in] ptr   First element of the sequence, or nullptr when @p count is zero.
     * @param[in] count Number of elements in the sequence.
     * @return None.
     */
    constexpr Span(pointer ptr, size_type count) noexcept : m_data(ptr), m_size(count)
    {
    }

    /**
     * @brief Construct from a built-in array.
     * @tparam N Number of elements in the array.
     * @param[in] arr Array to view.
     * @return None.
     */
    template <std::size_t N>
    constexpr explicit Span(T (&arr)[N]) noexcept : m_data(&arr[0]), m_size(N)
    {
    }

    /**
     * @brief First element.
     * @return Pointer to the first element, or nullptr when the view is empty.
     */
    constexpr pointer data() const noexcept
    {
        return m_data;
    }

    /**
     * @brief Number of elements.
     * @return Element count.
     */
    constexpr size_type size() const noexcept
    {
        return m_size;
    }

    /**
     * @brief Size in bytes.
     * @return Element count multiplied by the element size.
     */
    constexpr size_type size_bytes() const noexcept
    {
        return m_size * sizeof(T);
    }

    /**
     * @brief Whether the view holds no element.
     * @return Whether size() is zero.
     * @retval true  The view is empty.
     * @retval false The view holds at least one element.
     */
    constexpr bool empty() const noexcept
    {
        return (m_size == 0U);
    }

    /**
     * @brief Element at a given index.
     * @param[in] index Zero-based index, which must be below size().
     * @return Reference to the element.
     * @warning The index is not checked. Calling this with @p index at or above size() is
     *          undefined behaviour.
     */
    constexpr reference operator[](size_type index) const
    {
        return m_data[index];
    }

    /**
     * @brief Iterator to the first element.
     * @return Begin iterator.
     */
    constexpr iterator begin() const noexcept
    {
        return m_data;
    }

    /**
     * @brief Iterator past the last element.
     * @return End iterator.
     */
    constexpr iterator end() const noexcept
    {
        return (m_data + m_size);
    }

    /**
     * @brief View over a sub-range.
     * @param[in] offset Index of the first element of the sub-range.
     * @param[in] count  Number of elements in the sub-range.
     * @return View over @p count elements starting at @p offset, or an empty view when @p offset
     *         is at or beyond size().
     */
    constexpr Span<T> subspan(size_type offset, size_type count) const noexcept
    {
        return ((offset >= m_size) ? Span<T>() : Span<T>(m_data + offset, ((count < (m_size - offset)) ? count : (m_size - offset))));
    }

  private:
    pointer   m_data; ///< First element, or nullptr when empty.
    size_type m_size; ///< Element count.
};

/**
 * @brief Build a view over a contiguous sequence, deducing the element type.
 * @tparam T Element type.
 * @param[in] ptr   First element of the sequence.
 * @param[in] count Number of elements.
 * @return View over the sequence.
 */
template <typename T>
constexpr Span<T> MakeSpan(T *ptr, std::size_t count) noexcept
{
    return Span<T>(ptr, count);
}

} // namespace core
} // namespace ara

/** @}*/ // End of AraCoreSpan
