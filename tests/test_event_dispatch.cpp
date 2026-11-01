/* Teste do caminho de saida do sensor: contexto serializado, resolucao do
 * especificador e despacho por classe.
 *
 * Tres coisas aqui sao contrato e nao detalhe de implementacao:
 *
 *   1. TrafficClass::Normal NAO levanta evento. E a maioria esmagadora dos
 *      pacotes; se levantasse, o IdsM afogaria.
 *   2. A identidade do evento vem do InstanceSpecifier dado na CONSTRUCAO, nao
 *      de um parametro da chamada. Por isso existe um reporter por classe.
 *   3. A configuracao e OBRIGATORIA: um especificador que nao esta na lista
 *      configurada nao resolve, e o Create falha. Sem isso o Create nao teria
 *      como falhar e o seu Result estaria declarando um erro que ninguem produz.
 *
 * ⚠️ A lista configurada e lida UMA vez por processo, na primeira resolucao, e
 * fica em cache dali em diante. Isso e deliberado: a configuracao de um processo
 * em execucao nao muda por baixo dele, e reler por reporter faria a construcao
 * depender de timing de sistema de arquivos.
 *
 * A consequencia para este teste e que "sem configuracao" e "com configuracao"
 * NAO cabem no mesmo processo. A primeira versao deste arquivo tentava os dois
 * em sequencia e o Create positivo falhava, porque a lista vazia do primeiro
 * caso ja estava em cache. Por isso o caso sem configuracao roda num PROCESSO
 * FILHO: o binario se reexecuta com o argumento "sem-config" e com a variavel de
 * ambiente removida.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "src/ara/core/CoreErrorDomain.hpp"
#include "src/ids_sensor/SecurityEventDispatcher.hpp"
#include "tests/check.hpp"

namespace
{

const char *const kPrefixo = "ids_sensor/SecurityEvents";

/**
 * @brief Caminho de apoio dentro de /tmp.
 * @param[in] nome Nome do arquivo.
 * @return O caminho completo.
 */
std::string Tmp(const char *nome)
{
    return std::string("/tmp/") + nome;
}

/**
 * @brief Escreve a lista de eventos configurados.
 * @param[in] caminho Arquivo a criar.
 * @param[in] prefixo Prefixo das portas.
 * @param[in] quantos Quantos dos quatro eventos incluir.
 * @return Whether the file could be written.
 */
bool EscreveConfig(const std::string &caminho, const char *prefixo, std::size_t quantos)
{
    static const char *const sufixos[] = {"/DenialOfService", "/MalformedPayload", "/ManInTheMiddleSingle",
                                          "/ManInTheMiddleMulti"};
    std::ofstream            out(caminho.c_str());
    if (!out.is_open())
    {
        return false;
    }
    out << "# gerado por tests/test_event_dispatch.cpp\n";
    for (std::size_t i = 0U; (i < quantos) && (i < 4U); ++i)
    {
        out << prefixo << sufixos[i] << "\n";
    }
    return true;
}

/**
 * @brief Le o sink inteiro.
 * @param[in] caminho Arquivo do sink.
 * @return As linhas, na ordem.
 */
std::vector<std::string> LeSink(const std::string &caminho)
{
    std::vector<std::string> linhas;
    std::ifstream            in(caminho.c_str());
    std::string              linha;
    while (std::getline(in, linha))
    {
        if (!linha.empty())
        {
            linhas.push_back(linha);
        }
    }
    return linhas;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string config = Tmp("test_event_dispatch_cfg.txt");
    const std::string sink   = Tmp("test_event_dispatch_sink.txt");

    /* ------------------------------------------- modo de processo filho */
    if ((argc == 2) && (std::string(argv[1]) == "sem-config"))
    {
        /* Processo proprio, com a variavel removida pelo pai antes do exec: aqui
         * a lista configurada e lida vazia na primeira resolucao, que e o que se
         * quer medir. */
        chk::Grupo("filho: sem ARA_IDSM_CONFIGURATION");
        const ara::core::Result<std::unique_ptr<ids::IdsmDispatcher>> r = ids::IdsmDispatcher::Create(kPrefixo);
        chk::Checa(!r.HasValue(), "o Create FALHA quando nao ha configuracao");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::InvalidConfiguration),
                            "  com InvalidConfiguration");
        }
        return chk::Veredito("test_event_dispatch (filho)");
    }

    static_cast<void>(std::remove(sink.c_str()));

    /* ------------------------------------------------------- serializacao */
    chk::Grupo("SecurityEventContext::Serialize");
    {
        ids::SecurityEventContext ctx;
        ctx.srcIp      = 0xAC1E000BU; /* 172.30.0.11 */
        ctx.serviceId  = 0x1234U;
        ctx.methodId   = 0x8777U;
        ctx.confidence = 1.0F; /* IEEE-754: 0x3F800000 */

        const ids::SecurityEventContext::Buffer b = ctx.Serialize();
        chk::ChecaIgual(static_cast<long long>(b.size()), 12, "sao 12 bytes");
        chk::Checa((b[0] == 0xACU) && (b[1] == 0x1EU) && (b[2] == 0x00U) && (b[3] == 0x0BU),
                   "endereco em ordem de rede, byte mais significativo primeiro");
        chk::Checa((b[4] == 0x12U) && (b[5] == 0x34U), "service id em ordem de rede");
        chk::Checa((b[6] == 0x87U) && (b[7] == 0x77U), "method id em ordem de rede");
        chk::Checa((b[8] == 0x3FU) && (b[9] == 0x80U) && (b[10] == 0x00U) && (b[11] == 0x00U),
                   "confianca como padrao de bits IEEE-754, em ordem de rede");
    }

    /* ------------------------------------------------- resolucao e config */
    chk::Grupo("IdsmDispatcher::Create: a configuracao e obrigatoria");
    {
        /* O caso sem configuracao roda num processo FILHO, porque a lista fica em
         * cache no processo e uma leitura vazia aqui envenenaria todos os casos
         * seguintes. `env -u` remove a variavel so para o filho. */
        const std::string comando = std::string("env -u ARA_IDSM_CONFIGURATION '") + argv[0] + "' sem-config";
        const int         saida   = std::system(comando.c_str());
        chk::ChecaIgual(saida, 0, "o filho sem ARA_IDSM_CONFIGURATION verificou que o Create falha");
        if (saida != 0)
        {
            std::printf("        (o filho saiu com %d; a saida dele esta acima)\n", saida);
        }
    }

    chk::Checa(EscreveConfig(config, kPrefixo, 4U), "lista com os 4 eventos escrita");
    static_cast<void>(::setenv("ARA_IDSM_CONFIGURATION", config.c_str(), 1));
    static_cast<void>(::setenv("ARA_IDSM_SINK", sink.c_str(), 1));

    {
        const ara::core::Result<std::unique_ptr<ids::IdsmDispatcher>> r =
            ids::IdsmDispatcher::Create("ids_sensor//SecurityEvents");
        chk::Checa(!r.HasValue(), "prefixo com separador duplicado e recusado");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::InvalidConfiguration),
                            "  com InvalidConfiguration");
        }
    }
    {
        const ara::core::Result<std::unique_ptr<ids::IdsmDispatcher>> r = ids::IdsmDispatcher::Create("");
        chk::Checa(!r.HasValue(), "prefixo vazio e recusado");
    }
    {
        const ara::core::Result<std::unique_ptr<ids::IdsmDispatcher>> r =
            ids::IdsmDispatcher::Create("ids_sensor/Outra/Coisa");
        chk::Checa(!r.HasValue(), "prefixo bem formado mas NAO configurado e recusado");
    }

    /* ---------------------------------------------- especificador em si */
    chk::Grupo("InstanceSpecifier::Create: o que e um caminho valido");
    {
        chk::Checa(ara::core::InstanceSpecifier::Create("a/b/c").HasValue(), "a/b/c vale");
        chk::Checa(ara::core::InstanceSpecifier::Create("_x9/Y_2").HasValue(), "sublinhado e digito no meio valem");
        chk::Checa(!ara::core::InstanceSpecifier::Create("/a").HasValue(), "separador inicial nao vale");
        chk::Checa(!ara::core::InstanceSpecifier::Create("a/").HasValue(), "separador final nao vale");
        chk::Checa(!ara::core::InstanceSpecifier::Create("a//b").HasValue(), "separador duplicado nao vale");
        chk::Checa(!ara::core::InstanceSpecifier::Create("9a/b").HasValue(), "segmento comecando com digito nao vale");
        chk::Checa(!ara::core::InstanceSpecifier::Create("a b/c").HasValue(), "espaco nao vale");
        chk::Checa(!ara::core::InstanceSpecifier::Create("").HasValue(), "vazio nao vale");

        const ara::core::Result<ara::core::InstanceSpecifier> ruim = ara::core::InstanceSpecifier::Create("9a");
        if (!ruim.HasValue())
        {
            chk::ChecaIgual(ruim.Error().Value(),
                            static_cast<int>(ara::core::CoreErrc::kInvalidMetaModelShortname),
                            "segmento ruim da kInvalidMetaModelShortname");
            chk::Checa(ruim.Error().Domain().Id() == ara::core::CoreErrorDomain::DOMAIN_ID,
                       "e vem do dominio do ara::core, nao do ids_sensor");
        }
        const ara::core::Result<ara::core::InstanceSpecifier> vazio = ara::core::InstanceSpecifier::Create("");
        if (!vazio.HasValue())
        {
            chk::ChecaIgual(vazio.Error().Value(), static_cast<int>(ara::core::CoreErrc::kInvalidMetaModelPath),
                            "caminho vazio da kInvalidMetaModelPath");
        }
    }

    /* ------------------------------------------------------- despacho */
    chk::Grupo("Dispatch: uma classe, um evento");
    {
        ara::core::Result<std::unique_ptr<ids::IdsmDispatcher>> r = ids::IdsmDispatcher::Create(kPrefixo);
        chk::Checa(r.HasValue(), "com os 4 eventos configurados, o Create passa");
        if (r.HasValue())
        {
            ids::IdsmDispatcher      &d = *r.Value();
            ids::SecurityEventContext ctx;
            ctx.srcIp      = 0x0A000001U;
            ctx.serviceId  = 0x1001U;
            ctx.methodId   = 0x0001U;
            ctx.confidence = 0.75F;

            chk::Checa(!d.Dispatch(ids::TrafficClass::Normal, ctx, 1000U), "Normal NAO levanta evento");
            chk::Checa(d.Dispatch(ids::TrafficClass::Dos, ctx, 1001U), "Dos levanta");
            chk::Checa(d.Dispatch(ids::TrafficClass::Fuzzy, ctx, 1002U), "Fuzzy levanta");
            chk::Checa(d.Dispatch(ids::TrafficClass::MitmSingle, ctx, 1003U), "MitmSingle levanta");
            chk::Checa(d.Dispatch(ids::TrafficClass::MitmMulti, ctx, 1004U), "MitmMulti levanta");

            const std::vector<std::string> linhas = LeSink(sink);
            chk::ChecaIgual(static_cast<long long>(linhas.size()), 4, "o sink tem 4 linhas, nao 5");

            if (linhas.size() == 4U)
            {
                /* Cada classe tem de ir para o SEU caminho de evento. Trocar dois
                 * reporters de lugar e um erro que nenhum teste de contagem pega. */
                chk::Checa(linhas[0].find("/DenialOfService") != std::string::npos, "a 1a linha e do DenialOfService");
                chk::Checa(linhas[1].find("/MalformedPayload") != std::string::npos, "a 2a e do MalformedPayload");
                chk::Checa(linhas[2].find("/ManInTheMiddleSingle") != std::string::npos,
                           "a 3a e do ManInTheMiddleSingle");
                chk::Checa(linhas[3].find("/ManInTheMiddleMulti") != std::string::npos, "a 4a e do ManInTheMiddleMulti");
                chk::Checa(linhas[0].find("ts=1001") != std::string::npos, "o timestamp dado chega ao sink");
                chk::Checa(linhas[0].find("context=0a0000011001000") != std::string::npos,
                           "e o contexto serializado chega em hexadecimal");
            }
        }
    }

    static_cast<void>(std::remove(config.c_str()));
    static_cast<void>(std::remove(sink.c_str()));
    return chk::Veredito("test_event_dispatch");
}
