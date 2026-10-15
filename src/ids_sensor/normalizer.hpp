// Normalizacao min-max com os limites do TREINO.
//
// E dado embarcado, e precisa ser o do treino: recalcular os limites em runtime
// mudaria o significado das features e invalidaria o modelo em silencio.
#ifndef IDS_SENSOR_NORMALIZER_HPP
#define IDS_SENSOR_NORMALIZER_HPP

#include <string>

#include "types.hpp"

namespace ids {

class Normalizer {
public:
    /// Carrega xmin/xmax exportados pelo treino.
    static Normalizer Load(const std::string &path);

    /// Aplica (x - xmin) / (xmax - xmin), no lugar.
    ///
    /// Atencao aos limites: uma feature sem teto natural pode exceder o xmax do
    /// treino em runtime. O comportamento aqui (saturar ou deixar passar) tem
    /// de ser o MESMO do Python, senao a fronteira de decisao muda justamente
    /// nos casos extremos, que sao os ataques.
    void Apply(FeatureVec &x) const;

    FeatureVec xmin{};
    FeatureVec xmax{};
};

}  // namespace ids

#endif  // IDS_SENSOR_NORMALIZER_HPP
