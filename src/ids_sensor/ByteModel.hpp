/**
 * @file      ByteModel.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Per-layer positional byte model: log-probability of each byte value AT EACH
 *            POSITION, estimated over benign traffic.
 * @version   3.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   This is DATA, not code: the tables are trained in Python with Laplace smoothing and
 *            embedded here, together with the normalisation bounds and the tree model.
 *
 * @details   The model is **positional**, which is easy to get wrong. The table is not one
 *            distribution over 256 byte values; it is @c L distributions, one per byte position,
 *            so the score of a payload depends on WHERE each byte sits. An earlier version of this
 *            header declared a single 256-entry table and would have made parity with the Python
 *            reference impossible while compiling perfectly.
 *
 * @details   The number of positions differs per layer, and the values are a CONTRACT with
 *            training: SOME/IP 64, Service Discovery 80, transport 64. A payload longer than
 *            @c L contributes only its first @c L bytes; a shorter one contributes only the
 *            positions it has, with no padding symbol.
 *
 * @details   On the cross entropy: it divides by the number of bytes actually scored, not by
 *            @c L. The Python docstring says @c 1/L while its code divides by @c k, and the code
 *            is what the model was trained and evaluated with, so @c k is what parity requires.
 *
 * @details   The table holds @c double, not @c float, because the reference computes in float64.
 *            Single precision would still land inside the precision of the @c float feature
 *            vector, but it would turn the parity test from an exact comparison into one with a
 *            tolerance, and a tolerance hides exactly the kind of small systematic shift that a
 *            parity test exists to catch. The cost is about 250 KiB.
 */
#pragma once

#include <array>
#include <cstddef>
#include <memory>
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

static constexpr std::size_t kSomeipPositions = 64U; ///< Positions scored on the SOME/IP payload. Contract with training.
static constexpr std::size_t kSdPositions     = 80U; ///< Positions scored on the Service Discovery payload.
static constexpr std::size_t kL4Positions     = 64U; ///< Positions scored on the transport payload.
static constexpr std::size_t kMaxPositions    = 80U; ///< Largest of the three, so one fixed table type serves all.

/**
 * @brief Both payload scores, computed together.
 * @details Returned as one aggregate rather than through two output parameters, per rules A8-4-8
 *          and A8-4-4.
 */
struct PayloadScores
{
    float logLik       = 0.0F; ///< Log-likelihood of the payload under the model.
    float crossEntropy = 0.0F; ///< Cross entropy of the payload, in nats per scored byte.
};

/**
 * @brief Positional log-probability table for one protocol layer.
 */
class ByteModel final
{
  public:
    using LogProbRow   = std::array<double, kByteValues>;         ///< Log-probability per byte value, at one position.
    using LogProbTable = std::array<LogProbRow, kMaxPositions>;   ///< One row per position; only the first Positions() are used.

    /**
     * @brief Build a model from a trained table.
     * @param[in] logProb   Positional log-probabilities, as exported by training.
     * @param[in] positions Number of valid rows, which is the @c L of this layer.
     * @return The model, or an error when @p positions is unusable.
     * @retval ::ids::IdsSensorErrc::InvalidConfiguration @p positions is zero or above
     *                                                   ::ids::kMaxPositions.
     */
    static ara::core::Result<ByteModel> Create(const LogProbTable &logProb, std::size_t positions);

    /**
     * @brief Log-likelihood of a payload under this model.
     * @param[in] payload Payload view to score.
     * @return Sum of the per-position log-probabilities over the first Positions() bytes, or
     *         0.0F for an empty payload.
     */
    float LogLik(const PayloadView &payload) const;

    /**
     * @brief Cross entropy of a payload, that is minus the log-likelihood over the bytes scored.
     * @param[in] payload Payload view to score.
     * @return Cross entropy in nats per scored byte, or 0.0F for an empty payload.
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
     * @brief Number of positions this model scores.
     * @return The @c L of this layer.
     */
    std::size_t Positions() const noexcept
    {
        return m_positions;
    }

  private:
    /**
     * @brief Construct from an already validated table.
     * @param[in] logProb   Positional log-probabilities.
     * @param[in] positions Number of valid rows.
     * @return None.
     * @details Private so that Create() is the only way in.
     */
    ByteModel(const LogProbTable &logProb, std::size_t positions) : m_logProb(logProb), m_positions(positions)
    {
    }

    LogProbTable m_logProb;  ///< Positional log-probabilities from training.
    std::size_t  m_positions; ///< Valid rows of m_logProb, the L of this layer.
};

/**
 * @brief One byte model per protocol layer, matching the training setup.
 * @details Held behind a smart pointer because the three tables together are around 490 KiB, and
 *          the C++ code standard asks a validating factory to hand back a smart pointer rather
 *          than a by-value object.
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
     * @retval ::ids::IdsSensorErrc::ArtefactCorrupted  The file does not hold three tables with
     *                                                 the expected dimensions.
     * @warning Must come from the same training run as the tree model and the normalisation
     *          bounds.
     */
    static ara::core::Result<std::unique_ptr<ByteModels>> Create(const std::string &path);

    /**
     * @brief Build from three explicit models, bypassing the artefact file.
     * @param[in] someip Model over the SOME/IP payload, header excluded.
     * @param[in] sd     Model over the Service Discovery payload.
     * @param[in] l4     Model over the transport payload, SOME/IP header included.
     * @return The three models.
     * @details Used by the tests, to drive the extractor without an artefact on disk.
     */
    static std::unique_ptr<ByteModels> Create(const ByteModel &someip, const ByteModel &sd, const ByteModel &l4);

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
     * @brief Construct from three already built models.
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

/**
 * @brief Byte-level Hamming distance between two payloads.
 * @param[in] a First payload.
 * @param[in] b Second payload.
 * @return Positions that differ over the common length, plus the difference in length.
 * @details Bytes present in one payload and absent from the other count as a change, which is
 *          what makes the measure react to a payload that grew or shrank. Two empty payloads give
 *          zero, and when one is empty the result is the length of the other.
 */
std::uint32_t HammingDistance(const PayloadView &a, const PayloadView &b);

} // namespace ids

/** @}*/ // End of IdsSensorByteModel
