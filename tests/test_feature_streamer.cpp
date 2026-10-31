/* Teste UNITARIO do FeatureStreamer.
 *
 * O teste de paridade afirma que os valores batem com o Python, mas EXIGE que o
 * despejo nao aconteca, e portanto nunca exercita o teto de fluxos. Aqui o teto
 * e atingido de proposito, junto com os caminhos de erro e o Reset, que a
 * paridade tambem nao toca.
 */
#include <cstdio>
#include <vector>

#include "src/ids_sensor/FeatureStreamer.hpp"
#include "tests/check.hpp"

namespace
{

/// Indices das features, para os casos abaixo nao virarem numeros soltos.
constexpr std::size_t kIat        = 0U;
constexpr std::size_t kRepeatRate = 12U;
constexpr std::size_t kSomeipLen  = 13U;
constexpr std::size_t kL4Len      = 14U;
constexpr std::size_t kSrcDiv     = 15U;

/**
 * @brief Tabela neutra: log-probabilidade zero em toda posicao e valor.
 * @return A tabela.
 * @details Zerada para o conteudo nao interferir nos casos deste arquivo, que
 *          testam estado e janelas. A pontuacao em si tem o seu proprio teste.
 */
ids::ByteModel::LogProbTable TabelaNeutra()
{
    ids::ByteModel::LogProbTable t;
    for (std::size_t pos = 0U; pos < ids::kMaxPositions; ++pos)
    {
        t[pos].fill(0.0);
    }
    return t;
}

std::unique_ptr<ids::ByteModels> Modelos()
{
    const ids::ByteModel::LogProbTable t = TabelaNeutra();
    return ids::ByteModels::Create(ids::ByteModel::Create(t, ids::kSomeipPositions).Value(),
                                   ids::ByteModel::Create(t, ids::kSdPositions).Value(),
                                   ids::ByteModel::Create(t, ids::kL4Positions).Value());
}

/// Um pacote com os buffers que ele referencia, para as vistas nao morrerem.
struct Pacote
{
    ids::ParsedPacket         p;
    std::vector<std::uint8_t> l4;
    std::vector<std::uint8_t> someip;
    std::vector<std::uint8_t> sd;

    Pacote(double ts, std::uint32_t src, std::uint16_t sport, const std::vector<std::uint8_t> &corpo)
        : l4(), someip(corpo), sd()
    {
        l4.resize(16U);
        l4.insert(l4.end(), corpo.begin(), corpo.end());
        p.ts          = ts;
        p.flow.src    = src;
        p.flow.dst    = 0x0A000001U;
        p.flow.sport  = sport;
        p.flow.dport  = 30509U;
        p.flow.proto  = 17U;
        p.ipLen       = static_cast<std::uint16_t>(28U + l4.size());
        p.plL4        = ids::PayloadView(l4.data(), l4.size());
        p.plSomeip    = ids::PayloadView(someip.data(), someip.size());
        p.plSd        = ids::PayloadView(sd.data(), sd.size());
    }
};

} // namespace

int main()
{
    const std::unique_ptr<ids::ByteModels> modelos = Modelos();

    /* --------------------------------------------------------------- validacao */
    chk::Grupo("Create: validacao da configuracao");
    {
        ids::StreamerConfig c;
        chk::Checa(ids::FeatureStreamer::Create(*modelos, c).HasValue(), "configuracao padrao e aceita");

        const char *nomes[4] = {"recentPayloadWindow", "sourcePayloadWindow", "maxFlows", "maxSources"};
        for (std::size_t campo = 0U; campo < 4U; ++campo)
        {
            ids::StreamerConfig zero;
            if (campo == 0U) { zero.recentPayloadWindow = 0U; }
            else if (campo == 1U) { zero.sourcePayloadWindow = 0U; }
            else if (campo == 2U) { zero.maxFlows = 0U; }
            else { zero.maxSources = 0U; }

            const ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> r = ids::FeatureStreamer::Create(*modelos, zero);
            char texto[128];
            std::snprintf(texto, sizeof(texto), "%s igual a zero e recusado", nomes[campo]);
            chk::Checa(!r.HasValue(), texto);
            if (!r.HasValue())
            {
                std::snprintf(texto, sizeof(texto), "%s: o codigo e InvalidConfiguration", nomes[campo]);
                chk::Checa(r.Error().Value() == static_cast<int>(ids::IdsSensorErrc::InvalidConfiguration), texto);
            }
        }
    }

    /* ------------------------------------------------------- primeiro pacote */
    chk::Grupo("primeiro pacote de um fluxo");
    {
        ids::StreamerConfig                                      c;
        ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> r = ids::FeatureStreamer::Create(*modelos, c);
        ids::FeatureStreamer                                    &s = *r.Value();

        Pacote um(10.0, 1U, 40000U, {1U, 2U, 3U, 4U});
        const ara::core::Optional<ids::FeatureVec> v = s.Step(um.p);
        chk::Checa(v.has_value(), "produz vetor");
        chk::ChecaIgualF(v.value()[kIat], 0.0, "intervalo do primeiro pacote e zero, nao o timestamp");
        chk::ChecaIgualF(v.value()[kRepeatRate], 0.0, "repeticao do primeiro pacote e zero");
        chk::ChecaIgualF(v.value()[kSrcDiv], 0.0, "diversidade com janela de 1 e zero, nao 1");
        chk::ChecaIgualF(v.value()[kSomeipLen], 4.0, "tamanho do corpo someip");
        chk::ChecaIgualF(v.value()[kL4Len], 20.0, "tamanho do l4 inclui os 16 de cabecalho");
    }

    /* ------------------------------------------------------------------ Reset */
    chk::Grupo("Reset apaga o estado");
    {
        ids::StreamerConfig                                      c;
        ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> r = ids::FeatureStreamer::Create(*modelos, c);
        ids::FeatureStreamer                                    &s = *r.Value();

        Pacote a(10.0, 1U, 40000U, {1U});
        Pacote b(10.5, 1U, 40000U, {1U});
        static_cast<void>(s.Step(a.p));
        const ara::core::Optional<ids::FeatureVec> segundo = s.Step(b.p);
        chk::ChecaPerto(static_cast<double>(segundo.value()[kIat]), 0.5, 1e-6, "segundo pacote tem intervalo real");
        chk::Checa(segundo.value()[kRepeatRate] > 0.0F, "segundo pacote ve o primeiro na janela");

        s.Reset();
        Pacote c2(20.0, 1U, 40000U, {1U});
        const ara::core::Optional<ids::FeatureVec> depois = s.Step(c2.p);
        chk::ChecaIgualF(depois.value()[kIat], 0.0, "depois do Reset o fluxo volta a ser novo");
        chk::ChecaIgualF(depois.value()[kRepeatRate], 0.0, "depois do Reset a janela esta vazia");
    }

    /* -------------------------------------------------------- janelas isoladas */
    chk::Grupo("janelas sao por fluxo e por origem");
    {
        ids::StreamerConfig                                      c;
        ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> r = ids::FeatureStreamer::Create(*modelos, c);
        ids::FeatureStreamer                                    &s = *r.Value();

        const std::vector<std::uint8_t> mesmo = {9U};
        Pacote fluxoA1(1.0, 1U, 40000U, mesmo);
        Pacote fluxoA2(1.1, 1U, 40000U, mesmo);
        Pacote fluxoB1(1.2, 1U, 40001U, mesmo);   /* MESMA origem, outro fluxo */
        Pacote origemC(1.3, 2U, 40000U, mesmo);   /* outra origem */

        static_cast<void>(s.Step(fluxoA1.p));
        const ara::core::Optional<ids::FeatureVec> a2 = s.Step(fluxoA2.p);
        chk::ChecaIgualF(a2.value()[kRepeatRate], 1.0, "repeticao no mesmo fluxo chega a 1");

        const ara::core::Optional<ids::FeatureVec> b1 = s.Step(fluxoB1.p);
        chk::ChecaIgualF(b1.value()[kRepeatRate], 0.0, "fluxo novo tem janela de repeticao propria");
        /* A diversidade e por ORIGEM, e a origem 1 ja viu 3 payloads iguais:
         * 1 distinto / 3 na janela. */
        chk::ChecaPerto(static_cast<double>(b1.value()[kSrcDiv]), 1.0 / 3.0, 1e-6,
                        "diversidade e por origem, entao atravessa os dois fluxos");

        const ara::core::Optional<ids::FeatureVec> c1 = s.Step(origemC.p);
        chk::ChecaIgualF(c1.value()[kSrcDiv], 0.0, "origem nova tem janela de diversidade propria");
    }

    /* ----------------------------------------------------- payload vazio entra */
    chk::Grupo("payload vazio: entra na repeticao, nao na diversidade");
    {
        ids::StreamerConfig                                      c;
        ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> r = ids::FeatureStreamer::Create(*modelos, c);
        ids::FeatureStreamer                                    &s = *r.Value();

        const std::vector<std::uint8_t> vazio;
        const std::vector<std::uint8_t> cheio = {5U};
        Pacote v1(1.0, 1U, 40000U, vazio);
        Pacote v2(1.1, 1U, 40000U, vazio);
        Pacote cheio1(1.2, 1U, 40000U, cheio);

        static_cast<void>(s.Step(v1.p));
        static_cast<void>(s.Step(v2.p));
        const ara::core::Optional<ids::FeatureVec> depois = s.Step(cheio1.p);
        /* A janela de repeticao tem 2 vazios; nenhum casa com o cheio, entao 0/2. */
        chk::ChecaIgualF(depois.value()[kRepeatRate], 0.0, "vazios na janela nao casam com o cheio");
        /* A de diversidade recebeu so o cheio, entao tem 1 entrada e o guarda da 0. */
        chk::ChecaIgualF(depois.value()[kSrcDiv], 0.0, "vazios nao entraram na diversidade");
    }

    /* ---------------------------------------------------------------- despejo */
    chk::Grupo("despejo: o teto que o Python nao tem");
    {
        ids::StreamerConfig c;
        c.maxFlows   = 2U;
        c.maxSources = 2U;
        ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> r = ids::FeatureStreamer::Create(*modelos, c);
        ids::FeatureStreamer                                    &s = *r.Value();

        chk::ChecaIgual(static_cast<long long>(s.FlowEvictions()), 0, "comeca sem despejo");

        Pacote f1(1.0, 1U, 40001U, {1U});
        Pacote f2(1.1, 1U, 40002U, {1U});
        static_cast<void>(s.Step(f1.p));
        static_cast<void>(s.Step(f2.p));
        chk::ChecaIgual(static_cast<long long>(s.FlowEvictions()), 0, "dois fluxos cabem no teto de 2");

        Pacote f3(1.2, 1U, 40003U, {1U});
        static_cast<void>(s.Step(f3.p));
        chk::ChecaIgual(static_cast<long long>(s.FlowEvictions()), 1, "o terceiro fluxo despeja o mais antigo");

        /* O fluxo 1 foi despejado, entao ele volta a ser "novo": intervalo zero. */
        Pacote f1DeNovo(2.0, 1U, 40001U, {1U});
        const ara::core::Optional<ids::FeatureVec> v = s.Step(f1DeNovo.p);
        chk::ChecaIgualF(v.value()[kIat], 0.0, "fluxo despejado reaparece como novo");
        chk::ChecaIgual(static_cast<long long>(s.FlowEvictions()), 2, "e o reaparecimento despeja outro");

        /* Origens: o teto e independente do de fluxos. */
        chk::ChecaIgual(static_cast<long long>(s.SourceEvictions()), 0, "uma origem so, sem despejo de origem");
        Pacote o2(3.0, 2U, 40001U, {1U});
        Pacote o3(3.1, 3U, 40001U, {1U});
        static_cast<void>(s.Step(o2.p));
        static_cast<void>(s.Step(o3.p));
        chk::ChecaIgual(static_cast<long long>(s.SourceEvictions()), 1, "a terceira origem despeja a mais antiga");

        s.Reset();
        chk::ChecaIgual(static_cast<long long>(s.FlowEvictions()), 0, "Reset zera o contador de fluxos");
        chk::ChecaIgual(static_cast<long long>(s.SourceEvictions()), 0, "Reset zera o contador de origens");
    }

    /* ------------------------------------------------- janela de repeticao cheia */
    chk::Grupo("janela de repeticao transborda em maxlen");
    {
        ids::StreamerConfig c;
        c.recentPayloadWindow = 3U;
        ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> r = ids::FeatureStreamer::Create(*modelos, c);
        ids::FeatureStreamer                                    &s = *r.Value();

        const std::vector<std::uint8_t> p1 = {1U};
        const std::vector<std::uint8_t> p2 = {2U};

        /* Tres iguais, depois um diferente quatro vezes: a janela de 3 acaba sem
         * nenhum p1, e a taxa volta a zero. */
        Pacote a(1.0, 1U, 40000U, p1);
        Pacote b(1.1, 1U, 40000U, p1);
        Pacote c3(1.2, 1U, 40000U, p1);
        static_cast<void>(s.Step(a.p));
        static_cast<void>(s.Step(b.p));
        const ara::core::Optional<ids::FeatureVec> terceiro = s.Step(c3.p);
        chk::ChecaIgualF(terceiro.value()[kRepeatRate], 1.0, "janela cheia de iguais da 1");

        /* A rampa inteira, que e a prova de que a taxa e calculada ANTES do
         * append: no primeiro p2 a janela ainda e toda p1, e so no QUARTO p2 ela
         * esta toda p2. Um teto de 3 nao chega a 1 no terceiro, e foi exatamente
         * aqui que a minha primeira expectativa estava errada. */
        const double rampaEsperada[4] = {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
        const char  *rotulos[4]       = {"1o p2: janela ainda toda p1, taxa 0",
                                         "2o p2: um p2 na janela de 3, taxa 1/3",
                                         "3o p2: dois p2 na janela de 3, taxa 2/3",
                                         "4o p2: janela toda p2, taxa 1"};
        std::vector<Pacote> diferentes;
        for (std::size_t i = 0U; i < 4U; ++i)
        {
            diferentes.push_back(Pacote(2.0 + 0.1 * static_cast<double>(i), 1U, 40000U, p2));
        }
        for (std::size_t i = 0U; i < diferentes.size(); ++i)
        {
            const ara::core::Optional<ids::FeatureVec> v = s.Step(diferentes[i].p);
            chk::ChecaPerto(static_cast<double>(v.value()[kRepeatRate]), rampaEsperada[i], 1e-6, rotulos[i]);
        }
    }

    return chk::Veredito("test_feature_streamer");
}
