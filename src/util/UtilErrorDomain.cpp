/**
 * @file      UtilErrorDomain.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Singleton of the util error domain.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 */
#include "src/util/UtilErrorDomain.hpp"

namespace ids
{
namespace util
{

const ara::core::ErrorDomain &GetUtilErrorDomain() noexcept
{
    static const UtilErrorDomain domain;
    return domain;
}

} // namespace util
} // namespace ids
