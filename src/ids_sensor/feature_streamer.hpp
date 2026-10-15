// Extracao das 16 features. Classe CONCRETA, sem interface: ha uma
// implementacao so, e ela e justamente o que o teste de paridade fixa.
//
// E o unico estagio COM ESTADO, e por isso o de maior risco: basta uma janela
// comecar cheia em vez de vazia, ou uma ordem de atualizacao diferente da do
// Python, para as features divergirem em silencio.
//
// Criterio de aceite: mesmo PCAP produz as mesmas 16 features em Python e aqui.
#ifndef IDS_SENSOR_FEATURE_STREAMER_HPP
#define IDS_SENSOR_FEATURE_STREAMER_HPP

#include <cstddef>

#include "byte_model.hpp"
#include "types.hpp"

namespace ids {

/// Parametros das janelas. Devem casar com os do treino, senao as features
/// mudam de significado.
struct StreamerConfig {
    std::size_t recent_payload_window = 5;      ///< repeticao de payload, por fluxo
    std::size_t source_payload_window = 1000;   ///< diversidade, por origem

    /// Teto de fluxos e origens mantidos em memoria.
    ///
    /// Offline, sobre um PCAP fixo, o estado por fluxo simplesmente cresce e
    /// nada acontece. Num sensor que roda por horas isso e vazamento: cada
    /// fluxo novo adiciona estado que nunca sai. Com teto, o mais antigo e
    /// descartado. E um problema que so existe online, e que a validacao
    /// offline nao pegaria.
    std::size_t max_flows = 4096;
    std::size_t max_sources = 256;
};

class FeatureStreamer {
public:
    FeatureStreamer(const ByteModels &models, const StreamerConfig &cfg);
    ~FeatureStreamer();

    /// Consome um pacote e produz as 16 features CRUAS (sem normalizar).
    /// Devolve false quando o pacote nao gera vetor (ex.: nao e SOME/IP).
    /// Nao aloca no caminho quente.
    bool Step(const ParsedPacket &p, FeatureVec &out);

    /// Zera todo o estado. Usado entre execucoes para a medicao ser repetivel.
    void Reset();

private:
    struct Impl;
    Impl *impl_;   ///< pimpl: mantem os contêineres fora do cabecalho
};

}  // namespace ids

#endif  // IDS_SENSOR_FEATURE_STREAMER_HPP
