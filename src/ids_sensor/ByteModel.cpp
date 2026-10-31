/**
 * @file      ByteModel.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Positional byte-model scoring and the byte-level Hamming distance.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   Every decision here is dictated by parity with the Python reference in
 *            @c src/bytemodel.py of the reproduction repository. Where a choice looked arbitrary,
 *            it was taken from that code and not from the paper, because the model was trained
 *            and evaluated against that code.
 */
#include "src/ids_sensor/ByteModel.hpp"

#include <cstdio>

namespace ids
{

ara::core::Result<ByteModel> ByteModel::Create(const LogProbTable &logProb, std::size_t positions)
{
    using Res = ara::core::Result<ByteModel>;

    if ((positions == 0U) || (positions > kMaxPositions))
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::InvalidConfiguration));
    }
    else
    {
        /* Usable number of positions. */
    }
    return Res::FromValue(ByteModel(logProb, positions));
}

float ByteModel::LogLik(const PayloadView &payload) const
{
    return Score(payload).logLik;
}

float ByteModel::CrossEntropy(const PayloadView &payload) const
{
    return Score(payload).crossEntropy;
}

PayloadScores ByteModel::Score(const PayloadView &payload) const
{
    PayloadScores scores;

    /* Bytes actually scored: the payload truncated to this layer's L. A longer
     * payload contributes only its first L bytes; a shorter one contributes only
     * the positions it has, with no padding symbol. */
    const std::size_t scored = (payload.size() < m_positions) ? payload.size() : m_positions;
    if (scored == 0U)
    {
        /* Empty payload scores zero on both, which is what the reference does and
         * what keeps the feature vector finite. Dividing by zero here would put a
         * NaN into the model input. */
        return scores;
    }
    else
    {
        /* There is something to score. */
    }

    /* One pass for both quantities. The reference computes the same sum twice,
     * once in loglik and once in cross_entropy, which costs about half the time
     * of the whole entropy category. */
    double sum = 0.0;
    for (std::size_t i = 0U; i < scored; ++i)
    {
        sum += m_logProb[i][payload[i]];
    }

    scores.logLik = static_cast<float>(sum);

    /* Divided by the bytes SCORED, not by L. The reference docstring says 1/L
     * while its code divides by k, and the code is what trained the model. */
    scores.crossEntropy = static_cast<float>(-sum / static_cast<double>(scored));
    return scores;
}

ara::core::Result<std::unique_ptr<ByteModels>> ByteModels::Create(const std::string &path)
{
    using Res = ara::core::Result<std::unique_ptr<ByteModels>>;

    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactNotFound));
    }
    else
    {
        /* File opened. */
    }

    /* Artefact layout, written by the exporter alongside the model: three tables
     * in the order someip, sd, l4, each one its own number of positions followed
     * by that many rows of 256 double-precision values. The position counts are
     * read from the file and checked against the contract, rather than assumed,
     * so an artefact from a differently configured training run is rejected here
     * instead of silently shifting every score. */
    const std::size_t expected[3] = {kSomeipPositions, kSdPositions, kL4Positions};
    ByteModel::LogProbTable tables[3];
    bool                    ok = true;

    for (std::size_t layer = 0U; (layer < 3U) && ok; ++layer)
    {
        std::uint32_t positions = 0U;
        if (std::fread(&positions, sizeof(positions), 1U, file) != 1U)
        {
            ok = false;
        }
        else if (static_cast<std::size_t>(positions) != expected[layer])
        {
            ok = false;
        }
        else
        {
            for (std::size_t i = 0U; (i < static_cast<std::size_t>(positions)) && ok; ++i)
            {
                if (std::fread(tables[layer][i].data(), sizeof(double), kByteValues, file) != kByteValues)
                {
                    ok = false;
                }
                else
                {
                    /* Row read. */
                }
            }
        }
    }

    static_cast<void>(std::fclose(file));

    if (!ok)
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* All three tables read with the expected dimensions. */
    }

    const ara::core::Result<ByteModel> someip = ByteModel::Create(tables[0], kSomeipPositions);
    const ara::core::Result<ByteModel> sd     = ByteModel::Create(tables[1], kSdPositions);
    const ara::core::Result<ByteModel> l4     = ByteModel::Create(tables[2], kL4Positions);
    if ((!someip.HasValue()) || (!sd.HasValue()) || (!l4.HasValue()))
    {
        return Res::FromError(ara::core::ErrorCode(IdsSensorErrc::ArtefactCorrupted));
    }
    else
    {
        /* The three models are usable. */
    }

    return Res::FromValue(ByteModels::Create(someip.Value(), sd.Value(), l4.Value()));
}

std::unique_ptr<ByteModels> ByteModels::Create(const ByteModel &someip, const ByteModel &sd, const ByteModel &l4)
{
    return std::unique_ptr<ByteModels>(new ByteModels(someip, sd, l4));
}

std::uint32_t HammingDistance(const PayloadView &a, const PayloadView &b)
{
    const std::size_t sizeA = a.size();
    const std::size_t sizeB = b.size();

    if ((sizeA == 0U) && (sizeB == 0U))
    {
        return 0U;
    }
    else
    {
        /* At least one payload has bytes. */
    }

    const std::size_t common = (sizeA < sizeB) ? sizeA : sizeB;
    if (common == 0U)
    {
        /* One side is empty: every byte of the other counts as a change. */
        return static_cast<std::uint32_t>((sizeA > sizeB) ? sizeA : sizeB);
    }
    else
    {
        /* Both sides have bytes to compare. */
    }

    std::uint32_t differing = 0U;
    for (std::size_t i = 0U; i < common; ++i)
    {
        if (a[i] != b[i])
        {
            ++differing;
        }
        else
        {
            /* Same byte at this position. */
        }
    }

    /* Plus the length difference: a byte present in one payload and absent from
     * the other is a change too, which is what makes the measure react to a
     * payload that grew or shrank. */
    const std::size_t lengthDiff = (sizeA > sizeB) ? (sizeA - sizeB) : (sizeB - sizeA);
    return (differing + static_cast<std::uint32_t>(lengthDiff));
}

} // namespace ids
