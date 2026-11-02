/**
 * @file      score_pcap.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Scores the frozen chain against a labelled capture: detection per class.
 * @version   1.0
 * @date      2026-11-02
 * @copyright Copyright (c) 2026
 *
 * @details   O irmão do eval_pcap. Onde aquele mede FALSO POSITIVO sobre tráfego
 *            todo benigno, este mede DETECÇÃO sobre uma captura ROTULADA: o PCAP e o
 *            .labels que o tools/inject_scenarios.py produz, com uma classe por frame.
 *
 * @details   O alinhamento predição×rótulo é por ÍNDICE DE FRAME, e é por isso que o
 *            laço dirige o sensor quadro a quadro com OnFrame() em vez de Run(): a
 *            frame i do PCAP é a linha i do .labels, as duas na mesma ordem de
 *            timestamp em que o injetor escreveu. Run() processaria tudo de uma vez e
 *            perderia essa correspondência.
 *
 * @details   A predição de um frame é a classe do evento que o sensor dispara para
 *            ele; quando nenhum evento é disparado — benigno, ou frame que o parser
 *            rejeita — a predição é Normal. É a mesma semântica do IdsM: Normal não
 *            levanta evento. Um pino por frame zera antes de cada OnFrame e guarda a
 *            classe disparada, se houver.
 *
 * @details   Uso:
 * @code
 *     score_pcap <captura.pcap> <labels> <dir-artefatos> [--engine compact|tl2cgen]
 * @endcode
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "src/ids_sensor/ByteModel.hpp"
#include "src/ids_sensor/Classifier.hpp"
#include "src/ids_sensor/FeatureStreamer.hpp"
#include "src/ids_sensor/Normalizer.hpp"
#include "src/ids_sensor/PacketSource.hpp"
#include "src/ids_sensor/SecurityEventDispatcher.hpp"
#include "src/ids_sensor/Sensor.hpp"
#include "src/ids_sensor/SomeipParser.hpp"

namespace
{

const char *NomeDaClasse(unsigned i)
{
    static const char *const nomes[ids::kNumClasses] = {"normal", "dos", "fuzzy", "mitm-single", "mitm-multi"};
    return (i < ids::kNumClasses) ? nomes[i] : "?";
}

/**
 * @brief Pino de uma frame: guarda a classe do último evento disparado.
 * @details Zerado antes de cada OnFrame. Se o sensor nao disparar nada, a frame fica
 *          Normal -- que e a predicao correta para benigno e para frame rejeitada.
 */
class PinoFrame final : public ids::ISecurityEventDispatcher
{
  public:
    void Reset()
    {
        m_classe   = ids::TrafficClass::Normal;
        m_disparou = false;
    }

    bool Dispatch(ids::TrafficClass trafficClass, const ids::SecurityEventContext &context, ids::SecurityEventTimestamp timestamp) override
    {
        static_cast<void>(context);
        static_cast<void>(timestamp);
        if (trafficClass == ids::TrafficClass::Normal)
        {
            return false;
        }
        m_classe   = trafficClass;
        m_disparou = true;
        return true;
    }

    unsigned Classe() const
    {
        return static_cast<unsigned>(m_classe);
    }

  private:
    ids::TrafficClass m_classe   = ids::TrafficClass::Normal;
    bool              m_disparou = false;
};

std::vector<int> LeLabels(const std::string &caminho)
{
    std::vector<int> labels;
    std::ifstream fh(caminho.c_str());
    if (!fh)
    {
        return labels;
    }
    long        idx = 0;
    int         cls = 0;
    std::string linha;
    while (std::getline(fh, linha))
    {
        if (std::sscanf(linha.c_str(), "%ld %d", &idx, &cls) == 2)
        {
            labels.push_back(cls);
        }
    }
    return labels;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::printf("uso: score_pcap <captura.pcap> <labels> <dir-artefatos> [--engine compact|tl2cgen]\n");
        return 2;
    }

    const std::string pcap   = argv[1];
    const std::string labels = argv[2];
    const std::string dir    = argv[3];
    std::string       motorPedido = "auto";
    for (int i = 4; i < argc; ++i)
    {
        if ((std::strcmp(argv[i], "--engine") == 0) && ((i + 1) < argc))
        {
            motorPedido = argv[i + 1];
            ++i;
        }
    }

    /* ---- artefatos ---- */
    ara::core::Result<std::unique_ptr<ids::ByteModels>> modelos = ids::ByteModels::Create(dir + "/bytemodels.bin");
    ara::core::Result<ids::Normalizer>                  norm    = ids::Normalizer::Create(dir + "/minmax.bin");
    if (!modelos.HasValue() || !norm.HasValue())
    {
        std::printf("ERRO lendo artefatos em %s\n", dir.c_str());
        return 1;
    }

    std::unique_ptr<ids::Tl2cgenClassifier> gerado;
    std::unique_ptr<ids::CompactClassifier> compacto;
    const ids::IClassifier                 *clf   = nullptr;
    const char                             *motor = "";
    if (motorPedido != "compact")
    {
        ara::core::Result<std::unique_ptr<ids::Tl2cgenClassifier>> r = ids::Tl2cgenClassifier::Create();
        if (r.HasValue())
        {
            gerado = std::move(r.Value());
            clf    = gerado.get();
            motor  = "tl2cgen";
        }
    }
    if (clf == nullptr)
    {
        ara::core::Result<std::unique_ptr<ids::CompactClassifier>> r = ids::CompactClassifier::Create(dir + "/tables.bin");
        if (!r.HasValue())
        {
            std::printf("ERRO lendo %s/tables.bin: %s\n", dir.c_str(), r.Error().Message());
            return 1;
        }
        compacto = std::move(r.Value());
        clf      = compacto.get();
        motor    = "compacto";
    }

    ids::StreamerConfig cfgStreamer;
    ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> streamer = ids::FeatureStreamer::Create(*modelos.Value(), cfgStreamer);
    ara::core::Result<std::unique_ptr<ids::PcapSource>>      fonte    = ids::PcapSource::Create(pcap);
    if (!streamer.HasValue() || !fonte.HasValue())
    {
        std::printf("ERRO montando a cadeia ou abrindo %s\n", pcap.c_str());
        return 1;
    }

    const std::vector<int> y = LeLabels(labels);
    if (y.empty())
    {
        std::printf("ERRO: nenhum rotulo lido de %s\n", labels.c_str());
        return 1;
    }

    ids::SomeipParser parser;
    PinoFrame         pino;
    ids::Sensor       sensor(parser, *streamer.Value(), norm.Value(), *clf, pino);

    std::printf("captura:   %s\nlabels:    %s (%zu)\nmotor:     %s\n\n", pcap.c_str(), labels.c_str(), y.size(), motor);

    /* ---- laco: uma frame por vez, predicao alinhada ao rotulo ---- */
    std::array<std::array<std::uint64_t, ids::kNumClasses>, ids::kNumClasses> confusao{};
    std::size_t                i      = 0U;
    std::size_t                faltou = 0U;
    for (ara::core::Optional<ids::Frame> fr = fonte.Value()->Next(); fr.has_value(); fr = fonte.Value()->Next())
    {
        pino.Reset();
        sensor.OnFrame(fr.value());
        const unsigned pred = pino.Classe();
        if (i < y.size())
        {
            const unsigned real = static_cast<unsigned>(y[i]);
            if ((real < ids::kNumClasses) && (pred < ids::kNumClasses))
            {
                ++confusao[real][pred];
            }
        }
        else
        {
            ++faltou;
        }
        ++i;
    }

    if ((i != y.size()) || (faltou != 0U))
    {
        std::printf("AVISO: %zu frames no PCAP, %zu rotulos. Alinhamento pode estar errado.\n\n", i, y.size());
    }

    /* ---- matriz de confusao ---- */
    std::printf("confusao (linha = real, coluna = predito):\n");
    std::printf("%-12s", "");
    for (unsigned c = 0U; c < ids::kNumClasses; ++c)
    {
        std::printf("%12s", NomeDaClasse(c));
    }
    std::printf("\n");
    for (unsigned r = 0U; r < ids::kNumClasses; ++r)
    {
        std::printf("%-12s", NomeDaClasse(r));
        for (unsigned c = 0U; c < ids::kNumClasses; ++c)
        {
            std::printf("%12llu", static_cast<unsigned long long>(confusao[r][c]));
        }
        std::printf("\n");
    }

    /* ---- recall, precisao e F1 por classe, mais o macro ---- */
    std::printf("\n%-12s %10s %10s %10s\n", "classe", "recall", "precisao", "f1");
    double macroF1  = 0.0;
    unsigned nClasses = 0U;
    for (unsigned k = 0U; k < ids::kNumClasses; ++k)
    {
        std::uint64_t tp = confusao[k][k];
        std::uint64_t linha = 0U;
        std::uint64_t coluna = 0U;
        for (unsigned j = 0U; j < ids::kNumClasses; ++j)
        {
            linha += confusao[k][j];
            coluna += confusao[j][k];
        }
        if (linha == 0U)
        {
            continue; /* classe ausente dos rotulos: nao entra no macro. */
        }
        const double recall    = static_cast<double>(tp) / static_cast<double>(linha);
        const double precisao  = (coluna > 0U) ? (static_cast<double>(tp) / static_cast<double>(coluna)) : 0.0;
        const double f1        = ((recall + precisao) > 0.0) ? (2.0 * recall * precisao / (recall + precisao)) : 0.0;
        std::printf("%-12s %10.4f %10.4f %10.4f\n", NomeDaClasse(k), recall, precisao, f1);
        macroF1 += f1;
        ++nClasses;
    }
    if (nClasses > 0U)
    {
        std::printf("\nmacro-F1 (classes presentes): %.4f\n", macroF1 / static_cast<double>(nClasses));
    }
    return 0;
}
