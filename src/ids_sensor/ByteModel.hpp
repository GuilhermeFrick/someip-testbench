/**
 * @file      ByteModel.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Per-layer byte model: log-probability table estimated over benign traffic, used for
 *            payload log-likelihood and cross entropy.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   This is DATA, not code: the tables are trained in Python with Laplace smoothing and
 *            embedded here. Together with the normalisation bounds they are the artefacts that
 *            cross the language boundary alongside the tree model itself.
 */
#pragma once

#include <array>
#include <string>

#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorByteModel Byte models
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Log-probability table for one protocol layer.
 */
class ByteModel final
{
  public:
    using LogProbTable = std::array<float, kByteValues>; ///< Log-probability indexed by byte value.

    /**
     * @brief Construct from a trained table.
     * @param[in] logProb Log-probability per byte value, as exported by training.
     * @return None.
     */
    explicit ByteModel(const LogProbTable &logProb);

    /**
     * @brief Log-likelihood of a payload under this model.
     * @param[in] bytes Payload view to score.
     * @return Sum of the per-byte log-probabilities, or 0.0F for an empty payload.
     */
    float LogLik(const Bytes &bytes) const;

    /**
     * @brief Cross entropy of a payload, that is @c -LogLik/n.
     * @param[in] bytes Payload view to score.
     * @return Cross entropy in nats per byte, or 0.0F for an empty payload.
     * @warning When both quantities are needed, call Both() instead. On the Python side the two
     *          functions recompute the same sum, which costs about half the time of the whole
     *          entropy category. That waste is not repeated here.
     */
    float CrossEntropy(const Bytes &bytes) const;

    /**
     * @brief Compute both quantities in a single pass over the payload.
     * @param[in]  bytes        Payload view to score.
     * @param[out] logLik       Log-likelihood of the payload.
     * @param[out] crossEntropy Cross entropy of the payload.
     * @return None.
     */
    void Both(const Bytes &bytes, float &logLik, float &crossEntropy) const;

    /**
     * @brief Table in use.
     * @return Log-probability per byte value.
     */
    const LogProbTable &LogProb() const
    {
        return m_logProb;
    }

  private:
    LogProbTable m_logProb{}; ///< Log-probability per byte value, from training.
};

/**
 * @brief One byte model per protocol layer, matching the training setup.
 */
struct ByteModels
{
    ByteModel someip; ///< Model over the SOME/IP payload, header excluded.
    ByteModel sd;     ///< Model over the Service Discovery payload.
    ByteModel l4;     ///< Model over the transport payload.

    /**
     * @brief Load the three tables from the artefact exported by training.
     * @param[in] path Path to the exported byte-model artefact.
     * @return The three loaded models.
     * @warning Must come from the same training run as the model and the normalisation bounds.
     */
    static ByteModels Load(const std::string &path);
};

} // namespace ids

/** @}*/ // End of IdsSensorByteModel
