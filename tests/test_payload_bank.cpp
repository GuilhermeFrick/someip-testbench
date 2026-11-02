/* Confere o banco de payloads e o filtro BPF.
 *
 * O banco e o filtro entraram pelo mesmo motivo: a medicao de falso positivo so
 * vale se o que entra na cadeia for o que se pensa que entrou. Um banco que falha
 * e vira silenciosamente vazio faria o no publicar zero bytes; um filtro que nao
 * instala faria duas rodadas incomparaveis parecerem comparaveis.
 *
 * O arquivo de teste e escrito aqui mesmo, byte a byte, em vez de ser um artefato
 * guardado no repo: assim o teste sabe EXATAMENTE o que o arquivo contem, e os
 * casos de truncamento sao construidos de proposito.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "src/ids_sensor/PacketSource.hpp"
#include "src/nodes/PayloadBank.hpp"
#include "tests/check.hpp"

namespace
{

/* Escreve um banco com dois fluxos, e devolve o caminho. */
std::string EscreveBanco(const std::string &caminho, bool truncado)
{
    std::FILE *const fh = std::fopen(caminho.c_str(), "wb");
    if (fh == nullptr)
    {
        return std::string();
    }

    const std::uint32_t versao = 1U;
    const std::uint32_t fluxos = 2U;
    std::fwrite("SPAY", 1U, 4U, fh);
    std::fwrite(&versao, sizeof(versao), 1U, fh);
    std::fwrite(&fluxos, sizeof(fluxos), 1U, fh);

    /* Fluxo 0x1001/0x0001: 4 bytes, 3 amostras, cada uma reconhecivel. */
    const std::uint16_t s1 = 0x1001U;
    const std::uint16_t e1 = 0x0001U;
    const std::uint32_t t1 = 4U;
    const std::uint32_t n1 = 3U;
    std::fwrite(&s1, sizeof(s1), 1U, fh);
    std::fwrite(&e1, sizeof(e1), 1U, fh);
    std::fwrite(&t1, sizeof(t1), 1U, fh);
    std::fwrite(&n1, sizeof(n1), 1U, fh);
    const std::uint8_t dados1[12] = {0xA0U, 0, 0, 1, 0xA0U, 0, 0, 2, 0xA0U, 0, 0, 3};
    std::fwrite(dados1, 1U, truncado ? 5U : sizeof(dados1), fh);

    if (!truncado)
    {
        const std::uint16_t s2 = 0x1002U;
        const std::uint16_t e2 = 0x0003U;
        const std::uint32_t t2 = 2U;
        const std::uint32_t n2 = 1U;
        std::fwrite(&s2, sizeof(s2), 1U, fh);
        std::fwrite(&e2, sizeof(e2), 1U, fh);
        std::fwrite(&t2, sizeof(t2), 1U, fh);
        std::fwrite(&n2, sizeof(n2), 1U, fh);
        const std::uint8_t dados2[2] = {0xBBU, 0xCCU};
        std::fwrite(dados2, 1U, sizeof(dados2), fh);
    }

    std::fclose(fh);
    return caminho;
}

std::string EscreveLixo(const std::string &caminho)
{
    std::FILE *const fh = std::fopen(caminho.c_str(), "wb");
    if (fh != nullptr)
    {
        std::fwrite("NAOEHBANCO........", 1U, 18U, fh);
        std::fclose(fh);
    }
    return caminho;
}

} // namespace

int main()
{
    const std::string dir  = "/tmp";
    const std::string bom  = EscreveBanco(dir + "/tpb_bom.bin", false);
    const std::string trnc = EscreveBanco(dir + "/tpb_trunc.bin", true);
    const std::string lixo = EscreveLixo(dir + "/tpb_lixo.bin");

    chk::Grupo("PayloadBank: os caminhos de erro, que tem de FALAR");
    {
        /* Um banco que falha e vira vazio em silencio faria o no publicar zero
         * bytes a cada evento, e a rodada pareceria normal. */
        const ids::nodes::PayloadBank ausente = ids::nodes::PayloadBank::FromFile(dir + "/nao_existe_isto.bin");
        chk::Checa(!ausente.Problem().empty(), "arquivo inexistente e recusado, com motivo");
        chk::Checa(ausente.Count() == 0U, "e nao finge ter fluxos");

        const ids::nodes::PayloadBank errado = ids::nodes::PayloadBank::FromFile(lixo);
        chk::Checa(!errado.Problem().empty(), "arquivo que nao e banco e recusado");

        const ids::nodes::PayloadBank cortado = ids::nodes::PayloadBank::FromFile(trnc);
        chk::Checa(!cortado.Problem().empty(), "amostras truncadas sao recusadas");
    }

    chk::Grupo("PayloadBank: leitura do arquivo bom");
    ids::nodes::PayloadBank banco = ids::nodes::PayloadBank::FromFile(bom);
    chk::Checa(banco.Problem().empty(), banco.Problem().empty() ? "lido sem problema" : banco.Problem().c_str());
    chk::Checa(banco.Count() == 2U, "dois fluxos");
    chk::Checa(banco.Find(0x1001U, 0x0001U) != nullptr, "acha o primeiro fluxo");
    chk::Checa(banco.Find(0x1002U, 0x0003U) != nullptr, "acha o segundo fluxo");
    chk::Checa(banco.Find(0x1001U, 0x0003U) == nullptr, "nao mistura servico de um com evento de outro");
    chk::Checa(banco.Find(0x9999U, 0x0001U) == nullptr, "servico inexistente devolve nulo");

    chk::Grupo("PayloadStream: ORDEM preservada, e volta ao inicio");
    {
        ids::nodes::PayloadStream *const fluxo = banco.Find(0x1001U, 0x0001U);
        chk::Checa(fluxo->SampleSize() == 4U, "tamanho de amostra");
        chk::Checa(fluxo->Count() == 3U, "numero de amostras");

        /* A ordem e a propria razao de o banco existir: quatro das dezesseis
         * features olham para o pacote ANTERIOR do fluxo. Entregar fora de ordem
         * produziria um terceiro trafego sintetico em vez do do dataset. */
        bool ordem = true;
        for (unsigned volta = 0U; volta < 2U; ++volta)
        {
            for (std::uint8_t esperado = 1U; esperado <= 3U; ++esperado)
            {
                const ids::ecu::ByteView v = fluxo->Next();
                ordem = ordem && (v.size() == 4U) && (v.data()[0] == 0xA0U) && (v.data()[3] == esperado);
            }
        }
        chk::Checa(ordem, "entrega 1,2,3 e depois 1,2,3 de novo (da a volta)");

        ids::nodes::PayloadStream *const outro = banco.Find(0x1002U, 0x0003U);
        const ids::ecu::ByteView         um    = outro->Next();
        const ids::ecu::ByteView         dois  = outro->Next();
        chk::Checa((um.size() == 2U) && (um.data()[0] == 0xBBU), "fluxo de uma amostra entrega ela");
        chk::Checa((dois.size() == 2U) && (dois.data()[0] == 0xBBU), "e repete, em vez de devolver vazio");
    }

    chk::Grupo("filtro BPF: expressao invalida e RECUSADA, nao ignorada");
    {
        /* Um filtro que nao instala faria duas rodadas incomparaveis parecerem
         * comparaveis, e e o tipo de coisa que so apareceria no numero final. */
        const ara::core::Result<std::unique_ptr<ids::PcapSource>> ruim =
            ids::PcapSource::Create("/tmp/tpb_nao_existe.pcap", "isto nao e bpf");
        chk::Checa(!ruim.HasValue(), "captura inexistente com filtro invalido falha");

        /* Sem captura real nao da para exercitar o caminho feliz aqui; quem faz
         * isso e o test_packet_source, que escreve um PCAP de verdade. Este
         * conjunto cobre o que o outro nao alcanca: a recusa. */
    }

    std::printf("\n");
    return chk::Veredito("test_payload_bank");
}
