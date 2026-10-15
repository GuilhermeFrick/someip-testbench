// Saida do sensor: o Security Event tipado.
//
// A interface existe porque o destino varia de verdade: na bancada e um log
// estruturado; num alvo AUTOSAR e o ara::idsm; nos testes e um coletor em
// memoria. O sensor nao deve saber qual deles esta ativo.
#ifndef IDS_SENSOR_EVENT_REPORTER_HPP
#define IDS_SENSOR_EVENT_REPORTER_HPP

#include <cstdint>

#include "types.hpp"

namespace ids {

/// Identificadores dos tipos de evento. Correspondem a um
/// SecurityEventDefinition do IdsM; os valores concretos vem da configuracao
/// da plataforma, nao sao inventados aqui.
enum class SecurityEventId : std::uint32_t {
    kDos = 1,
    kMalformedMessage = 2,     ///< Fuzzy
    kSpoofedNotification = 3,  ///< MITM de origem unica
    kMultiSourceSpoof = 4,     ///< MITM de multiplas origens
};

/// Dados de contexto do evento. Campos fixos, sem alocacao.
struct EventContext {
    std::uint32_t src_ip = 0;
    std::uint16_t service_id = 0;
    std::uint16_t method_id = 0;
    float confidence = 0.0F;   ///< probabilidade da classe predita
};

class IEventReporter {
public:
    virtual ~IEventReporter() = default;

    /// Reporta um evento. O IdsM e quem filtra, agrega e qualifica; o sensor
    /// apenas reporta o que observou.
    virtual void Report(SecurityEventId id, double ts, const EventContext &ctx) = 0;
};

/// Traduz a classe predita em um tipo de evento.
/// Devolve false para `kNormal`, que nao gera evento.
bool KlassToEvent(Klass k, SecurityEventId &out);

}  // namespace ids

#endif  // IDS_SENSOR_EVENT_REPORTER_HPP
