/**
 * @file      eval_pcap.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Runs the frozen sensor chain over a capture and reports what it decided.
 * @version   1.0
 * @date      2026-11-01
 * @copyright Copyright (c) 2026
 *
 * @details   This is the criterion the bench exists to answer. Bringing nine nodes up and seeing
 *            notifications flow says the BUS works; it says nothing about whether the detector
 *            trained on the reference dataset still behaves on it. That question is answered by one
 *            number: how many security events the chain raises over traffic that is entirely
 *            benign, which is to say the FALSE POSITIVE rate.
 *
 * @details   The capture is benign by construction, not by labelling: every node in it is one of
 *            ours, publishing on its own cadence, and no attacker node is started. So every event
 *            is a false positive, and no ground-truth file is needed. That is the one thing a
 *            reproduced bus gives that a dataset cannot.
 *
 * @details   The event sink here is a TALLY, not the IdsM. ::ids::IdsmDispatcher writes through
 *            ara::idsm and needs a configured event list; what this tool wants instead is the
 *            distribution: which class, how confident, and from which source and service. A
 *            confident wrong answer and a hesitant one are different findings, and a mean would
 *            hide both.
 *
 * @details   Usage:
 * @code
 *     eval_pcap <capture.pcap> <artefact-dir> [--engine compact|tl2cgen]
 * @endcode
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
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

/// Names of the five classes, in the order of ::ids::TrafficClass.
const char *NomeDaClasse(ids::TrafficClass c)
{
    const char *nome = "?";
    switch (c)
    {
        case ids::TrafficClass::Normal:
            nome = "normal";
            break;
        case ids::TrafficClass::Dos:
            nome = "dos";
            break;
        case ids::TrafficClass::Fuzzy:
            nome = "fuzzy";
            break;
        case ids::TrafficClass::MitmSingle:
            nome = "mitm-single";
            break;
        case ids::TrafficClass::MitmMulti:
            nome = "mitm-multi";
            break;
        default:
            /* Keeps "?" so an unmapped value never yields nullptr. */
            break;
    }
    return nome;
}

/**
 * @brief Event sink that only counts, keeping every confidence it saw.
 * @details Keeps the whole list rather than a running mean on purpose: the useful statistic here is
 *          the MEDIAN and the extremes. A chain that is wrong at 0,52 over five classes is barely
 *          above chance and the error is one of calibration; a chain that is wrong at 0,98 is
 *          confidently wrong, which is a different problem with a different fix.
 */
class Contador final : public ids::ISecurityEventDispatcher
{
  public:
    bool Dispatch(ids::TrafficClass trafficClass, const ids::SecurityEventContext &context, ids::SecurityEventTimestamp timestamp) override
    {
        static_cast<void>(timestamp);

        if (trafficClass == ids::TrafficClass::Normal)
        {
            /* Normal raises no event; the sensor does not even call us for it, but the contract
             * allows it and answering here keeps the two in step. */
            return false;
        }
        else
        {
            /* A reportable class. */
        }

        const unsigned idx = static_cast<unsigned>(trafficClass);
        ++m_porClasse[idx];
        m_confianca.push_back(context.confidence);
        ++m_porOrigem[context.srcIp];
        ++m_porServico[context.serviceId];
        return true;
    }

    std::uint64_t Total() const
    {
        std::uint64_t t = 0U;
        for (std::size_t i = 0U; i < m_porClasse.size(); ++i)
        {
            t += m_porClasse[i];
        }
        return t;
    }

    const std::array<std::uint64_t, ids::kNumClasses> &PorClasse() const
    {
        return m_porClasse;
    }

    const std::map<std::uint32_t, std::uint64_t> &PorOrigem() const
    {
        return m_porOrigem;
    }

    const std::map<std::uint16_t, std::uint64_t> &PorServico() const
    {
        return m_porServico;
    }

    /// Confidences, sorted, so the caller can take quantiles.
    std::vector<float> ConfiancaOrdenada() const
    {
        std::vector<float> c = m_confianca;
        std::sort(c.begin(), c.end());
        return c;
    }

  private:
    std::array<std::uint64_t, ids::kNumClasses> m_porClasse{{0U, 0U, 0U, 0U, 0U}}; ///< Events per class.
    std::vector<float>                          m_confianca;                       ///< Every confidence seen.
    std::map<std::uint32_t, std::uint64_t>      m_porOrigem;                       ///< Events per source address.
    std::map<std::uint16_t, std::uint64_t>      m_porServico;                      ///< Events per service identifier.
};

/// Quantile of an already sorted vector; zero when it is empty.
float Quantil(const std::vector<float> &ordenada, double q)
{
    if (ordenada.empty())
    {
        return 0.0F;
    }
    else
    {
        /* There is something to take a quantile of. */
    }
    std::size_t i = static_cast<std::size_t>(q * static_cast<double>(ordenada.size() - 1U));
    if (i >= ordenada.size())
    {
        i = ordenada.size() - 1U;
    }
    else
    {
        /* In range. */
    }
    return ordenada[i];
}

/// Dotted form of an address held in host byte order.
std::string ComoTexto(std::uint32_t ip)
{
    char texto[24];
    static_cast<void>(std::snprintf(texto, sizeof(texto), "%u.%u.%u.%u", (ip >> 24U) & 0xFFU, (ip >> 16U) & 0xFFU, (ip >> 8U) & 0xFFU,
                                    ip & 0xFFU));
    return std::string(texto);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::printf("uso: eval_pcap <captura.pcap> <dir-artefatos> [--engine compact|tl2cgen]\n");
        return 2;
    }
    else
    {
        /* Arguments given. */
    }

    const std::string pcap = argv[1];
    const std::string dir  = argv[2];
    std::string       motorPedido = "auto";
    std::string       filtro      = "";
    for (int i = 3; i < argc; ++i)
    {
        if ((std::strcmp(argv[i], "--engine") == 0) && ((i + 1) < argc))
        {
            motorPedido = argv[i + 1];
            ++i;
        }
        else if ((std::strcmp(argv[i], "--filter") == 0) && ((i + 1) < argc))
        {
            filtro = argv[i + 1];
            ++i;
        }
        else
        {
            /* Unknown argument: ignored rather than fatal, so a scenario script may pass extras. */
        }
    }

    /* ------------------------------------------------------------- artefatos */
    ara::core::Result<std::unique_ptr<ids::ByteModels>> modelos = ids::ByteModels::Create(dir + "/bytemodels.bin");
    if (!modelos.HasValue())
    {
        std::printf("ERRO lendo %s/bytemodels.bin: %s\n", dir.c_str(), modelos.Error().Message());
        return 1;
    }
    else
    {
        /* Byte models loaded. */
    }

    ara::core::Result<ids::Normalizer> norm = ids::Normalizer::Create(dir + "/minmax.bin");
    if (!norm.HasValue())
    {
        std::printf("ERRO lendo %s/minmax.bin: %s\n", dir.c_str(), norm.Error().Message());
        return 1;
    }
    else
    {
        /* Bounds loaded. */
    }

    /* O motor preferido e o C99 do TL2cgen, como no no vivo; o compacto entra
     * quando o C gerado nao foi ligado, e o programa DIZ qual usou -- comparar
     * duas rodadas sem saber o motor seria comparar coisas diferentes. */
    std::unique_ptr<ids::Tl2cgenClassifier> gerado;
    std::unique_ptr<ids::CompactClassifier> compacto;
    const ids::IClassifier                 *clf = nullptr;
    const char                             *motor = "";

    if (motorPedido != "compact")
    {
        ara::core::Result<std::unique_ptr<ids::Tl2cgenClassifier>> r = ids::Tl2cgenClassifier::Create();
        if (r.HasValue())
        {
            gerado = std::move(r.Value());
            clf    = gerado.get();
            motor  = "tl2cgen (C99 gerado)";
        }
        else
        {
            /* Not linked; falls back below. */
        }
    }
    else
    {
        /* The compact evaluator was asked for explicitly. */
    }

    if (clf == nullptr)
    {
        ara::core::Result<std::unique_ptr<ids::CompactClassifier>> r = ids::CompactClassifier::Create(dir + "/tables.bin");
        if (!r.HasValue())
        {
            std::printf("ERRO lendo %s/tables.bin: %s\n", dir.c_str(), r.Error().Message());
            return 1;
        }
        else
        {
            compacto = std::move(r.Value());
            clf      = compacto.get();
            motor    = "compacto (tabelas de nos)";
        }
    }
    else
    {
        /* Already have an engine. */
    }

    /* --------------------------------------------------------------- cadeia */
    ids::StreamerConfig cfgStreamer;
    ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> streamer = ids::FeatureStreamer::Create(*modelos.Value(), cfgStreamer);
    if (!streamer.HasValue())
    {
        std::printf("ERRO montando o extrator: %s\n", streamer.Error().Message());
        return 1;
    }
    else
    {
        /* Extractor ready. */
    }

    ara::core::Result<std::unique_ptr<ids::PcapSource>> fonte = ids::PcapSource::Create(pcap, filtro);
    if (!fonte.HasValue())
    {
        std::printf("ERRO abrindo %s: %s\n", pcap.c_str(), fonte.Error().Message());
        return 1;
    }
    else
    {
        /* Capture open. */
    }

    ids::SomeipParser parser;
    Contador          contador;
    ids::Sensor       sensor(parser, *streamer.Value(), norm.Value(), *clf, contador);

    std::printf("captura:   %s\n", pcap.c_str());
    std::printf("artefatos: %s\n", dir.c_str());
    std::printf("motor:     %s\n\n", motor);

    sensor.Run(*fonte.Value());

    /* O fim do arquivo e o erro de leitura chegam iguais no Next(); sem esta
     * consulta uma captura truncada daria um numero mais baixo e com cara de
     * valido. */
    const ara::core::Result<void> estado = fonte.Value()->StreamStatus();
    if (!estado.HasValue())
    {
        std::printf("AVISO: a leitura terminou em erro (%s). Os numeros abaixo estao INCOMPLETOS.\n\n",
                    estado.Error().Message());
    }
    else
    {
        /* Clean end of file. */
    }

    /* ------------------------------------------------------------- veredito */
    const ids::StageTiming  t    = sensor.Timing();
    const std::uint64_t     evts = contador.Total();

    std::printf("quadros lidos:        %llu\n", static_cast<unsigned long long>(t.packets + t.malformed));
    std::printf("  aceitos pelo parser: %llu\n", static_cast<unsigned long long>(t.packets));
    std::printf("  rejeitados:          %llu\n", static_cast<unsigned long long>(t.malformed));
    std::printf("eventos levantados:   %llu\n", static_cast<unsigned long long>(evts));
    if (t.packets > 0U)
    {
        /* Toda a captura e benigna por construcao -- nenhum no atacante foi
         * subido -- logo TODO evento aqui e falso positivo, e esta razao e a
         * taxa. Nao ha arquivo de rotulo a consultar. */
        std::printf("FALSO POSITIVO:       %.4f%%  (%llu em %llu pacotes aceitos)\n",
                    100.0 * static_cast<double>(evts) / static_cast<double>(t.packets), static_cast<unsigned long long>(evts),
                    static_cast<unsigned long long>(t.packets));
    }
    else
    {
        std::printf("FALSO POSITIVO:       indefinido, nenhum pacote foi aceito pelo parser\n");
    }

    std::printf("\npor classe:\n");
    for (std::size_t i = 1U; i < ids::kNumClasses; ++i)
    {
        const std::uint64_t n = contador.PorClasse()[i];
        if (n > 0U)
        {
            std::printf("  %-12s %llu\n", NomeDaClasse(static_cast<ids::TrafficClass>(i)), static_cast<unsigned long long>(n));
        }
        else
        {
            /* Class not predicted once. */
        }
    }

    const std::vector<float> conf = contador.ConfiancaOrdenada();
    if (!conf.empty())
    {
        /* Mediana e extremos, nao media: com cinco classes, 0,20 e o acaso.
         * Perto disso o erro e de calibracao; perto de 1,00 e erro confiante, e
         * sao problemas diferentes. */
        std::printf("\nconfianca dos eventos (acaso = %.2f com %zu classes):\n", 1.0 / static_cast<double>(ids::kNumClasses),
                    ids::kNumClasses);
        std::printf("  minima  %.4f\n", static_cast<double>(conf.front()));
        std::printf("  mediana %.4f\n", static_cast<double>(Quantil(conf, 0.50)));
        std::printf("  p90     %.4f\n", static_cast<double>(Quantil(conf, 0.90)));
        std::printf("  maxima  %.4f\n", static_cast<double>(conf.back()));
    }
    else
    {
        /* Nothing was reported, so there is no confidence to summarise. */
    }

    if (!contador.PorOrigem().empty())
    {
        std::printf("\npor origem:\n");
        for (std::map<std::uint32_t, std::uint64_t>::const_iterator it = contador.PorOrigem().begin(); it != contador.PorOrigem().end();
             ++it)
        {
            std::printf("  %-16s %llu\n", ComoTexto(it->first).c_str(), static_cast<unsigned long long>(it->second));
        }
        std::printf("\npor servico:\n");
        for (std::map<std::uint16_t, std::uint64_t>::const_iterator it = contador.PorServico().begin(); it != contador.PorServico().end();
             ++it)
        {
            std::printf("  0x%04X           %llu\n", static_cast<unsigned>(it->first), static_cast<unsigned long long>(it->second));
        }
    }
    else
    {
        /* No events, so no breakdown. */
    }

    return 0;
}
