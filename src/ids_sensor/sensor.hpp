// Composicao: e a unica classe que o main() enxerga.
//
// O sensor nao implementa nada do pipeline; ele encadeia os estagios e
// cronometra cada um. A separacao em quatro estagios e deliberada: e a mesma
// decomposicao de custo que reportamos (layer, feature, classification,
// decision), entao a instrumentacao cai nos mesmos pontos, sem traducao.
#ifndef IDS_SENSOR_SENSOR_HPP
#define IDS_SENSOR_SENSOR_HPP

#include <cstdint>

#include "classifier.hpp"
#include "event_reporter.hpp"
#include "feature_streamer.hpp"
#include "normalizer.hpp"
#include "packet_source.hpp"
#include "someip_parser.hpp"
#include "types.hpp"

namespace ids {

/// Tempo acumulado por estagio, em microssegundos.
struct StageTiming {
    double parse_us = 0.0;
    double feature_us = 0.0;
    double classify_us = 0.0;
    double decide_us = 0.0;
    std::uint64_t packets = 0;
    std::uint64_t events = 0;
};

class Sensor {
public:
    /// As dependencias entram por referencia: quem constroi decide qual motor
    /// e qual destino de evento usar. O sensor nao escolhe nenhum dos dois.
    Sensor(SomeipParser &parser, FeatureStreamer &streamer,
           const Normalizer &norm, const IClassifier &clf,
           IEventReporter &reporter);

    /// Processa um frame: parse, features, normalizacao, inferencia e, se a
    /// classe nao for normal, reporta o Security Event.
    void OnFrame(const Frame &f);

    /// Laco: consome a fonte ate o fim. A MESMA funcao serve para replay de
    /// PCAP e para captura viva, porque a fonte e uma interface.
    void Run(IPacketSource &source);

    const StageTiming &timing() const { return timing_; }
    void ResetTiming() { timing_ = StageTiming{}; }

private:
    SomeipParser &parser_;
    FeatureStreamer &streamer_;
    const Normalizer &norm_;
    const IClassifier &clf_;
    IEventReporter &reporter_;
    StageTiming timing_;
};

}  // namespace ids

#endif  // IDS_SENSOR_SENSOR_HPP
