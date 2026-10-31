/**
 * @file      Normalizer.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Min-max feature normalisation using the bounds recorded during TRAINING.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   The bounds are embedded data, and they must be the ones from training. Recomputing
 *            them at runtime would change the meaning of every feature and invalidate the model
 *            silently.
 */
#pragma once

#include <string>

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
     * @brief Construct from explicit bounds.
     * @param[in] xmin Per-feature lower bounds, as recorded during training.
     * @param[in] xmax Per-feature upper bounds, as recorded during training.
     * @return None.
     */
    Normalizer(const FeatureVec &xmin, const FeatureVec &xmax);

    /**
     * @brief Load the xmin/xmax pair exported by the training pipeline.
     * @param[in] path Path to the exported bounds artefact.
     * @return A normaliser holding the loaded bounds.
     * @warning The artefact must come from the same training run as the model and the byte models.
     *          Mixing artefacts from different runs shifts the decision boundary without any error.
     */
    static Normalizer Load(const std::string &path);

    /**
     * @brief Apply @c (x-xmin)/(xmax-xmin) in place.
     * @param[in,out] x Raw feature vector on entry, normalised vector on return.
     * @return None.
     * @warning Mind the bounds: a feature with no natural ceiling can exceed the training xmax at
     *          runtime. Whatever is done here, saturate or let it through, MUST match the Python
     *          side, otherwise the decision boundary differs exactly on the extreme cases, which
     *          are the attacks.
     */
    void Apply(FeatureVec &x) const;

    /**
     * @brief Lower bounds in use.
     * @return Per-feature lower bounds.
     */
    const FeatureVec &Xmin() const
    {
        return m_xmin;
    }

    /**
     * @brief Upper bounds in use.
     * @return Per-feature upper bounds.
     */
    const FeatureVec &Xmax() const
    {
        return m_xmax;
    }

  private:
    FeatureVec m_xmin{}; ///< Per-feature lower bounds from training.
    FeatureVec m_xmax{}; ///< Per-feature upper bounds from training.
};

} // namespace ids

/** @}*/ // End of IdsSensorNormalizer
