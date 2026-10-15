// Modelo de bytes por camada: tabela de log-probabilidade estimada no trafego
// benigno (com suavizacao de Laplace), usada para log-verossimilhanca e
// entropia cruzada do payload.
//
// Isto e DADO, nao codigo: as tabelas sao treinadas em Python e embarcadas.
// Junto com a normalizacao, formam os artefatos que atravessam a fronteira
// alem do modelo de arvores.
#ifndef IDS_SENSOR_BYTE_MODEL_HPP
#define IDS_SENSOR_BYTE_MODEL_HPP

#include <array>
#include <string>

#include "types.hpp"

namespace ids {

class ByteModel {
public:
    /// log-verossimilhanca do payload sob este modelo.
    float LogLik(const Bytes &b) const;

    /// Entropia cruzada: -LogLik/n.
    ///
    /// Reaproveite a soma quando precisar das duas: no lado Python as duas
    /// funcoes recalculam a mesma soma, o que custa cerca de metade do tempo
    /// da categoria de entropia. Aqui nao repetimos esse desperdicio.
    float CrossEntropy(const Bytes &b) const;

    /// Calcula as duas de uma vez, com uma passada so.
    void Both(const Bytes &b, float &loglik, float &cross_entropy) const;

    std::array<float, 256> log_p{};   ///< log-probabilidade por valor de byte
};

/// Um modelo por camada, como no treino.
struct ByteModels {
    ByteModel someip;
    ByteModel sd;
    ByteModel l4;

    /// Carrega as tres tabelas do artefato exportado pelo treino.
    static ByteModels Load(const std::string &path);
};

}  // namespace ids

#endif  // IDS_SENSOR_BYTE_MODEL_HPP
