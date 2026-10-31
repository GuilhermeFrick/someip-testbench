/**
 * @file      ByteModel.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Per-layer byte model: log-probability table estimated over benign traffic, used for
 *            payload log-likelihood and cross entropy.
 * @version   2.0
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

#include "src/ara/core/Result.hpp"
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"
#include "src/ids_sensor/types.hpp"

/** @addtogroup IdsSensorByteModel Byte models
 *  @ingroup IdsSensor
 *  @{
 */

namespace ids
{

/**
 * @brief Both payload scores, computed together.
 * @details Returned as one aggregate rather than through two output parameters, per rules A8-4-8
 *          and A8-4-4.
 */
struct PayloadScores
{
    float logLik       = 0.0F; ///< Log-likelihood of the payload under the model.
    float crossEntropy = 0.0F; ///< Cross entropy of the payload, in nats per byte.
};

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
    explicit ByteModel(const LogProbTable &logProb) : m_logProb(logProb)
    {
    }

    /**
     * @brief Log-likelihood of a payload under this model.
     * @param[in] payload Payload view to score.
     * @return Sum of the per-byte log-probabilities, or 0.0F for an empty payload.
     */
    float LogLik(const PayloadView &payload) const;

    /**
     * @brief Cross entropy of a payload, that is @c -LogLik/n.
     * @param[in] payload Payload view to score.
     * @return Cross entropy in nats per byte, or 0.0F for an empty payload.
     * @warning When both quantities are needed, call Score() instead. On the Python side the two
     *          functions recompute the same sum, which costs about half the time of the whole
     *          entropy category. That waste is not repeated here.
     */
    float CrossEntropy(const PayloadView &payload) const;

    /**
     * @brief Compute both scores in a single pass over the payload.
     * @param[in] payload Payload view to score.
     * @return Both scores, or zeroes for an empty payload.
     */
    PayloadScores Score(const PayloadView &payload) const;

    /**
     * @brief Table in use.
     * @return Log-probability per byte value.
     */
    const LogProbTable &LogProb() const noexcept
    {
        return m_logProb;
    }

  private:
    LogProbTable m_logProb; ///< Log-probability per byte value, from training.
};

/**
 * @brief One byte model per protocol layer, matching the training setup.
 */
class ByteModels final
{
  public:
    /**
     * @brief Load the three tables from the artefact exported by training.
     * @param[in] path Path to the exported byte-model artefact.
     * @return The three loaded models, or an error describing why they could not be loaded.
     * @retval ::ids::IdsSensorErrc::ArtefactNotFound   No file exists at @p path.
     * @retval ::ids::IdsSensorErrc::ArtefactUnreadable The file exists but could not be read; the
     *                                                 support data carries the platform errno.
     * @retval ::ids::IdsSensorErrc::ArtefactCorrupted  The file does not hold three tables of
     *                                                 ::ids::kByteValues values each.
     * @warning Must come from the same training run as the model and the normalisation bounds.
     */
    static ara::core::Result<ByteModels> Create(const std::string &path);

    /**
     * @brief Build from three explicit tables, bypassing the artefact file.
     * @param[in] someip Table over the SOME/IP payload, header excluded.
     * @param[in] sd     Table over the Service Discovery payload.
     * @param[in] l4     Table over the transport payload.
     * @return The three models.
     * @details Used by the tests, to drive the extractor without an artefact on disk.
     */
    static ByteModels Create(const ByteModel::LogProbTable &someip, const ByteModel::LogProbTable &sd, const ByteModel::LogProbTable &l4);

    /**
     * @brief Model over the SOME/IP payload, header excluded.
     * @return Reference to the model.
     */
    const ByteModel &Someip() const noexcept
    {
        return m_someip;
    }

    /**
     * @brief Model over the Service Discovery payload.
     * @return Reference to the model.
     */
    const ByteModel &Sd() const noexcept
    {
        return m_sd;
    }

    /**
     * @brief Model over the transport payload.
     * @return Reference to the model.
     */
    const ByteModel &L4() const noexcept
    {
        return m_l4;
    }

  private:
    /**
     * @brief Construct from three already loaded models.
     * @param[in] someip Model over the SOME/IP payload.
     * @param[in] sd     Model over the Service Discovery payload.
     * @param[in] l4     Model over the transport payload.
     * @return None.
     * @details Private so that a Create() overload is the only way in.
     */
    ByteModels(const ByteModel &someip, const ByteModel &sd, const ByteModel &l4) : m_someip(someip), m_sd(sd), m_l4(l4)
    {
    }

    ByteModel m_someip; ///< Model over the SOME/IP payload, header excluded.
    ByteModel m_sd;     ///< Model over the Service Discovery payload.
    ByteModel m_l4;     ///< Model over the transport payload.
};

} // namespace ids

/** @}*/ // End of IdsSensorByteModel
