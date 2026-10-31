/**
 * @file      SlidingWindow.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Fixed-capacity sliding window: pushing past the capacity evicts the oldest item and
 *            hands it back.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   The extractor keeps two sliding windows, and before this component existed the
 *            behaviour was written INLINE TWICE inside ::ids::FeatureStreamer::Step, with its own
 *            trim loop each time. The C++ code standard already answers this: section 2 says
 *            reusable code becomes a component, and section 7 uses a @c SlidingWindow template as
 *            its worked example of a template class.
 *
 * @details   The design decision that earns the component its keep is Push() returning the EVICTED
 *            item rather than @c void. One of the two windows maintains a distinct-value count on
 *            top of itself, and it needs to know what left in order to decrement. With @c void it
 *            would have to inspect the window before pushing, which is exactly how the duplicated
 *            version worked and why the trim loop had to live at the call site.
 *
 * @details   Storage is a ring buffer over a vector reserved at construction, not a deque: once
 *            built, Push() never allocates. That matters because this sits on the per-packet path.
 *
 * @details   Items are kept BY VALUE, so @p T should be cheap to copy. Both current users store a
 *            64-bit payload hash.
 */
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "src/ara/core/Optional.hpp"
#include "src/ara/core/Result.hpp"
#include "src/util/UtilErrorDomain.hpp"

/** @defgroup Util util
 *  @brief Hardware-independent helpers.
 */

/** @addtogroup UtilSlidingWindow SlidingWindow
 *  @ingroup Util
 *  @{
 */

namespace ids
{
namespace util
{

/**
 * @brief Window over the last @c Capacity() items pushed into it.
 * @tparam T Item type, kept by value.
 */
template <typename T>
class SlidingWindow final
{
  public:
    /**
     * @brief Build a window of a given capacity.
     * @param[in] capacity How many items the window holds; must not be zero.
     * @return The window, or an error when @p capacity is zero.
     * @retval ::ids::util::UtilErrc::ZeroCapacity @p capacity is zero.
     * @details Reserves the storage here, so that Push() never allocates afterwards.
     */
    static ara::core::Result<SlidingWindow> Create(std::size_t capacity);

    /**
     * @brief Add an item, evicting the oldest when the window is already full.
     * @param[in] item Item to add.
     * @return The evicted item, or an empty Optional when the window was not full.
     * @details The returned item is what lets a caller maintain an aggregate over the window
     *          without inspecting it: a distinct-value count decrements on what left and
     *          increments on what arrived.
     */
    ara::core::Optional<T> Push(const T &item);

    /**
     * @brief Item at a position, counted from the OLDEST.
     * @param[in] index Position, which must be below Size().
     * @return Reference to the item; index 0 is the oldest still in the window.
     * @warning The index is not checked. Calling this with @p index at or above Size() is
     *          undefined behaviour.
     * @details Indexed access rather than iterators: the storage is a ring buffer, so a custom
     *          iterator would be more code than the two call sites justify. Section 9 of the code
     *          standard prefers a range-based @c for, and that preference assumes a container that
     *          offers one.
     */
    const T &At(std::size_t index) const;

    /**
     * @brief How many items the window currently holds.
     * @return Item count, never above Capacity().
     */
    std::size_t Size() const noexcept;

    /**
     * @brief How many items the window holds when full.
     * @return The capacity given at construction.
     */
    std::size_t Capacity() const noexcept;

    /**
     * @brief Whether the window holds no item.
     * @return Whether Size() is zero.
     * @retval true  Empty.
     * @retval false Holds at least one item.
     */
    bool Empty() const noexcept;

    /**
     * @brief Whether the next Push() will evict.
     * @return Whether Size() equals Capacity().
     * @retval true  Full; the next push evicts the oldest.
     * @retval false Room left.
     */
    bool Full() const noexcept;

    /**
     * @brief Drop every item, keeping the capacity and the reserved storage.
     * @return None.
     */
    void Clear() noexcept;

  private:
    /**
     * @brief Construct with an already validated capacity.
     * @param[in] capacity Item capacity.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    explicit SlidingWindow(std::size_t capacity);

    std::vector<T> m_buffer;   ///< Ring storage; grows to the capacity and stops.
    std::size_t    m_capacity; ///< Item capacity.
    std::size_t    m_oldest;   ///< Index into m_buffer of the oldest item.
    std::size_t    m_size;     ///< Items currently held.
};

template <typename T>
SlidingWindow<T>::SlidingWindow(std::size_t capacity) : m_buffer(), m_capacity(capacity), m_oldest(0U), m_size(0U)
{
    m_buffer.reserve(capacity);
}

template <typename T>
ara::core::Result<SlidingWindow<T>> SlidingWindow<T>::Create(std::size_t capacity)
{
    using Res = ara::core::Result<SlidingWindow<T>>;

    if (capacity == 0U)
    {
        return Res::FromError(ara::core::ErrorCode(UtilErrc::ZeroCapacity));
    }
    else
    {
        /* Usable capacity. */
    }
    return Res::FromValue(SlidingWindow<T>(capacity));
}

template <typename T>
ara::core::Optional<T> SlidingWindow<T>::Push(const T &item)
{
    ara::core::Optional<T> evicted;

    if (m_size < m_capacity)
    {
        /* Still filling: the storage grows up to the capacity it reserved, so this
         * does not allocate. */
        const std::size_t slot = ((m_oldest + m_size) % m_capacity);
        if (m_buffer.size() < m_capacity)
        {
            m_buffer.push_back(item);
        }
        else
        {
            m_buffer[slot] = item;
        }
        ++m_size;
    }
    else
    {
        /* Full: the oldest slot is the one being overwritten, so hand it back
         * before it is lost. */
        evicted             = ara::core::Optional<T>(m_buffer[m_oldest]);
        m_buffer[m_oldest]  = item;
        m_oldest            = ((m_oldest + 1U) % m_capacity);
    }

    return evicted;
}

template <typename T>
const T &SlidingWindow<T>::At(std::size_t index) const
{
    return m_buffer[(m_oldest + index) % m_capacity];
}

template <typename T>
std::size_t SlidingWindow<T>::Size() const noexcept
{
    return m_size;
}

template <typename T>
std::size_t SlidingWindow<T>::Capacity() const noexcept
{
    return m_capacity;
}

template <typename T>
bool SlidingWindow<T>::Empty() const noexcept
{
    return (m_size == 0U);
}

template <typename T>
bool SlidingWindow<T>::Full() const noexcept
{
    return (m_size == m_capacity);
}

template <typename T>
void SlidingWindow<T>::Clear() noexcept
{
    m_buffer.clear();
    m_oldest = 0U;
    m_size   = 0U;
}

} // namespace util
} // namespace ids

/** @}*/ // End of UtilSlidingWindow
