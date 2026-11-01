/* Teste do Sensor: a cadeia inteira, com as pontas trocadas por duplas.
 *
 * O que este teste existe para fixar nao e aritmetica -- isso os testes de cada
 * estagio ja cobrem. E a CONTABILIDADE da cadeia, que e o que vai para a
 * decomposicao de custo do artigo:
 *
 *   1. quadro que o parser rejeita NAO conta como pacote: conta em `malformed`
 *      e paga so o estagio de parse. Se contasse junto, uma captura cheia de
 *      trafego que nao e SOME/IP puxaria as outras tres medias para baixo na
 *      proporcao do lixo, e a decomposicao publicada estaria errada;
 *   2. so a classe que levanta evento incrementa `events`;
 *   3. o evento e estampado com o relogio do CENARIO, nao com o relogio da
 *      medicao: a mesma captura reproduzida amanha tem de dar os mesmos tempos.
 *
 * A fonte de pacotes e o despachante sao duplas de teste, o que e justamente o
 * motivo de os dois serem interfaces.
 *
 * Uso:
 *   test_sensor <tables.bin>
 */
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "src/ids_sensor/Sensor.hpp"
#include "tests/check.hpp"

namespace
{

/**
 * @brief Fonte de pacotes servida de um vetor em memoria.
 */
class FonteDeVetor final : public ids::IPacketSource
{
  public:
    /**
     * @brief Acrescenta um quadro.
     * @param[in] bytes Bytes do quadro.
     * @param[in] ts    Instante no relogio do cenario.
     * @return None.
     */
    void Acrescenta(const std::vector<std::uint8_t> &bytes, double ts)
    {
        m_quadros.push_back(bytes);
        m_instantes.push_back(ts);
    }

    /**
     * @copydoc ::ids::IPacketSource::Next()
     */
    ara::core::Optional<ids::Frame> Next() override
    {
        ara::core::Optional<ids::Frame> fora;
        if (m_proximo < m_quadros.size())
        {
            ids::Frame f;
            f.bytes = ids::PayloadView(m_quadros[m_proximo].data(), m_quadros[m_proximo].size());
            f.ts    = m_instantes[m_proximo];
            ++m_proximo;
            fora = ara::core::Optional<ids::Frame>(f);
        }
        return fora;
    }

  private:
    std::vector<std::vector<std::uint8_t>> m_quadros;   ///< Quadros, na ordem.
    std::vector<double>                    m_instantes; ///< Instante de cada quadro.
    std::size_t                            m_proximo = 0U; ///< Proximo a entregar.
};

/**
 * @brief Despachante que só guarda o que recebeu.
 */
class ColetorDeEventos final : public ids::ISecurityEventDispatcher
{
  public:
    /**
     * @copydoc ::ids::ISecurityEventDispatcher::Dispatch(ids::TrafficClass, const ids::SecurityEventContext &, ids::SecurityEventTimestamp)
     */
    bool Dispatch(ids::TrafficClass trafficClass, const ids::SecurityEventContext &context,
                  ids::SecurityEventTimestamp timestamp) override
    {
        if (trafficClass == ids::TrafficClass::Normal)
        {
            return false;
        }
        classes.push_back(trafficClass);
        contextos.push_back(context);
        instantes.push_back(timestamp);
        return true;
    }

    std::vector<ids::TrafficClass>           classes;   ///< Classes recebidas, na ordem.
    std::vector<ids::SecurityEventContext>   contextos; ///< Contextos recebidos.
    std::vector<ids::SecurityEventTimestamp> instantes; ///< Instantes recebidos.
};

/**
 * @brief Despachante que levanta evento para TODA classe, inclusive a benigna.
 * @details Serve a um caso só: provar que o contador `events` do Sensor segue o que o despachante
 *          responde, e não uma regra própria sobre a classe. Se o Sensor tivesse a sua própria
 *          ideia de "Normal não conta", este coletor e o de cima dariam o mesmo número.
 */
class ColetorQueAceitaTudo final : public ids::ISecurityEventDispatcher
{
  public:
    /**
     * @copydoc ::ids::ISecurityEventDispatcher::Dispatch(ids::TrafficClass, const ids::SecurityEventContext &, ids::SecurityEventTimestamp)
     */
    bool Dispatch(ids::TrafficClass, const ids::SecurityEventContext &context,
                  ids::SecurityEventTimestamp timestamp) override
    {
        ++quantos;
        contextos.push_back(context);
        instantes.push_back(timestamp);
        return true;
    }

    std::size_t                              quantos = 0U; ///< Quantas vezes foi chamado.
    std::vector<ids::SecurityEventContext>   contextos;     ///< Contextos recebidos.
    std::vector<ids::SecurityEventTimestamp> instantes;     ///< Instantes recebidos.
};

/* ---------------------------------------------------- quadros de apoio */

std::vector<std::uint8_t> Be16(std::uint16_t v)
{
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(v >> 8U));
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    return b;
}

void Junta(std::vector<std::uint8_t> &dentro, const std::vector<std::uint8_t> &mais)
{
    dentro.insert(dentro.end(), mais.begin(), mais.end());
}

/**
 * @brief Monta um quadro Ethernet/IPv4/UDP/SOME/IP.
 * @param[in] sport Porta de origem.
 * @param[in] corpo Corpo SOME/IP.
 * @return Os bytes do quadro.
 */
std::vector<std::uint8_t> QuadroSomeip(std::uint16_t sport, const std::vector<std::uint8_t> &corpo)
{
    std::vector<std::uint8_t> someip;
    Junta(someip, Be16(0x1234U)); /* service */
    Junta(someip, Be16(0x8777U)); /* method  */
    const std::uint32_t len = static_cast<std::uint32_t>(8U + corpo.size());
    someip.push_back(static_cast<std::uint8_t>(len >> 24U));
    someip.push_back(static_cast<std::uint8_t>((len >> 16U) & 0xFFU));
    someip.push_back(static_cast<std::uint8_t>((len >> 8U) & 0xFFU));
    someip.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    Junta(someip, Be16(0x0001U)); /* client  */
    Junta(someip, Be16(0x0002U)); /* session */
    someip.push_back(0x01U);      /* protocol version  */
    someip.push_back(0x01U);      /* interface version */
    someip.push_back(0x02U);      /* NOTIFICATION      */
    someip.push_back(0x00U);      /* return code       */
    Junta(someip, corpo);

    std::vector<std::uint8_t> udp;
    Junta(udp, Be16(sport));
    Junta(udp, Be16(30509U));
    Junta(udp, Be16(static_cast<std::uint16_t>(8U + someip.size())));
    Junta(udp, Be16(0U));
    Junta(udp, someip);

    std::vector<std::uint8_t> ip;
    ip.push_back(0x45U);
    ip.push_back(0x00U);
    Junta(ip, Be16(static_cast<std::uint16_t>(20U + udp.size())));
    Junta(ip, Be16(0x1234U));
    Junta(ip, Be16(0U));
    ip.push_back(64U);
    ip.push_back(17U); /* UDP */
    Junta(ip, Be16(0U));
    const std::uint8_t a[4] = {172U, 30U, 0U, 11U};
    const std::uint8_t b[4] = {172U, 30U, 0U, 12U};
    for (std::size_t i = 0U; i < 4U; ++i)
    {
        ip.push_back(a[i]);
    }
    for (std::size_t i = 0U; i < 4U; ++i)
    {
        ip.push_back(b[i]);
    }
    Junta(ip, udp);

    std::vector<std::uint8_t> eth(12U, 0xAAU);
    Junta(eth, Be16(0x0800U));
    Junta(eth, ip);
    return eth;
}

/**
 * @brief Quadro ARP, que o parser descarta.
 * @return Os bytes do quadro.
 */
std::vector<std::uint8_t> QuadroArp()
{
    std::vector<std::uint8_t> eth(12U, 0xBBU);
    Junta(eth, Be16(0x0806U));
    for (std::size_t i = 0U; i < 40U; ++i)
    {
        eth.push_back(0U);
    }
    return eth;
}

/**
 * @brief Modelos de bytes neutros, só para o extrator ter com que pontuar.
 * @return Os modelos, ou nada quando a construção falhar.
 */
std::unique_ptr<ids::ByteModels> ModelosNeutros()
{
    ids::ByteModel::LogProbTable tabela = ids::ByteModel::LogProbTable();
    for (std::size_t p = 0U; p < ids::kMaxPositions; ++p)
    {
        for (std::size_t v = 0U; v < ids::kByteValues; ++v)
        {
            /* log(1/256), uniforme: o valor não importa, só que seja finito. */
            tabela[p][v] = -5.5451774444795625;
        }
    }
    ara::core::Result<ids::ByteModel> someip = ids::ByteModel::Create(tabela, ids::kSomeipPositions);
    ara::core::Result<ids::ByteModel> sd     = ids::ByteModel::Create(tabela, ids::kSdPositions);
    ara::core::Result<ids::ByteModel> l4     = ids::ByteModel::Create(tabela, ids::kL4Positions);
    if (!someip.HasValue() || !sd.HasValue() || !l4.HasValue())
    {
        return std::unique_ptr<ids::ByteModels>();
    }
    return ids::ByteModels::Create(someip.Value(), sd.Value(), l4.Value());
}

ids::BoundsVec Preenche(double valor)
{
    ids::BoundsVec v = ids::BoundsVec();
    for (std::size_t i = 0U; i < ids::kNumFeatures; ++i)
    {
        v[i] = valor;
    }
    return v;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::printf("uso: %s <tables.bin>\n", argv[0]);
        std::printf("  gere com: bash experiments/bench-inference/make_probe.sh\n");
        return 2;
    }

    const std::unique_ptr<ids::ByteModels> modelos = ModelosNeutros();
    if (!modelos)
    {
        std::printf("ERRO montando os modelos de bytes\n");
        return 1;
    }

    const ids::StreamerConfig cfg;
    ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> streamer = ids::FeatureStreamer::Create(*modelos, cfg);
    ara::core::Result<std::unique_ptr<ids::CompactClassifier>> clf    = ids::CompactClassifier::Create(argv[1]);
    if (!streamer.HasValue() || !clf.HasValue())
    {
        std::printf("ERRO montando a cadeia: %s\n",
                    streamer.HasValue() ? clf.Error().Message() : streamer.Error().Message());
        return 1;
    }

    const ids::Normalizer norm = ids::Normalizer::FromBounds(Preenche(0.0), Preenche(100.0));
    ids::SomeipParser     parser;

    /* -------------------------------------------------------- contabilidade */
    chk::Grupo("Sensor: a contabilidade da cadeia");
    {
        ColetorDeEventos coletor;
        ids::Sensor      sensor(parser, *streamer.Value(), norm, *clf.Value(), coletor);

        FonteDeVetor fonte;
        const std::vector<std::uint8_t> corpo(8U, 0xAAU);
        fonte.Acrescenta(QuadroSomeip(40000U, corpo), 1.000);
        fonte.Acrescenta(QuadroArp(), 1.100);                    /* descartado */
        fonte.Acrescenta(QuadroSomeip(40000U, corpo), 1.200);
        fonte.Acrescenta(QuadroArp(), 1.300);                    /* descartado */
        fonte.Acrescenta(QuadroSomeip(40001U, corpo), 1.400);

        sensor.Run(fonte);
        const ids::StageTiming &t = sensor.Timing();

        chk::ChecaIgual(static_cast<long long>(t.packets), 3, "3 pacotes contados");
        chk::ChecaIgual(static_cast<long long>(t.malformed), 2, "2 quadros descartados, contados APARTE");
        chk::Checa(t.parseUs > 0.0, "o estagio de parse acumulou tempo");
        chk::Checa(t.featureUs > 0.0, "o de features tambem");
        chk::Checa(t.classifyUs > 0.0, "o de classificacao tambem");
        chk::Checa(t.decideUs >= 0.0, "e o de decisao nao e negativo");
        chk::ChecaIgual(static_cast<long long>(t.events), static_cast<long long>(coletor.classes.size()),
                        "o contador de eventos bate com o que o despachante recebeu");

        /* Se algum evento subiu, o contexto e o instante tem de vir do pacote. */
        if (!coletor.contextos.empty())
        {
            chk::ChecaIgual(static_cast<long long>(coletor.contextos[0].srcIp), 0xAC1E000BLL,
                            "o endereco de origem do pacote chega no contexto");
            chk::ChecaIgual(static_cast<long long>(coletor.contextos[0].serviceId), 0x1234,
                            "e o service id tambem");
            chk::ChecaIgual(static_cast<long long>(coletor.instantes[0]), 1000000LL,
                            "e o instante e o do CENARIO, em microssegundos (1,0 s -> 1000000)");
        }
        else
        {
            std::printf("  (o modelo de prova classificou tudo como Normal; os campos do contexto\n");
            std::printf("   sao conferidos pelo caso do coletor que aceita tudo, abaixo)\n");
        }

        sensor.ResetTiming();
        chk::ChecaIgual(static_cast<long long>(sensor.Timing().packets), 0, "ResetTiming zera os contadores");
        chk::ChecaIgualF(sensor.Timing().parseUs, 0.0, "e zera os tempos");
    }

    /* ------------------------------------- quem decide levantar e o sink */
    chk::Grupo("quem decide levantar evento e o despachante, nao o Sensor");
    {
        streamer.Value()->Reset();
        ColetorQueAceitaTudo tudo;
        ids::Sensor          sensor(parser, *streamer.Value(), norm, *clf.Value(), tudo);

        FonteDeVetor fonte;
        const std::vector<std::uint8_t> corpo(8U, 0xCCU);
        for (std::size_t i = 0U; i < 4U; ++i)
        {
            fonte.Acrescenta(QuadroSomeip(40000U, corpo), 2.0 + (0.1 * static_cast<double>(i)));
        }
        sensor.Run(fonte);

        chk::ChecaIgual(static_cast<long long>(sensor.Timing().packets), 4, "4 pacotes");
        chk::ChecaIgual(static_cast<long long>(tudo.quantos), 4, "o despachante foi chamado para os 4");
        chk::ChecaIgual(static_cast<long long>(sensor.Timing().events), 4,
                        "e os 4 contaram como evento, porque o despachante disse que sim");

        /* E aqui, com um despachante que aceita tudo, os campos do contexto ficam
         * conferidos independentemente de que classe o modelo de prova escolheu. O
         * caso acima nao conseguia fazer isso: o modelo de prova foi treinado em
         * dados aleatorios e classificou todos os pacotes como Normal, que nao
         * levanta evento. */
        if (tudo.contextos.size() == 4U)
        {
            chk::ChecaIgual(static_cast<long long>(tudo.contextos[0].srcIp), 0xAC1E000BLL,
                            "o endereco de origem do pacote chega no contexto (172.30.0.11)");
            chk::ChecaIgual(static_cast<long long>(tudo.contextos[0].serviceId), 0x1234,
                            "e o service id tambem");
            chk::ChecaIgual(static_cast<long long>(tudo.contextos[0].methodId), 0x8777, "e o method id tambem");
            chk::Checa((tudo.contextos[0].confidence > 0.0F) && (tudo.contextos[0].confidence <= 1.0F),
                       "e a confianca e uma probabilidade");
            /* 2,0 s no relogio do cenario -> 2000000 us. E o relogio do CENARIO, nao
             * o da medicao: a mesma captura reproduzida amanha da o mesmo numero. */
            chk::ChecaIgual(static_cast<long long>(tudo.instantes[0]), 2000000LL,
                            "e o instante e o do cenario, em microssegundos");
            chk::ChecaIgual(static_cast<long long>(tudo.instantes[3]), 2300000LL,
                            "e o do quarto pacote acompanha (2,3 s)");
        }
        else
        {
            chk::Checa(false, "esperava 4 contextos coletados");
        }
    }

    /* --------------------------------------------------- captura vazia */
    chk::Grupo("fonte vazia");
    {
        streamer.Value()->Reset();
        ColetorDeEventos coletor;
        ids::Sensor      sensor(parser, *streamer.Value(), norm, *clf.Value(), coletor);
        FonteDeVetor     vazia;
        sensor.Run(vazia);
        chk::ChecaIgual(static_cast<long long>(sensor.Timing().packets), 0, "nenhum pacote");
        chk::ChecaIgual(static_cast<long long>(sensor.Timing().malformed), 0, "nenhum descarte");
        chk::ChecaIgual(static_cast<long long>(sensor.Timing().events), 0, "nenhum evento");
    }

    return chk::Veredito("test_sensor");
}
