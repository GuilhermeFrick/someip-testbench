// De onde vem os pacotes.
//
// Esta e, na pratica, a interface mais util das tres: ela faz o MESMO sensor
// rodar offline sobre um PCAP e online no barramento. Sem ela, o teste de
// paridade exigiria um caminho de codigo separado do de producao, e aí o teste
// deixaria de testar o que roda.
#ifndef IDS_SENSOR_PACKET_SOURCE_HPP
#define IDS_SENSOR_PACKET_SOURCE_HPP

#include <memory>
#include <string>

#include "types.hpp"

namespace ids {

class IPacketSource {
public:
    virtual ~IPacketSource() = default;

    /// Entrega o proximo frame. Devolve false no fim do fluxo.
    /// O buffer apontado por `out` e valido ate a proxima chamada.
    virtual bool Next(Frame &out) = 0;
};

/// Replay de PCAP: determinista e repetivel, para paridade e medicao.
class PcapSource final : public IPacketSource {
public:
    explicit PcapSource(const std::string &path);
    ~PcapSource() override;
    bool Next(Frame &out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Captura viva na interface do barramento.
class LiveSource final : public IPacketSource {
public:
    explicit LiveSource(const std::string &iface);
    ~LiveSource() override;
    bool Next(Frame &out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ids

#endif  // IDS_SENSOR_PACKET_SOURCE_HPP
