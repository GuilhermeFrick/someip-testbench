/**
 * @file      Normalizer.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Min-max normalisation and the loading of the training bounds.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   Every arithmetic decision here is taken from @c extract_all() in
 *            @c src/pipeline.py of the reproduction repository, which is the code that normalised
 *            the features the model was trained on. The two that look like omissions, the absence
 *            of clipping and the acceptance of a degenerate feature, are explained in the header
 *            and are reproduced on purpose.
 */
#include "src/ids_sensor/Normalizer.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace ids
{

Normalizer::Normalizer(const BoundsVec &xmin, const BoundsVec &xmax) : m_xmin(xmin), m_xmax(xmax), m_range()
{
    for (std::size_t i = 0U; i < kNumFeatures; ++i)
    {
        /* The reference: rng = where(xmax > xmin, xmax - xmin, 1.0). A feature that
         * was constant during training is divided by ONE, not rejected and not
         * divided by zero. Strictly greater, as in the reference, so equality also
         * lands on the divisor of one. */
        if (m_xmax[i] > m_xmin[i])
        {
            m_range[i] = (m_xmax[i] - m_xmin[i]);
        }
        else
        {
            m_range[i] = 1.0;
        }
    }
}

Normalizer Normalizer::FromBounds(const BoundsVec &xmin, const BoundsVec &xmax)
{
    return Normalizer(xmin, xmax);
}

ara::core::Result<Normalizer> Normalizer::Create(const std::string &path)
{
    using Res = ara::core::Result<Normalizer>;

    errno                 = 0;
    std::FILE *const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        /* Same distinction as ::ids::ByteModels::Create: "not deployed" and "deployed
         * and unreadable" are different problems for whoever has to fix it. */
        if (errno == ENOENT)
        {
            return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactNotFound));
        }
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactUnreadable, errno));
    }
    else
    {
        /* File opened. */
    }

    std::uint32_t declaradas = 0U;
    if (std::fread(&declaradas, sizeof(declaradas), 1U, file) != 1U)
    {
        static_cast<void>(std::fclose(file));
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* The count was read; whether it is the right one is checked next. */
    }

    /* The count is CHECKED against the contract rather than trusted. An artefact
     * exported for a different feature set would otherwise be read as if it were
     * this one, and the trees would see sixteen values that mean something else. */
    if (static_cast<std::size_t>(declaradas) != kNumFeatures)
    {
        static_cast<void>(std::fclose(file));
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* Sixteen features, as this build expects. */
    }

    /* Read straight into the arrays, as ::ids::ByteModels::Create does with its
     * tables: the artefact holds IEEE-754 doubles in the platform byte order, which
     * is what the exporter writes with struct.pack("<d"). */
    BoundsVec  xmin = BoundsVec();
    BoundsVec  xmax = BoundsVec();
    const bool bom  = ((std::fread(xmin.data(), sizeof(double), kNumFeatures, file) == kNumFeatures) &&
                      (std::fread(xmax.data(), sizeof(double), kNumFeatures, file) == kNumFeatures));
    static_cast<void>(std::fclose(file));

    if (!bom)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* Both vectors are complete. */
    }

    return Res::FromValue(Normalizer(xmin, xmax));
}

FeatureVec Normalizer::Apply(const FeatureVec &x) const
{
    FeatureVec out = FeatureVec();
    for (std::size_t i = 0U; i < kNumFeatures; ++i)
    {
        /* In double, then cast: the reference subtracts and divides in float64 and
         * only the result becomes float32. No clipping, by design; see the header. */
        const double bruto      = static_cast<double>(x[i]);
        const double normalizado = ((bruto - m_xmin[i]) / m_range[i]);
        out[i]                   = static_cast<float>(normalizado);
    }
    return out;
}

} // namespace ids
