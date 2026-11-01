/* Confere o cabecalho GERADO contra a matriz de comunicacao.
 *
 * Imprime cada identificador como `chave=valor`, e o tests/check_comm_matrix.py
 * compara linha por linha com o YAML. E um round-trip: se o gerador errar um
 * campo, a comparacao acusa. Conferir "a olho" o cabecalho gerado nao pega
 * troca de instancia por servico, que sao os dois 16 bits mais faceis de
 * inverter.
 *
 * Tambem exercita de proposito a armadilha de ODR do C++14, e este teste JA A
 * PEGOU UMA VEZ: na primeira versao o gerador emitia `static constexpr` membros
 * de DADO. Lidos por valor funcionavam; passados por REFERENCIA CONSTANTE sao
 * odr-used, e em C++14 nao existe `inline variable`, entao a ligacao falhava com
 * "undefined reference to ids::matrix::GnssService::ID". A compilacao passava
 * limpa. O gerador passou a emitir FUNCOES constexpr, que sao implicitamente
 * inline.
 *
 * O que mantem a regressao coberta e a propria SINTAXE de chamada: se o gerador
 * voltar a emitir membros de dado, `GnssService::Id()` deixa de compilar e o
 * teste quebra em voz alta. As duas chamadas por referencia no fim do main()
 * provam a propriedade que importa, que e poder ligar o valor a uma referencia
 * constante sem erro de ligacao.
 */
#include <cstdio>

#include "build/cfg/CommMatrix.hpp"
#include "src/libecu/Types.hpp"

namespace
{

/* Recebe por referencia constante: e isto que forca o odr-use. */
ids::ecu::ServiceId PorReferencia(const ids::ecu::ServiceId &id)
{
    return id;
}

const char *TextoPorReferencia(const char *const &texto)
{
    return texto;
}

} // namespace

int main()
{
    using ids::matrix::GnssEcu;
    using ids::matrix::GnssService;
    using ids::matrix::HmiEcu;
    using ids::matrix::IdsSensor;
    using ids::matrix::Network;

    /* --- rede --- */
    std::printf("network.subnet=%s\n", Network::Subnet());
    std::printf("network.multicast_route=%s\n", Network::MulticastRoute());
    std::printf("network.sd_multicast=%s\n", Network::SdMulticast());
    std::printf("network.sd_port=%u\n", static_cast<unsigned>(Network::SdPort()));

    /* --- servico --- */
    std::printf("service.GnssService.id=%u\n", static_cast<unsigned>(GnssService::Id()));
    std::printf("service.GnssService.instance=%u\n", static_cast<unsigned>(GnssService::Instance()));
    std::printf("service.GnssService.port=%u\n", static_cast<unsigned>(GnssService::UdpPort()));

    /* --- eventos --- */
    std::printf("event.GnssService.Position=%u\n", static_cast<unsigned>(GnssService::Events::Position()));
    std::printf("event.GnssService.Velocity=%u\n", static_cast<unsigned>(GnssService::Events::Velocity()));
    std::printf("event.GnssService.Status=%u\n", static_cast<unsigned>(GnssService::Events::Status()));

    /* --- eventgroups --- */
    std::printf("eventgroup.GnssService.4455=%u\n", static_cast<unsigned>(GnssService::Eventgroups::Group4455()));
    std::printf("eventgroup.GnssService.4465=%u\n", static_cast<unsigned>(GnssService::Eventgroups::Group4465()));
    std::printf("eventgroup.GnssService.4555=%u\n", static_cast<unsigned>(GnssService::Eventgroups::Group4555()));

    /* --- nos --- */
    std::printf("node.gnss_ecu.name=%s\n", GnssEcu::Name());
    std::printf("node.gnss_ecu.app_name=%s\n", GnssEcu::AppName());
    std::printf("node.gnss_ecu.ip=%s\n", GnssEcu::Ip());
    std::printf("node.gnss_ecu.app_id=%u\n", static_cast<unsigned>(GnssEcu::AppId()));
    std::printf("node.hmi_ecu.name=%s\n", HmiEcu::Name());
    std::printf("node.hmi_ecu.app_name=%s\n", HmiEcu::AppName());
    std::printf("node.hmi_ecu.ip=%s\n", HmiEcu::Ip());
    std::printf("node.hmi_ecu.app_id=%u\n", static_cast<unsigned>(HmiEcu::AppId()));
    /* O no monitor. Foi a ausencia DELE aqui que o check_comm_matrix.py acusou
     * quando ele entrou na matriz: o round-trip exige que todo no do YAML
     * apareca no cabecalho gerado E seja exercitado por este teste.
     * Acrescentar um no a matriz e esquecer esta linha e exatamente o que ele
     * existe para pegar. */
    std::printf("node.ids_sensor.name=%s\n", IdsSensor::Name());
    std::printf("node.ids_sensor.app_name=%s\n", IdsSensor::AppName());
    std::printf("node.ids_sensor.ip=%s\n", IdsSensor::Ip());
    std::printf("node.ids_sensor.app_id=%u\n", static_cast<unsigned>(IdsSensor::AppId()));

    /* --- a prova de ODR: se isto ligar, a ressalva do C++14 nao morde --- */
    std::printf("odr.service_id_por_referencia=%u\n", static_cast<unsigned>(PorReferencia(GnssService::Id())));
    std::printf("odr.ip_por_referencia=%s\n", TextoPorReferencia(GnssEcu::Ip()));

    return 0;
}
