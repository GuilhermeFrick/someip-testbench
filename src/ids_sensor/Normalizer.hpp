/**
 * @file      Normalizer.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Min-max feature normalisation using the bounds recorded during TRAINING.
 * @version   3.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   The bounds are embedded data, and they must be the ones from training. Recomputing
 *            them at runtime would change the meaning of every feature and invalidate the model
 *            silently.
 *
 * @details   Two decisions here come from reading @c extract_all() in @c src/pipeline.py of the
 *            reproduction repository, and NOT from what min-max normalisation usually does:
 *            - a feature whose @c xmax equals its @c xmin is NOT an error. The reference computes
 *              @c rng @c = @c where(xmax @c > @c xmin, @c xmax-xmin, @c 1.0), so a feature that
 *              was constant during training is divided by one and comes out as @c x-xmin. An
 *              implementation that rejected the artefact, or that divided by zero, would differ
 *              from the model's own training on exactly that feature;
 *            - there is NO clipping. The reference uses plain arithmetic, not
 *              @c sklearn.MinMaxScaler, so a feature that exceeds its training range at runtime
 *              comes out above one or below zero, and is handed to the trees that way. Saturating
 *              here would move the decision boundary precisely on the extreme values, which are
 *              the attacks.
 *
 * @details   The bounds are kept in @c double, not in @c float, because the reference computes
 *            @c (x-xmin)/rng in float64 and only then casts the result to float32. Keeping them
 *            in float would add a rounding step the reference does not have.
 */
#pragma once

#include <array>
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

/// Per-feature bounds, in the precision the reference normalises with.
using BoundsVec = std::array<double, kNumFeatures>;

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
     * @retval ::ids::IdsSensorErrc::ArtefactCorrupted  The file does not declare
     *                                                 ::ids::kNumFeatures features, or ends before
     *                                                 both vectors are read.
     * @details Layout, little-endian, matching @c tools/export_minmax.py: a @c uint32 with the
     *          number of features, then that many @c double of @c xmin, then that many of
     *          @c xmax.
     * @details Bounds with @c xmax @c <= @c xmin are ACCEPTED, because the reference accepts them;
     *          see the note at the top of this file. There is therefore no configuration error to
     *          report from here.
     * @warning The artefact must come from the same training run as the model and the byte models.
     *          Mixing artefacts from different runs shifts the decision boundary without any error.
     */
    static ara::core::Result<Normalizer> Create(const std::string &path);

    /**
     * @brief Build from explicit bounds, bypassing the artefact file.
     * @param[in] xmin Per-feature lower bounds, as recorded during training.
     * @param[in] xmax Per-feature upper bounds, as recorded during training.
     * @return A normaliser holding the given bounds.
     * @details Returns the normaliser directly, not an ara::core::Result: there is no way for this
     *          to fail, because a degenerate feature is a legitimate input and not an error. A
     *          Result here would declare an error code that nothing ever produces, which is the
     *          defect the error-code audit in tests/README.md went looking for.
     * @details Used by the tests, to drive the normaliser without an artefact on disk.
     */
    static Normalizer FromBounds(const BoundsVec &xmin, const BoundsVec &xmax);

    /**
     * @brief Normalise a raw feature vector.
     * @param[in] x Raw feature vector.
     * @return The vector with @c (x-xmin)/(xmax-xmin) applied to each feature.
     * @details Returns the normalised vector rather than writing through a reference, per rule
     *          A8-4-8. ::ids::FeatureVec is 64 bytes and is returned in place, so this costs
     *          nothing on the hot path.
     * @details Values are NOT clipped to [0, 1], and a feature that was constant during training
     *          is divided by one. Both follow the reference; see the note at the top of this file.
     * @warning Exactness against the reference stops one step upstream, not here. The reference
     *          carries the RAW features in float64 all the way into this subtraction, while this
     *          component carries them in ::ids::FeatureVec, which is float32, and the parity test
     *          pins the two together at float32. The arithmetic below then matches the reference
     *          bit for bit GIVEN the same float32 input, but the float64 input the reference
     *          actually uses may differ beyond float32, so the normalised value can differ by
     *          about one float32 unit in the last place. That is a consequence of the component
     *          being float32 internally, which is a deliberate choice, and it is why no test
     *          claims exact parity on the normalised vector.
     */
    FeatureVec Apply(const FeatureVec &x) const;

    /**
     * @brief Lower bounds in use.
     * @return Per-feature lower bounds.
     */
    const BoundsVec &Xmin() const noexcept
    {
        return m_xmin;
    }

    /**
     * @brief Upper bounds in use.
     * @return Per-feature upper bounds.
     */
    const BoundsVec &Xmax() const noexcept
    {
        return m_xmax;
    }

    /**
     * @brief Divisors in use, one per feature.
     * @return @c xmax-xmin where that is positive, and one where it is not.
     * @details Exposed so a test can assert the degenerate-feature rule directly, instead of
     *          inferring it from a normalised value.
     */
    const BoundsVec &Range() const noexcept
    {
        return m_range;
    }

  private:
    /**
     * @brief Construct from bounds, computing the divisors.
     * @param[in] xmin Per-feature lower bounds.
     * @param[in] xmax Per-feature upper bounds.
     * @return None.
     * @details Private so that Create() and FromBounds() are the only ways in, which is what
     *          guarantees the divisors are never zero.
     */
    Normalizer(const BoundsVec &xmin, const BoundsVec &xmax);

    BoundsVec m_xmin;  ///< Per-feature lower bounds from training.
    BoundsVec m_xmax;  ///< Per-feature upper bounds from training.
    BoundsVec m_range; ///< Divisor per feature: xmax-xmin, or one for a degenerate feature.
};

} // namespace ids

/** @}*/ // End of IdsSensorNormalizer
