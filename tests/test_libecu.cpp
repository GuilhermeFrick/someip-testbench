/* Teste UNITARIO dos caminhos de ERRO da libecu.
 *
 * O platform/node-handshake prova o caminho feliz, incluindo o barramento real, mas
 * nao toca em nenhuma das onze falhas que o EcuErrorDomain declara. Um codigo de
 * erro que ninguem nunca viu disparar e documentacao, nao comportamento.
 *
 * Os casos de validacao pura rodam em qualquer lugar. Os que precisam de uma
 * Skeleton construida precisam de uma configuracao vsomeip valida, e por isso
 * este teste roda dentro da imagem someip-base com VSOMEIP_CONFIGURATION
 * apontando para a config gerada. Sem ela os casos sao REPORTADOS COMO PULADOS,
 * nunca como aprovados.
 */
#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "src/libecu/Proxy.hpp"
#include "src/ids_sensor/IdsSensorErrorDomain.hpp"
#include "src/libecu/Skeleton.hpp"
#include "tests/check.hpp"

namespace
{

/**
 * @brief Whether an error result carries the expected libecu code.
 * @param[in] erro     Error code produced.
 * @param[in] esperado Code expected.
 * @return Whether they match, domain included.
 */
bool EhCodigo(const ara::core::ErrorCode &erro, ids::ecu::EcuErrc esperado)
{
    return ((erro.Value() == static_cast<ara::core::ErrorDomain::CodeType>(esperado)) &&
            (erro.Domain() == ids::ecu::GetEcuErrorDomain()));
}

ids::ecu::SkeletonConfig ConfigBoa()
{
    ids::ecu::SkeletonConfig c;
    c.appName  = "service-sample";
    c.service  = 0x1234U;
    c.instance = 0x5678U;
    return c;
}

} // namespace

int main()
{
    /* -------------------------------------------- validacao, sem pilha nenhuma */
    chk::Grupo("Skeleton::Create: validacao da configuracao");
    {
        ids::ecu::SkeletonConfig semNome = ConfigBoa();
        semNome.appName.clear();
        const ara::core::Result<std::unique_ptr<ids::ecu::Skeleton>> a = ids::ecu::Skeleton::Create(semNome);
        chk::Checa(!a.HasValue(), "nome de aplicacao vazio e recusado");
        chk::Checa(!a.HasValue() && EhCodigo(a.Error(), ids::ecu::EcuErrc::InvalidConfiguration),
                   "e o codigo e InvalidConfiguration, no dominio da libecu");

        ids::ecu::SkeletonConfig semServico = ConfigBoa();
        semServico.service = 0U;
        chk::Checa(!ids::ecu::Skeleton::Create(semServico).HasValue(), "servico zero e recusado");

        ids::ecu::SkeletonConfig semInstancia = ConfigBoa();
        semInstancia.instance = 0U;
        chk::Checa(!ids::ecu::Skeleton::Create(semInstancia).HasValue(), "instancia zero e recusada");
    }

    chk::Grupo("Proxy::Create: validacao da configuracao");
    {
        ids::ecu::ProxyConfig c;
        c.appName  = "client-sample";
        c.service  = 0x1234U;
        c.instance = 0x5678U;

        ids::ecu::ProxyConfig semNome = c;
        semNome.appName.clear();
        const ara::core::Result<std::unique_ptr<ids::ecu::Proxy>> a = ids::ecu::Proxy::Create(semNome);
        chk::Checa(!a.HasValue(), "nome de aplicacao vazio e recusado");
        chk::Checa(!a.HasValue() && EhCodigo(a.Error(), ids::ecu::EcuErrc::InvalidConfiguration),
                   "e o codigo e InvalidConfiguration");

        ids::ecu::ProxyConfig semServico = c;
        semServico.service = 0U;
        chk::Checa(!ids::ecu::Proxy::Create(semServico).HasValue(), "servico zero e recusado");

        ids::ecu::ProxyConfig semInstancia = c;
        semInstancia.instance = 0U;
        chk::Checa(!ids::ecu::Proxy::Create(semInstancia).HasValue(), "instancia zero e recusada");
    }

    chk::Grupo("mensagens do dominio");
    {
        const ara::core::ErrorCode rota = ara::core::ErrorCode(ids::ecu::EcuErrc::ServiceDiscoveryNotReady);
        chk::Checa(std::string(rota.Domain().Name()) == "LibEcu", "o dominio se identifica");
        chk::Checa(std::string(rota.Message()).find("multicast") != std::string::npos,
                   "a mensagem do SD cita a rota multicast, que e a causa real");
        const ara::core::ErrorCode desconhecido = ara::core::ErrorCode(static_cast<ids::ecu::EcuErrc>(99));
        chk::Checa(std::string(desconhecido.Message()).find("unknown") == 0U,
                   "codigo nao mapeado cai na mensagem generica, nunca em nullptr");

        /* Os dois dominios do projeto NAO podem colidir: ambos usam o valor 1
         * para o seu primeiro erro, e e o dominio que os distingue. Se os
         * identificadores fossem iguais, um ArtefactNotFound do ids_sensor
         * compararia igual a um ApplicationCreateFailed da libecu. */
        const ara::core::ErrorCode daLibecu = ara::core::ErrorCode(ids::ecu::EcuErrc::ApplicationCreateFailed);
        const ara::core::ErrorCode doSensor = ara::core::ErrorCode(ids::IdsSensorErrc::ArtefactNotFound);
        chk::ChecaIgual(daLibecu.Value(), doSensor.Value(), "os dois erros tem o MESMO valor numerico");
        chk::Checa(daLibecu != doSensor, "mas comparam diferentes, porque o dominio distingue");
        chk::Checa(daLibecu.Domain().Id() != doSensor.Domain().Id(), "os identificadores de dominio diferem");
    }

    /* --------------------------------- casos que precisam de uma Skeleton viva */
    chk::Grupo("Skeleton: operacoes antes de Start()");
    const ara::core::Result<std::unique_ptr<ids::ecu::Skeleton>> criada = ids::ecu::Skeleton::Create(ConfigBoa());
    if (!criada.HasValue())
    {
        std::printf("  PULADO: Skeleton::Create falhou (%s).\n", criada.Error().Message());
        std::printf("          Estes casos precisam de VSOMEIP_CONFIGURATION valida; rode dentro\n");
        std::printf("          da imagem someip-base. Nao estao sendo contados como aprovados.\n");
        return chk::Veredito("test_libecu (parcial)");
    }
    ids::ecu::Skeleton &sk = *criada.Value();

    {
        const ara::core::Result<void> oferta = sk.Offer();
        chk::Checa(!oferta.HasValue(), "Offer() antes de Start() e recusado");
        chk::Checa(!oferta.HasValue() && EhCodigo(oferta.Error(), ids::ecu::EcuErrc::NotStarted),
                   "e o codigo e NotStarted");

        const ara::core::Result<void> espera = sk.WaitUntilRegistered(std::chrono::milliseconds(1));
        chk::Checa(!espera.HasValue() && EhCodigo(espera.Error(), ids::ecu::EcuErrc::NotStarted),
                   "WaitUntilRegistered() antes de Start() da NotStarted, nao timeout");

        chk::Checa(!sk.IsRegistered(), "IsRegistered() e falso antes de Start()");
    }

    chk::Grupo("Skeleton::OfferEvent: validacao");
    {
        const std::array<ids::ecu::EventgroupId, 1> grupos = {{0x4455U}};
        const ids::ecu::EventgroupIds               vista(grupos.data(), grupos.size());

        const ara::core::Result<void> zero = sk.OfferEvent(0U, vista, true, std::chrono::milliseconds::zero());
        chk::Checa(!zero.HasValue() && EhCodigo(zero.Error(), ids::ecu::EcuErrc::InvalidConfiguration),
                   "evento zero e recusado");

        const ara::core::Result<void> semGrupo =
            sk.OfferEvent(0x8777U, ids::ecu::EventgroupIds(), true, std::chrono::milliseconds::zero());
        chk::Checa(!semGrupo.HasValue() && EhCodigo(semGrupo.Error(), ids::ecu::EcuErrc::InvalidConfiguration),
                   "lista de eventgroups vazia e recusada");

        const ara::core::Result<void> ok = sk.OfferEvent(0x8777U, vista, true, std::chrono::milliseconds::zero());
        chk::Checa(ok.HasValue(), "evento valido e declarado");
    }

    chk::Grupo("Skeleton::Send: a ordem das verificacoes importa");
    {
        const std::vector<std::uint8_t> pequeno(8U, 0xAAU);
        const std::vector<std::uint8_t> grande(ids::ecu::kMaxPayloadBytes + 1U, 0xBBU);

        /* PayloadTooLarge e verificado ANTES de NotStarted, de proposito: um
         * payload que nunca vai caber e erro de programacao do chamador, e nao
         * deve ficar escondido atras do estado da pilha. */
        const ara::core::Result<void> demais =
            sk.Send(0x8777U, ids::ecu::ByteView(grande.data(), grande.size()));
        chk::Checa(!demais.HasValue() && EhCodigo(demais.Error(), ids::ecu::EcuErrc::PayloadTooLarge),
                   "payload acima do maximo da PayloadTooLarge mesmo sem Start()");

        const ara::core::Result<void> semStart =
            sk.Send(0x8777U, ids::ecu::ByteView(pequeno.data(), pequeno.size()));
        chk::Checa(!semStart.HasValue() && EhCodigo(semStart.Error(), ids::ecu::EcuErrc::NotStarted),
                   "payload que cabe, antes de Start(), da NotStarted");
    }

    chk::Grupo("Skeleton: ciclo de vida");
    {
        const ara::core::Result<void> um = sk.Start();
        chk::Checa(um.HasValue(), "Start() funciona uma vez");

        const ara::core::Result<void> dois = sk.Start();
        chk::Checa(!dois.HasValue() && EhCodigo(dois.Error(), ids::ecu::EcuErrc::AlreadyStarted),
                   "Start() de novo da AlreadyStarted");

        /* Sem routing manager no ar, o registro nao vem: o prazo curto deve
         * expirar com RegistrationTimeout em vez de travar para sempre. */
        const ara::core::Result<void> prazo = sk.WaitUntilRegistered(std::chrono::milliseconds(300));
        if (prazo.HasValue())
        {
            std::printf("  (registrou: ha routing manager no ar neste ambiente)\n");
            const ara::core::Result<void> envio = sk.Send(0x9999U, ids::ecu::ByteView());
            chk::Checa(!envio.HasValue() && EhCodigo(envio.Error(), ids::ecu::EcuErrc::EventNotOffered),
                       "Send() em evento nao declarado da EventNotOffered");
        }
        else
        {
            chk::Checa(EhCodigo(prazo.Error(), ids::ecu::EcuErrc::RegistrationTimeout),
                       "sem routing manager, a espera expira com RegistrationTimeout");
        }

        /* StopOffer e seguro mesmo sem oferta no ar. */
        sk.StopOffer();
        chk::Checa(true, "StopOffer() sem oferta no ar nao quebra");
    }

    return chk::Veredito("test_libecu");
}
