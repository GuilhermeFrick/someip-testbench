/* Teste de PARIDADE do parser: campo a campo contra o `someip.parse` do Python.
 *
 * A referencia nao e uma segunda implementacao minha: o
 * tests/gen_parser_vectors.py monta quadros Ethernet crus, roda o parser real do
 * repositorio de reproducao em cada um, e grava o que ele devolveu, inclusive os
 * casos em que ele devolve None.
 *
 * O caso mais instrutivo do conjunto e o do PADDING: uma carga UDP de 2 bytes num
 * quadro completado ate 60 vira um pl_l4 de 18 bytes, e o parser le um cabecalho
 * SOME/IP DENTRO do padding, com service_id 0x0102. O parser de referencia
 * inventa uma mensagem a partir de bytes de enchimento, e o modelo foi treinado
 * com esse comportamento. Cortar o payload em ip_len "consertaria" isso e
 * quebraria a paridade.
 *
 * Uso:
 *   test_parity_parser <parser_vectors.txt>
 */
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/ids_sensor/SomeipParser.hpp"
#include "tests/check.hpp"

namespace
{

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
        bytes.push_back(static_cast<std::uint8_t>(std::strtoul(hex.substr(i, 2U).c_str(), nullptr, 16)));
    }
    return bytes;
}

std::string ToHex(const ids::PayloadView &v)
{
    if (v.empty())
    {
        return "-";
    }
    static const char *d = "0123456789abcdef";
    std::string        s;
    s.reserve(v.size() * 2U);
    for (std::size_t i = 0U; i < v.size(); ++i)
    {
        s.push_back(d[(v[i] >> 4U) & 0x0FU]);
        s.push_back(d[v[i] & 0x0FU]);
    }
    return s;
}

/**
 * @brief Separa a linha nos tres campos delimitados por " | ".
 * @param[in]  linha     Linha do arquivo.
 * @param[out] rotulo    Rotulo do caso.
 * @param[out] quadroHex Quadro em hexadecimal.
 * @param[out] resultado Resultado que o Python devolveu.
 * @return Whether the line could be split.
 */
bool Divide(const std::string &linha, std::string &rotulo, std::string &quadroHex, std::string &resultado)
{
    const std::size_t a = linha.find(" | ");
    if (a == std::string::npos)
    {
        return false;
    }
    const std::size_t b = linha.find(" | ", a + 3U);
    if (b == std::string::npos)
    {
        return false;
    }
    rotulo    = linha.substr(0U, a);
    quadroHex = linha.substr(a + 3U, b - (a + 3U));
    resultado = linha.substr(b + 3U);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::printf("uso: %s <parser_vectors.txt>\n", argv[0]);
        return 2;
    }

    std::ifstream entrada(argv[1]);
    if (!entrada.is_open())
    {
        std::printf("ERRO abrindo %s\n", argv[1]);
        return 1;
    }

    const ids::SomeipParser parser;
    std::string             linha;
    std::size_t             casos = 0U;
    std::size_t             pulados = 0U;

    chk::Grupo("paridade do parser, caso a caso");
    while (std::getline(entrada, linha))
    {
        if (linha.empty() || (linha[0] == '#'))
        {
            continue;
        }

        std::string rotulo;
        std::string quadroHex;
        std::string resultado;
        if (!Divide(linha, rotulo, quadroHex, resultado))
        {
            continue;
        }
        ++casos;

        const std::vector<std::uint8_t> quadro = FromHex(quadroHex);
        ids::Frame                      frame;
        frame.bytes = ids::PayloadView(quadro.data(), quadro.size());
        frame.ts    = 1.0;

        const ara::core::Result<ids::ParsedPacket> obtido = parser.Parse(frame);

        std::istringstream campos(resultado);
        std::string        tipo;
        campos >> tipo;

        if (tipo == "CRASH")
        {
            /* O Python estourou neste quadro, entao nao ha referencia com que
             * comparar. O cabecalho do SomeipParser.cpp registra essa divergencia
             * deliberada: o C++ rejeita em vez de ler fora dos limites. */
            std::printf("  pulado (o Python estourou): %s\n", rotulo.c_str());
            ++pulados;
            continue;
        }

        if (tipo == "NONE")
        {
            chk::Checa(!obtido.HasValue(), ("descartado, como a referencia: " + rotulo).c_str());
            continue;
        }

        /* tipo == "PKT" */
        unsigned long src = 0UL;
        unsigned long dst = 0UL;
        unsigned      sport = 0U;
        unsigned      dport = 0U;
        unsigned      proto = 0U;
        unsigned      ipLen = 0U;
        long          service = 0L;
        long          method  = 0L;
        std::string   hexL4;
        std::string   hexSomeip;
        std::string   hexSd;
        campos >> src >> dst >> sport >> dport >> proto >> ipLen >> service >> method >> hexL4 >> hexSomeip >> hexSd;

        if (!obtido.HasValue())
        {
            std::printf("  FALHA %s: o C++ rejeitou e a referencia devolveu pacote (%s)\n", rotulo.c_str(),
                        obtido.Error().Message());
            ++chk::Falhas();
            continue;
        }

        const ids::ParsedPacket &p   = obtido.Value();
        bool                     bom = true;
        bom = bom && (p.flow.src == static_cast<std::uint32_t>(src));
        bom = bom && (p.flow.dst == static_cast<std::uint32_t>(dst));
        bom = bom && (p.flow.sport == static_cast<std::uint16_t>(sport));
        bom = bom && (p.flow.dport == static_cast<std::uint16_t>(dport));
        bom = bom && (p.flow.proto == static_cast<std::uint8_t>(proto));
        bom = bom && (p.ipLen == static_cast<std::uint16_t>(ipLen));
        bom = bom && (ToHex(p.plL4) == hexL4);
        bom = bom && (ToHex(p.plSomeip) == hexSomeip);
        bom = bom && (ToHex(p.plSd) == hexSd);

        /* service e method valem -1 na referencia quando nao ha cabecalho SOME/IP
         * completo. O ParsedPacket os mantem em zero nesse caso, porque sao
         * unsigned; a comparacao respeita essa diferenca de representacao em vez
         * de exigir que o C++ guarde -1 num uint16. */
        if (service >= 0L)
        {
            bom = bom && (p.serviceId == static_cast<std::uint16_t>(service));
            bom = bom && (p.methodId == static_cast<std::uint16_t>(method));
        }
        else
        {
            bom = bom && (p.serviceId == 0U);
            bom = bom && (p.methodId == 0U);
        }

        chk::Checa(bom, rotulo.c_str());
        if (!bom)
        {
            std::printf("        python: src=%lu dst=%lu sp=%u dp=%u proto=%u iplen=%u svc=%ld l4=%s someip=%s sd=%s\n", src,
                        dst, sport, dport, proto, ipLen, service, hexL4.c_str(), hexSomeip.c_str(), hexSd.c_str());
            std::printf("        cpp   : src=%u dst=%u sp=%u dp=%u proto=%u iplen=%u svc=%u l4=%s someip=%s sd=%s\n",
                        p.flow.src, p.flow.dst, p.flow.sport, p.flow.dport, p.flow.proto, p.ipLen, p.serviceId,
                        ToHex(p.plL4).c_str(), ToHex(p.plSomeip).c_str(), ToHex(p.plSd).c_str());
        }
    }

    std::printf("\n%zu casos comparados, %zu pulados\n", casos, pulados);
    return chk::Veredito("test_parity_parser");
}
