/**
 * @file      Normalizer.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Min-max feature normalisation using the bounds recorded during TRAINING.
 * @version   2.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   The bounds are embedded data, and they must be the ones from training. Recomputing
 *            them at runtime would change the meaning of every feature and invalidate the model
 *            silently.
 */
#pragma once

#include <string>

#include "src/ara/core/Result.hpp"
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"
#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorNormalizer Feature normaliser
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Applies the training min-max bounds to a raw feature vector.
 */
class Normalizer final
{
  public:
    /**
     * @brief Load the xmin/xmax pair exported by the training pipeline.
     * @param[in] path Path to the exported bounds artefact.
     * @return A normaliser holding the loaded bounds, or an error describing why it could not be
     *         loaded.
     * @retval ::ids::IdsSensorErrc::ArtefactNotFound   No file exists at @p path.
     * @retval ::ids::IdsSensorErrc::ArtefactUnreadable The file exists but could not be read; the
     *                                                 support data carries the platform errno.
     * @retval ::ids::IdsSensorErrc::ArtefactCorrupted  The file does not hold two vectors of
     *                                                 ::ids::kNumFeatures values each.
     * @retval ::ids::IdsSensorErrc::InvalidConfiguration Some feature has xmax below or equal to
     *                                                 xmin, which would divide by zero or invert
     *                                                 the scale.
     * @details Create() is the only way to obtain a normaliser, so a loaded one always holds
     *          usable bounds and Apply() needs no guard.
     * @warning The artefact must come from the same training run as the model and the byte models.
     *          Mixing artefacts from different runs shifts the decision boundary without any error.
     */
    static ara::core::Result<Normalizer> Create(const std::string &path);

    /**
     * @brief Build from explicit bounds, bypassing the artefact file.
     * @param[in] xmin Per-feature lower bounds, as recorded during training.
     * @param[in] xmax Per-feature upper bounds, as recorded during training.
     * @return A normaliser holding the given bounds, or an error when they are unusable.
     * @retval ::ids::IdsSensorErrc::InvalidConfiguration Some feature has xmax below or equal to xmin.
     * @details Used by the tests, to drive the normaliser without an artefact on disk.
     */
    static ara::core::Result<Normalizer> Create(const FeatureVec &xmin, const FeatureVec &xmax);

    /**
     * @brief Normalise a raw feature vector.
     * @param[in] x Raw feature vector.
     * @return The vector with @c (x-xmin)/(xmax-xmin) applied to each feature.
     * @details Returns the normalised vector rather than writing through a reference, per rule
     *          A8-4-8. ::ids::FeatureVec is 64 bytes and is returned in place, so this costs
     *          nothing on the hot path.
     * @warning Mind the bounds: a feature with no natural ceiling can exceed the training xmax at
     *          runtime. Whatever is done here, saturate or let it through, MUST match the Python
     *          side, otherwise the decision boundary differs exactly on the extreme cases, which
     *          are the attacks.
     */
    FeatureVec Apply(const FeatureVec &x) const;

    /**
     * @brief Lower bounds in use.
     * @return Per-feature lower bounds.
     */
    const FeatureVec &Xmin() const noexcept
    {
        return m_xmin;
    }

    /**
     * @brief Upper bounds in use.
     * @return Per-feature upper bounds.
     */
    const FeatureVec &Xmax() const noexcept
    {
        return m_xmax;
    }

  private:
    /**
     * @brief Construct from already validated bounds.
     * @param[in] xmin Per-feature lower bounds.
     * @param[in] xmax Per-feature upper bounds.
     * @return None.
     * @details Private so that Create() is the only way in, which is what guarantees the bounds
     *          were validated.
     */
    Normalizer(const FeatureVec &xmin, const FeatureVec &xmax) : m_xmin(xmin), m_xmax(xmax)
    {
    }

    FeatureVec m_xmin; ///< Per-feature lower bounds from training.
    FeatureVec m_xmax; ///< Per-feature upper bounds from training.
};

} // namespace ids

/** @}*/ // End of IdsSensorNormalizer
