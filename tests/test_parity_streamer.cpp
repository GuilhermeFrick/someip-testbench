/* Teste de PARIDADE do extrator: as 16 features aqui contra as 16 do Python.
 *
 * A referencia NAO e uma segunda implementacao minha. O
 * tests/gen_parity_vectors.py importa o `FeatureStreamer` real de
 * `src/extract_ext.py` do repositorio de reproducao, que e o codigo com o qual
 * o modelo foi treinado e avaliado, e grava o que ele produz.
 *
 * A comparacao e EXATA, bit a bit no `float` de saida, nao "dentro de uma
 * tolerancia". Isso e possivel porque os dois lados somam os mesmos float64 na
 * mesma ordem: a tabela de log-probabilidade e `double` no C++ justamente para
 * isso. Uma tolerancia esconderia o desvio sistematico pequeno que um teste de
 * paridade existe para pegar.
 *
 * Uso:
 *   test_parity_streamer <parity_bytemodels.bin> <parity_vectors.txt>
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/ids_sensor/FeatureStreamer.hpp"

namespace
{

const char *const kFeatureNames[16] = {
    "ip_time_interval", "someip_loglik", "sd_loglik",     "l4_loglik",
    "someip_entropy",   "sd_entropy",    "l4_entropy",    "someip_change",
    "sd_change",        "l4_change",     "ip_len_change", "l4_len_change",
    "repeat_rate",      "someip_len",    "l4_len",        "src_payload_div",
};

/**
 * @brief Converte uma cadeia hexadecimal em bytes; "-" significa vazio.
 * @param[in] hex Cadeia a converter.
 * @return Os bytes correspondentes.
 */
std::vector<std::uint8_t> FromHex(const std::string &hex)
{
    std::vector<std::uint8_t> bytes;
    if ((hex == "-") || hex.empty())
    {
        return bytes;
    }
    bytes.reserve(hex.size() / 2U);
    for (std::size_t i = 0U; (i + 1U) < hex.size(); i += 2U)
    {
        const std::string par = hex.substr(i, 2U);
        bytes.push_back(static_cast<std::uint8_t>(std::strtoul(par.c_str(), nullptr, 16)));
    }
    return bytes;
}

/// Um registro do arquivo de referencia: o pacote e as 16 features esperadas.
struct Registro
{
    ids::ParsedPacket         packet;
    std::vector<std::uint8_t> l4;
    std::vector<std::uint8_t> someip;
    std::vector<std::uint8_t> sd;
    double                    esperado[16];
};

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::printf("uso: %s <parity_bytemodels.bin> <parity_vectors.txt>\n", argv[0]);
        return 2;
    }

    /* ---------------------------------------------- tabelas, as mesmas do Python */
    ara::core::Result<std::unique_ptr<ids::ByteModels>> modelos = ids::ByteModels::Create(argv[1]);
    if (!modelos.HasValue())
    {
        std::printf("ERRO carregando %s: %s\n", argv[1], modelos.Error().Message());
        return 1;
    }
    std::printf("tabelas carregadas: someip L=%zu, sd L=%zu, l4 L=%zu\n", modelos.Value()->Someip().Positions(),
                modelos.Value()->Sd().Positions(), modelos.Value()->L4().Positions());

    /* ------------------------------------------------------ vetores de referencia */
    std::ifstream entrada(argv[2]);
    if (!entrada.is_open())
    {
        std::printf("ERRO abrindo %s\n", argv[2]);
        return 1;
    }

    std::vector<Registro> registros;
    std::string           linha;
    while (std::getline(entrada, linha))
    {
        if (linha.empty() || (linha[0] == '#'))
        {
            continue;
        }
        const std::size_t barra = linha.find('|');
        if (barra == std::string::npos)
        {
            continue;
        }

        Registro r;
        std::istringstream campos(linha.substr(0U, barra));
        std::string        hexL4;
        std::string        hexSomeip;
        std::string        hexSd;
        unsigned long      src = 0UL;
        unsigned long      dst = 0UL;
        unsigned           sport = 0U;
        unsigned           dport = 0U;
        unsigned           proto = 0U;
        unsigned           ipLen = 0U;

        campos >> r.packet.ts >> src >> dst >> sport >> dport >> proto >> ipLen >> hexL4 >> hexSomeip >> hexSd;

        r.packet.flow.src   = static_cast<std::uint32_t>(src);
        r.packet.flow.dst   = static_cast<std::uint32_t>(dst);
        r.packet.flow.sport = static_cast<std::uint16_t>(sport);
        r.packet.flow.dport = static_cast<std::uint16_t>(dport);
        r.packet.flow.proto = static_cast<std::uint8_t>(proto);
        r.packet.ipLen      = static_cast<std::uint16_t>(ipLen);

        r.l4     = FromHex(hexL4);
        r.someip = FromHex(hexSomeip);
        r.sd     = FromHex(hexSd);

        std::istringstream vals(linha.substr(barra + 1U));
        for (std::size_t i = 0U; i < 16U; ++i)
        {
            vals >> r.esperado[i];
        }
        registros.push_back(r);
    }
    std::printf("registros de referencia: %zu\n\n", registros.size());
    if (registros.empty())
    {
        std::printf("ERRO: nenhum registro lido\n");
        return 1;
    }

    /* As vistas sao montadas depois de o vetor parar de crescer: um push_back
     * posterior realocaria os buffers e invalidaria os ponteiros. */
    for (std::size_t i = 0U; i < registros.size(); ++i)
    {
        registros[i].packet.plL4     = ids::PayloadView(registros[i].l4.data(), registros[i].l4.size());
        registros[i].packet.plSomeip = ids::PayloadView(registros[i].someip.data(), registros[i].someip.size());
        registros[i].packet.plSd     = ids::PayloadView(registros[i].sd.data(), registros[i].sd.size());
    }

    /* ------------------------------------------------------------------- extrator */
    ids::StreamerConfig cfg;
    ara::core::Result<std::unique_ptr<ids::FeatureStreamer>> criado = ids::FeatureStreamer::Create(*modelos.Value(), cfg);
    if (!criado.HasValue())
    {
        std::printf("ERRO criando o extrator: %s\n", criado.Error().Message());
        return 1;
    }
    ids::FeatureStreamer &streamer = *criado.Value();

    /* ------------------------------------------------------------------ comparacao */
    std::size_t divergencias = 0U;
    for (std::size_t n = 0U; n < registros.size(); ++n)
    {
        const ara::core::Optional<ids::FeatureVec> obtido = streamer.Step(registros[n].packet);
        if (!obtido.has_value())
        {
            std::printf("pacote %zu: o extrator nao produziu vetor, e o Python produziu\n", n);
            ++divergencias;
            continue;
        }

        const ids::FeatureVec &v = obtido.value();
        for (std::size_t k = 0U; k < 16U; ++k)
        {
            /* A referencia e float64; a saida e float. Comparar o float contra o
             * float64 convertido e exato quando os dois caminhos aritmeticos
             * coincidem, que e o que este teste afirma. */
            const float ref = static_cast<float>(registros[n].esperado[k]);
            if (v[k] != ref)
            {
                std::printf("pacote %2zu  feature %2zu (%-16s)  python=%.9g  cpp=%.9g  delta=%.3g\n", n, k, kFeatureNames[k],
                            static_cast<double>(ref), static_cast<double>(v[k]),
                            static_cast<double>(std::fabs(static_cast<double>(v[k]) - static_cast<double>(ref))));
                ++divergencias;
            }
        }
    }

    /* O teto de fluxos nao existe no Python. Se ele disparou, a comparacao acima
     * deixa de significar paridade, e isso tem de aparecer. */
    const std::uint64_t despejosFluxo  = streamer.FlowEvictions();
    const std::uint64_t despejosOrigem = streamer.SourceEvictions();
    if ((despejosFluxo != 0U) || (despejosOrigem != 0U))
    {
        std::printf("\nATENCAO: houve despejo (%llu fluxos, %llu origens). O Python nao tem teto,\n"
                    "entao a comparacao acima nao prova paridade. Aumente maxFlows/maxSources.\n",
                    static_cast<unsigned long long>(despejosFluxo), static_cast<unsigned long long>(despejosOrigem));
        ++divergencias;
    }
    else
    {
        std::printf("despejos: nenhum (fluxos e origens cabem nos tetos)\n");
    }

    std::printf("\n%s: %zu pacotes x 16 features = %zu comparacoes, %zu divergencias\n",
                (divergencias == 0U) ? "PARIDADE EXATA" : "DIVERGIU", registros.size(), registros.size() * 16U, divergencias);
    return (divergencias == 0U) ? 0 : 1;
}
