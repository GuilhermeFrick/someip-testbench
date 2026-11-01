/* Paridade do sensor sobre o DATASET INTEIRO, feature a feature.
 *
 * O teste sintetico (tests/test_parity_streamer.cpp) compara 23 pacotes que eu
 * escolhi. Este compara os 14 milhoes do dataset, e portanto cobre o que eu nao
 * soube imaginar. A referencia e gerada por tests/gen_parity_dataset.py a partir
 * do codigo Python que treinou o modelo.
 *
 * E a MESMA cadeia que roda em producao: PcapSource -> SomeipParser ->
 * FeatureStreamer. Nao existe caminho de codigo separado para o teste, o que e o
 * motivo de o PcapSource existir.
 *
 * Nao entra no tests/run_all.sh: a rodada leva minutos e le 1,4 GB. Ela e
 * disparada a mao, e o resultado vai para docs/.
 *
 * Uso:
 *   parity_dataset <dataset_bytemodels.bin> <manifest.txt> <pcap_dir> <registros_dir>
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/ids_sensor/ByteModel.hpp"
#include "src/ids_sensor/FeatureStreamer.hpp"
#include "src/ids_sensor/PacketSource.hpp"
#include "src/ids_sensor/SomeipParser.hpp"
#include "tests/check.hpp"

namespace
{

/* Tamanho do registro de referencia: uint32 de indice mais 16 float32. Lido como
 * bytes e copiado com memcpy, para nao depender de preenchimento de struct. O
 * gerador escreve em little-endian, que e o que x86 le direto. */
constexpr std::size_t kIndexBytes  = 4U;
constexpr std::size_t kRecordBytes = kIndexBytes + (ids::kNumFeatures * sizeof(float));

/* Limites largos de proposito: a referencia em Python nao tem limite nenhum, e
 * qualquer despejo faz os dois lados pararem de calcular a mesma coisa. Os
 * contadores de despejo sao conferidos no fim de cada arquivo. O valor de
 * producao e outra discussao, e e justamente o que esta rodada mede. */
constexpr std::size_t kMaxFlows   = 4194304U;
constexpr std::size_t kMaxSources = 1048576U;

/* Quantas divergencias imprimir por arquivo antes de so contar. */
constexpr std::size_t kDivergenciasImpressas = 10U;

/**
 * @brief Uma linha do manifesto.
 */
struct Entrada
{
    std::string   pcap;        ///< Nome do PCAP.
    std::string   registros;   ///< Nome do arquivo de registros de referencia.
    std::uint64_t pacotes = 0U;    ///< Pacotes lidos pelo Python.
    std::uint64_t descartados = 0U; ///< Pacotes que o parser do Python descartou.
    std::uint64_t linhas = 0U;     ///< Vetores que o Python emitiu.
    std::uint64_t fluxos = 0U;     ///< Fluxos distintos que o Python viu.
    std::uint64_t origens = 0U;    ///< Origens distintas que o Python viu.
};

/**
 * @brief Le o manifesto.
 * @param[in] caminho Arquivo gerado pelo lado Python.
 * @return As entradas, na ordem em que aparecem.
 */
std::vector<Entrada> LeManifesto(const std::string &caminho)
{
    std::vector<Entrada> entradas;
    std::ifstream        entrada(caminho);
    std::string          linha;
    while (std::getline(entrada, linha))
    {
        if (linha.empty() || (linha[0] == '#'))
        {
            continue;
        }
        std::istringstream campos(linha);
        Entrada            e;
        if (campos >> e.pcap >> e.registros >> e.pacotes >> e.descartados >> e.linhas >> e.fluxos >> e.origens)
        {
            entradas.push_back(e);
        }
    }
    return entradas;
}

/**
 * @brief Resultado da comparacao de um arquivo.
 */
struct Resumo
{
    std::uint64_t pacotes       = 0U; ///< Quadros que o PcapSource entregou.
    std::uint64_t descartados   = 0U; ///< Quadros que o SomeipParser rejeitou.
    std::uint64_t linhas        = 0U; ///< Vetores que o extrator emitiu.
    std::uint64_t divergencias  = 0U; ///< Vetores que diferem da referencia.
    std::uint64_t indicesFora   = 0U; ///< Vetores cujo indice de pacote nao casa.
    double        piorDiferenca = 0.0; ///< Maior diferenca absoluta vista em qualquer feature.
    std::size_t   piorFeature   = 0U;  ///< Feature em que ela apareceu.
    double        segundos      = 0.0; ///< Tempo de parede da cadeia inteira sobre este arquivo.
};

/**
 * @brief Roda a cadeia inteira sobre um PCAP e compara com a referencia.
 * @param[in]     caminhoPcap Arquivo de captura.
 * @param[in]     caminhoRef  Arquivo de registros de referencia.
 * @param[in]     limite      Quantos quadros o lado Python leu, do manifesto.
 * @param[in,out] streamer    Extrator, reiniciado antes de comecar.
 * @param[in]     parser      Parser, sem estado.
 * @return O resumo, ou um resumo vazio quando o arquivo nao pudera ser aberto.
 * @details O limite existe porque o gerador aceita --max-packets, para um ensaio antes da rodada
 *          inteira. Na rodada inteira ele e o tamanho do arquivo, e entao serve de conferencia:
 *          se o C++ parar antes, a contagem de quadros nao fecha e StreamStatus() diz por que.
 */
Resumo Compara(const std::string &caminhoPcap, const std::string &caminhoRef, std::uint64_t limite,
               ids::FeatureStreamer &streamer, const ids::SomeipParser &parser)
{
    Resumo resumo;

    ara::core::Result<std::unique_ptr<ids::PcapSource>> fonte = ids::PcapSource::Create(caminhoPcap);
    if (!fonte.HasValue())
    {
        std::printf("  FALHA abrindo %s: %s\n", caminhoPcap.c_str(), fonte.Error().Message());
        ++chk::Falhas();
        return resumo;
    }

    std::FILE *const ref = std::fopen(caminhoRef.c_str(), "rb");
    if (ref == nullptr)
    {
        std::printf("  FALHA abrindo a referencia %s\n", caminhoRef.c_str());
        ++chk::Falhas();
        return resumo;
    }

    streamer.Reset();

    unsigned char registro[kRecordBytes];
    std::uint64_t indice = 0U;

    /* Tempo de parede do laco inteiro: cadeia MAIS a leitura e a comparacao dos
     * registros de referencia. E portanto um TETO para o custo da cadeia, nao uma
     * medida dela. A medida limpa, por pacote e sem a comparacao, e a do banco de
     * latencia; aqui o numero serve para saber que a cadeia processa o dataset em
     * tempo de uma rodada, nao para publicar microssegundos. */
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    for (ara::core::Optional<ids::Frame> quadro = fonte.Value()->Next(); quadro.has_value() && (indice < limite);
         quadro                                 = fonte.Value()->Next())
    {
        ++indice;
        ++resumo.pacotes;

        const ara::core::Result<ids::ParsedPacket> pacote = parser.Parse(quadro.value());
        if (!pacote.HasValue())
        {
            ++resumo.descartados;
            continue;
        }

        const ara::core::Optional<ids::FeatureVec> features = streamer.Step(pacote.Value());
        if (!features.has_value())
        {
            /* O extrator nao emite vetor. A referencia emite um para todo pacote
             * que o parser aceitou, entao isto ja e divergencia. */
            ++resumo.divergencias;
            continue;
        }
        ++resumo.linhas;

        if (std::fread(registro, 1U, kRecordBytes, ref) != kRecordBytes)
        {
            std::printf("  FALHA a referencia terminou no pacote %llu, o C++ continuou\n",
                        static_cast<unsigned long long>(indice));
            ++chk::Falhas();
            break;
        }

        std::uint32_t indiceRef = 0U;
        std::memcpy(&indiceRef, registro, kIndexBytes);
        if (static_cast<std::uint64_t>(indiceRef) != indice)
        {
            ++resumo.indicesFora;
            if (resumo.indicesFora <= kDivergenciasImpressas)
            {
                std::printf("  FALHA indice do pacote: C++ %llu, referencia %u. Um lado aceitou um quadro que o "
                            "outro descartou.\n",
                            static_cast<unsigned long long>(indice), indiceRef);
            }
            ++chk::Falhas();
            break;
        }

        float valoresRef[ids::kNumFeatures];
        std::memcpy(valoresRef, registro + kIndexBytes, sizeof(valoresRef));

        bool igual = true;
        for (std::size_t i = 0U; i < ids::kNumFeatures; ++i)
        {
            const float obtido   = features.value()[i];
            const float esperado = valoresRef[i];
            if (obtido != esperado)
            {
                igual                 = false;
                const double diferenca = std::fabs(static_cast<double>(obtido) - static_cast<double>(esperado));
                if (diferenca > resumo.piorDiferenca)
                {
                    resumo.piorDiferenca = diferenca;
                    resumo.piorFeature   = i;
                }
            }
        }

        if (!igual)
        {
            ++resumo.divergencias;
            if (resumo.divergencias <= kDivergenciasImpressas)
            {
                std::printf("  divergencia no pacote %llu:\n", static_cast<unsigned long long>(indice));
                for (std::size_t i = 0U; i < ids::kNumFeatures; ++i)
                {
                    const float obtido   = features.value()[i];
                    const float esperado = valoresRef[i];
                    if (obtido != esperado)
                    {
                        std::printf("      f%-2zu  cpp=%.9g  python=%.9g\n", i, static_cast<double>(obtido),
                                    static_cast<double>(esperado));
                    }
                }
            }
        }
    }

    resumo.segundos = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    /* A referencia tem de ter acabado junto. Se sobrou registro, o C++ descartou
     * quadros que o Python aceitou. */
    const std::size_t sobra = std::fread(registro, 1U, kRecordBytes, ref);
    chk::ChecaIgual(static_cast<long long>(sobra), 0, "a referencia acabou junto com a captura");
    static_cast<void>(std::fclose(ref));

    /* Um erro de leitura no meio do arquivo esvazia o Optional igual ao fim do
     * arquivo. Sem esta checagem, uma rodada truncada passaria por completa. */
    const ara::core::Result<void> estado = fonte.Value()->StreamStatus();
    chk::Checa(estado.HasValue(), "a captura foi lida inteira, sem erro de leitura");

    return resumo;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 5)
    {
        std::printf("uso: %s <dataset_bytemodels.bin> <manifest.txt> <pcap_dir> <registros_dir>\n", argv[0]);
        return 2;
    }

    const std::string caminhoModelos = argv[1];
    const std::string caminhoManif   = argv[2];
    const std::string dirPcap        = argv[3];
    const std::string dirRef         = argv[4];

    ara::core::Result<std::unique_ptr<ids::ByteModels>> modelos = ids::ByteModels::Create(caminhoModelos);
    if (!modelos.HasValue())
    {
        std::printf("ERRO lendo %s: %s\n", caminhoModelos.c_str(), modelos.Error().Message());
        return 1;
    }

    ids::StreamerConfig cfg;
    cfg.maxFlows   = kMaxFlows;
    cfg.maxSources = kMaxSources;

    ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> streamer =
        ids::FeatureStreamer::Create(*modelos.Value(), cfg);
    if (!streamer.HasValue())
    {
        std::printf("ERRO criando o extrator: %s\n", streamer.Error().Message());
        return 1;
    }

    const std::vector<Entrada> entradas = LeManifesto(caminhoManif);
    if (entradas.empty())
    {
        std::printf("ERRO: manifesto vazio ou ilegivel: %s\n", caminhoManif.c_str());
        return 1;
    }

    const ids::SomeipParser parser;
    std::uint64_t           totalPacotes      = 0U;
    std::uint64_t           totalLinhas       = 0U;
    std::uint64_t           totalDivergencias = 0U;
    double                  totalSegundos     = 0.0;

    for (std::size_t k = 0U; k < entradas.size(); ++k)
    {
        const Entrada &e = entradas[k];
        chk::Grupo(e.pcap.c_str());

        const Resumo r =
            Compara(dirPcap + "/" + e.pcap, dirRef + "/" + e.registros, e.pacotes, *streamer.Value(), parser);

        chk::ChecaIgual(static_cast<long long>(r.pacotes), static_cast<long long>(e.pacotes),
                        "mesmo numero de quadros lidos da captura");
        chk::ChecaIgual(static_cast<long long>(r.descartados), static_cast<long long>(e.descartados),
                        "mesmo numero de quadros descartados pelo parser");
        chk::ChecaIgual(static_cast<long long>(r.linhas), static_cast<long long>(e.linhas),
                        "mesmo numero de vetores emitidos");
        chk::ChecaIgual(static_cast<long long>(r.divergencias), 0, "nenhuma feature divergente");
        chk::ChecaIgual(static_cast<long long>(r.indicesFora), 0, "nenhum pacote fora de sincronia");
        chk::ChecaIgual(static_cast<long long>(streamer.Value()->FlowEvictions()), 0,
                        "nenhum fluxo despejado (a referencia nao despeja)");
        chk::ChecaIgual(static_cast<long long>(streamer.Value()->SourceEvictions()), 0,
                        "nenhuma origem despejada (a referencia nao despeja)");

        if (r.divergencias > 0U)
        {
            std::printf("      pior diferenca: %.9g na feature f%zu\n", r.piorDiferenca, r.piorFeature);
        }
        std::printf("      %llu quadros, %llu descartados, %llu vetores; python viu %llu fluxos e %llu origens\n",
                    static_cast<unsigned long long>(r.pacotes), static_cast<unsigned long long>(r.descartados),
                    static_cast<unsigned long long>(r.linhas), static_cast<unsigned long long>(e.fluxos),
                    static_cast<unsigned long long>(e.origens));
        if (r.segundos > 0.0)
        {
            std::printf("      %.1f s para cadeia mais comparacao (teto, nao medida da cadeia)\n", r.segundos);
        }

        totalPacotes += r.pacotes;
        totalLinhas += r.linhas;
        totalDivergencias += r.divergencias;
        totalSegundos += r.segundos;
    }

    std::printf("\n%llu quadros, %llu vetores comparados, %llu divergencias\n",
                static_cast<unsigned long long>(totalPacotes), static_cast<unsigned long long>(totalLinhas),
                static_cast<unsigned long long>(totalDivergencias));
    if (totalSegundos > 0.0)
    {
        std::printf("%.1f s no total, cadeia mais comparacao: %.2f us por quadro, como TETO\n", totalSegundos,
                    (totalSegundos * 1e6) / static_cast<double>(totalPacotes));
    }
    return chk::Veredito("parity_dataset");
}
