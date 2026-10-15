// Tipos compartilhados do ids_sensor.
//
// Restricoes de projeto, validas em todo este diretorio:
//  - C++14 (alinhado as diretrizes AUTOSAR C++14), entao nada de std::span
//  - sem alocacao no caminho quente: tamanhos fixos, buffers reservados na construcao
//  - payloads sao VISTAS para o buffer do frame, nunca copias
#ifndef IDS_SENSOR_TYPES_HPP
#define IDS_SENSOR_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace ids {

constexpr std::size_t kNumFeatures = 16;
constexpr std::size_t kNumClasses = 5;

using FeatureVec = std::array<float, kNumFeatures>;
using ProbVec = std::array<float, kNumClasses>;

/// As 5 classes, na MESMA ordem do treino. A ordem e contrato: o indice
/// devolvido pelo classificador indexa diretamente este enum.
enum class Klass : std::uint8_t {
    kNormal = 0,
    kDos = 1,
    kFuzzy = 2,
    kMitmSingle = 3,
    kMitmMulti = 4,
};

/// Vista nao-proprietaria sobre bytes do frame. Nao sobrevive ao frame.
struct Bytes {
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;
};

/// Frame cru, como entregue pela fonte de pacotes.
struct Frame {
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;
    double ts = 0.0;            ///< segundos; relogio unico do cenario
};

/// Chave de fluxo compacta. Inteiros, nao strings: comparacao e hash baratos
/// e sem alocacao (o lado Python usa tupla de strings, que aqui seria custoso).
struct FlowKey {
    std::uint32_t src = 0;
    std::uint32_t dst = 0;
    std::uint16_t sport = 0;
    std::uint16_t dport = 0;
    std::uint8_t proto = 0;

    bool operator==(const FlowKey &o) const {
        return src == o.src && dst == o.dst && sport == o.sport &&
               dport == o.dport && proto == o.proto;
    }
};

struct FlowKeyHash {
    std::size_t operator()(const FlowKey &k) const {
        std::uint64_t h = k.src;
        h = h * 1099511628211ULL ^ k.dst;
        h = h * 1099511628211ULL ^ (static_cast<std::uint64_t>(k.sport) << 16 | k.dport);
        h = h * 1099511628211ULL ^ k.proto;
        return static_cast<std::size_t>(h);
    }
};

/// Resultado do parsing em camadas (Ethernet -> IP -> TCP/UDP -> SOME/IP -> SD).
/// Sem estado: depende apenas do frame.
struct ParsedPacket {
    bool valid = false;         ///< false quando nao e SOME/IP ou esta malformado
    double ts = 0.0;
    FlowKey flow;
    std::uint16_t ip_len = 0;
    Bytes pl_l4;                ///< payload de transporte
    Bytes pl_someip;            ///< payload SOME/IP (sem cabecalho)
    Bytes pl_sd;                ///< payload Service Discovery, quando houver
};

}  // namespace ids

#endif  // IDS_SENSOR_TYPES_HPP
