/**
 * @file      IdsSensorErrorDomain.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Singleton of the ids_sensor error domain.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 */
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"

namespace ids
{

const ara::core::ErrorDomain &GetIdsSensorErrorDomain() noexcept
{
    static const IdsSensorErrorDomain domain;
    return domain;
}

} // namespace ids
