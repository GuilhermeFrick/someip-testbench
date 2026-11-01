/**
 * @file      CoreErrorDomain.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Singleton of the ara::core error domain.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 */
#include "src/ara/core/CoreErrorDomain.hpp"

namespace ara
{
namespace core
{

const ErrorDomain &GetCoreErrorDomain() noexcept
{
    static const CoreErrorDomain domain;
    return domain;
}

} // namespace core
} // namespace ara
