/* Teste unitario do PcapSource e do LiveSource.
 *
 * O teste de paridade sobre o dataset prova que a leitura esta certa quando o
 * arquivo esta certo. Este cobre o resto: os codigos de erro, o fim do arquivo,
 * e os dois casos em que a leitura ingenua entrega bytes errados sem reclamar.
 *
 * As capturas sao escritas aqui mesmo, byte a byte, em vez de virem de um
 * arquivo versionado: assim o caso fica visivel ao lado da expectativa, e um
 * PCAP de apoio nao pode se perder ou ser trocado sem ninguem notar.
 *
 * Assume little-endian, que e o que x86 le direto e o que os 7 arquivos do
 * dataset usam (magic d4c3b2a1).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "src/ids_sensor/PacketSource.hpp"
#include "tests/check.hpp"

namespace
{

constexpr std::uint32_t kPcapMagicMicro = 0xA1B2C3D4U; ///< Grava-se D4 C3 B2 A1 em little-endian.
constexpr std::uint16_t kVersaoMaior    = 2U;          ///< Versao do formato.
constexpr std::uint16_t kVersaoMenor    = 4U;          ///< Versao do formato.
constexpr std::uint32_t kSnaplen        = 262144U;     ///< O mesmo do dataset.
constexpr std::uint32_t kLinkEthernet   = 1U;          ///< DLT_EN10MB.
constexpr std::uint32_t kLinkRawIp      = 101U;        ///< LINKTYPE_RAW, que NAO e Ethernet.

/**
 * @brief Um registro de captura a escrever.
 */
struct Registro
{
    std::uint32_t              seg;   ///< Segundos do timestamp.
    std::uint32_t              useg;  ///< Microssegundos do timestamp.
    std::uint32_t              noFio; ///< Comprimento que o quadro tinha no fio.
    std::vector<std::uint8_t>  bytes; ///< Bytes capturados; o tamanho e o caplen.
};

/**
 * @brief Escreve uma captura no disco.
 * @param[in] caminho  Arquivo a criar.
 * @param[in] linkType Tipo de enlace a declarar no cabecalho.
 * @param[in] regs     Registros, na ordem.
 * @return Whether the file could be written.
 */
bool EscreveCaptura(const std::string &caminho, std::uint32_t linkType, const std::vector<Registro> &regs)
{
    std::FILE *const fh = std::fopen(caminho.c_str(), "wb");
    if (fh == nullptr)
    {
        return false;
    }

    const std::uint32_t zero = 0U;
    static_cast<void>(std::fwrite(&kPcapMagicMicro, sizeof(kPcapMagicMicro), 1U, fh));
    static_cast<void>(std::fwrite(&kVersaoMaior, sizeof(kVersaoMaior), 1U, fh));
    static_cast<void>(std::fwrite(&kVersaoMenor, sizeof(kVersaoMenor), 1U, fh));
    static_cast<void>(std::fwrite(&zero, sizeof(zero), 1U, fh));
    static_cast<void>(std::fwrite(&zero, sizeof(zero), 1U, fh));
    static_cast<void>(std::fwrite(&kSnaplen, sizeof(kSnaplen), 1U, fh));
    static_cast<void>(std::fwrite(&linkType, sizeof(linkType), 1U, fh));

    for (std::size_t i = 0U; i < regs.size(); ++i)
    {
        const std::uint32_t capturado = static_cast<std::uint32_t>(regs[i].bytes.size());
        static_cast<void>(std::fwrite(&regs[i].seg, sizeof(std::uint32_t), 1U, fh));
        static_cast<void>(std::fwrite(&regs[i].useg, sizeof(std::uint32_t), 1U, fh));
        static_cast<void>(std::fwrite(&capturado, sizeof(std::uint32_t), 1U, fh));
        static_cast<void>(std::fwrite(&regs[i].noFio, sizeof(std::uint32_t), 1U, fh));
        if (capturado > 0U)
        {
            static_cast<void>(std::fwrite(regs[i].bytes.data(), 1U, capturado, fh));
        }
    }
    return (std::fclose(fh) == 0);
}

/**
 * @brief Monta um quadro de @p n bytes com um padrao reconhecivel.
 * @param[in] n     Quantos bytes.
 * @param[in] marca Valor que vai no primeiro byte, para identificar o quadro.
 * @return Os bytes.
 */
std::vector<std::uint8_t> Quadro(std::size_t n, std::uint8_t marca)
{
    std::vector<std::uint8_t> bytes(n, 0U);
    for (std::size_t i = 0U; i < n; ++i)
    {
        bytes[i] = static_cast<std::uint8_t>((i * 3U) & 0xFFU);
    }
    if (n > 0U)
    {
        bytes[0] = marca;
    }
    return bytes;
}

/**
 * @brief Confere o codigo de erro de um Result que deveria ter falhado.
 * @param[in] obtido   Codigo que veio.
 * @param[in] esperado Codigo esperado.
 * @param[in] oQue     Descricao do caso.
 * @return None.
 */
void ChecaErro(const ara::core::ErrorCode &obtido, ids::IdsSensorErrc esperado, const char *oQue)
{
    const bool mesmoDominio = (obtido.Domain().Id() == ids::IdsSensorErrorDomain::DOMAIN_ID);
    const bool mesmoCodigo  = (obtido.Value() == static_cast<ara::core::ErrorDomain::CodeType>(esperado));
    chk::Checa(mesmoDominio && mesmoCodigo, oQue);
    if (!(mesmoDominio && mesmoCodigo))
    {
        std::printf("        veio: %s\n", obtido.Message());
    }
}

const char *kDirTmp = "/tmp";

/**
 * @brief Caminho de apoio dentro de /tmp.
 * @param[in] nome Nome do arquivo.
 * @return O caminho completo.
 */
std::string Tmp(const char *nome)
{
    return std::string(kDirTmp) + "/" + nome;
}

} // namespace

int main()
{
    /* ------------------------------------------------------------ erros de abertura */
    chk::Grupo("PcapSource::Create: os caminhos de erro");
    {
        const ara::core::Result<std::unique_ptr<ids::PcapSource>> r =
            ids::PcapSource::Create(Tmp("nao-existe-este-arquivo.pcap"));
        chk::Checa(!r.HasValue(), "arquivo inexistente e recusado");
        if (!r.HasValue())
        {
            ChecaErro(r.Error(), ids::IdsSensorErrc::ArtefactNotFound, "e o codigo e ArtefactNotFound");
        }
    }
    {
        /* Existe, abre para leitura, mas nao e captura nenhuma. */
        const std::string caminho = Tmp("test_packet_source_lixo.bin");
        std::FILE *const  fh      = std::fopen(caminho.c_str(), "wb");
        if (fh != nullptr)
        {
            static_cast<void>(std::fwrite("isto nao e um pcap", 1U, 18U, fh));
            static_cast<void>(std::fclose(fh));
        }
        const ara::core::Result<std::unique_ptr<ids::PcapSource>> r = ids::PcapSource::Create(caminho);
        chk::Checa(!r.HasValue(), "arquivo que nao e captura e recusado");
        if (!r.HasValue())
        {
            ChecaErro(r.Error(), ids::IdsSensorErrc::ArtefactCorrupted,
                      "e o codigo e ArtefactCorrupted, nao ArtefactNotFound");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }
    {
        /* Captura bem formada, mas de um enlace que NAO e Ethernet. A referencia
         * em Python nao olha o tipo de enlace e decodificaria isto contra os
         * deslocamentos errados, calada. */
        const std::string      caminho = Tmp("test_packet_source_raw.pcap");
        std::vector<Registro>  regs;
        Registro               reg;
        reg.seg   = 1U;
        reg.useg  = 0U;
        reg.noFio = 60U;
        reg.bytes = Quadro(60U, 0xAAU);
        regs.push_back(reg);
        chk::Checa(EscreveCaptura(caminho, kLinkRawIp, regs), "captura de apoio com LINKTYPE_RAW escrita");

        const ara::core::Result<std::unique_ptr<ids::PcapSource>> r = ids::PcapSource::Create(caminho);
        chk::Checa(!r.HasValue(), "captura que nao e Ethernet e recusada");
        if (!r.HasValue())
        {
            ChecaErro(r.Error(), ids::IdsSensorErrc::ArtefactCorrupted, "e o codigo e ArtefactCorrupted");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }

    /* ------------------------------------------------------------------- replay */
    chk::Grupo("PcapSource::Next: o que sai do arquivo");
    {
        const std::string     caminho = Tmp("test_packet_source_ok.pcap");
        std::vector<Registro> regs;

        Registro a;
        a.seg   = 1700000000U;
        a.useg  = 250000U;
        a.noFio = 64U;
        a.bytes = Quadro(64U, 0x11U);
        regs.push_back(a);

        /* Quadro curto COM padding: 2 bytes uteis, completado ate 60. O que importa
         * aqui e que os 60 bytes cheguem inteiros, porque e deles que o parser
         * depende, e e o que o RawPcapReader do scapy tambem entrega. */
        Registro b;
        b.seg   = 1700000000U;
        b.useg  = 250001U;
        b.noFio = 60U;
        b.bytes = Quadro(60U, 0x22U);
        regs.push_back(b);

        /* Registro TRUNCADO: no fio tinha 1514 bytes, foram capturados 80. A vista
         * tem de ter 80. Trocar caplen por len aqui foi verificado: o teste acusa
         * as duas linhas abaixo.
         *
         * E o AddressSanitizer NAO acusa. O buffer do libpcap tem o tamanho do
         * snaplen, entao os 1434 bytes a mais caem dentro da propria alocacao dele
         * e nenhuma ferramenta reclama: o que sai sao bytes de lixo tratados como
         * payload, caladamente. Por isso o caso e explicito aqui. */
        Registro c;
        c.seg   = 1700000001U;
        c.useg  = 0U;
        c.noFio = 1514U;
        c.bytes = Quadro(80U, 0x33U);
        regs.push_back(c);

        chk::Checa(EscreveCaptura(caminho, kLinkEthernet, regs), "captura de apoio escrita");

        ara::core::Result<std::unique_ptr<ids::PcapSource>> r = ids::PcapSource::Create(caminho);
        chk::Checa(r.HasValue(), "captura Ethernet bem formada e aceita");
        if (r.HasValue())
        {
            ids::PcapSource &fonte = *r.Value();

            ara::core::Optional<ids::Frame> q1 = fonte.Next();
            chk::Checa(q1.has_value(), "primeiro quadro entregue");
            if (q1.has_value())
            {
                chk::ChecaIgual(static_cast<long long>(q1.value().bytes.size()), 64, "  com 64 bytes");
                chk::Checa(q1.value().bytes[0] == 0x11U, "  e e o primeiro quadro, pela marca");
                /* 1700000000 + 0,25 representa exatamente em double. */
                chk::ChecaIgualF(q1.value().ts, 1700000000.25, "  timestamp = seg + useg * 1e-6");
            }

            ara::core::Optional<ids::Frame> q2 = fonte.Next();
            chk::Checa(q2.has_value(), "segundo quadro entregue");
            if (q2.has_value())
            {
                chk::ChecaIgual(static_cast<long long>(q2.value().bytes.size()), 60,
                                "  o padding do Ethernet vem junto, 60 bytes");
                chk::Checa(q2.value().bytes[0] == 0x22U, "  e e o segundo quadro, pela marca");
            }

            ara::core::Optional<ids::Frame> q3 = fonte.Next();
            chk::Checa(q3.has_value(), "terceiro quadro entregue");
            if (q3.has_value())
            {
                chk::ChecaIgual(static_cast<long long>(q3.value().bytes.size()), 80,
                                "  truncado: vale o CAPTURADO (80), nao o do fio (1514)");
                chk::Checa(q3.value().bytes[0] == 0x33U, "  e e o terceiro quadro, pela marca");
                /* Toca o ultimo byte: se a vista estivesse com 1514, o sanitizer
                 * acusa aqui, e nao numa leitura distante daqui a muito tempo. */
                chk::Checa(q3.value().bytes[q3.value().bytes.size() - 1U] ==
                               static_cast<std::uint8_t>((79U * 3U) & 0xFFU),
                           "  e o ultimo byte da vista e o ultimo byte capturado");
            }

            chk::Checa(!fonte.Next().has_value(), "depois do ultimo, o Optional vem vazio");
            chk::Checa(!fonte.Next().has_value(), "e continua vazio se chamarem de novo");
            chk::Checa(fonte.StreamStatus().HasValue(), "StreamStatus: o arquivo acabou, nao quebrou");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }
    {
        /* Captura valida e VAZIA: so o cabecalho. Fim de arquivo logo na primeira
         * chamada, e ainda assim um estado saudavel. */
        const std::string           caminho = Tmp("test_packet_source_vazio.pcap");
        const std::vector<Registro> nenhum;
        chk::Checa(EscreveCaptura(caminho, kLinkEthernet, nenhum), "captura vazia escrita");

        ara::core::Result<std::unique_ptr<ids::PcapSource>> r = ids::PcapSource::Create(caminho);
        chk::Checa(r.HasValue(), "captura sem nenhum registro e aceita");
        if (r.HasValue())
        {
            chk::Checa(!r.Value()->Next().has_value(), "e nao entrega quadro nenhum");
            chk::Checa(r.Value()->StreamStatus().HasValue(), "sem erro de leitura");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }

    /* --------------------------------------------------------------- LiveSource */
    chk::Grupo("LiveSource::Create: a interface que nao existe");
    {
        const ara::core::Result<std::unique_ptr<ids::LiveSource>> r =
            ids::LiveSource::Create("interface-que-nao-existe0");
        chk::Checa(!r.HasValue(), "interface inexistente e recusada");
        if (!r.HasValue())
        {
            /* Sem privilegio de captura o libpcap pode recusar por permissao antes
             * de chegar a procurar a interface. Os dois desfechos sao legitimos, e
             * o que nao pode e passar por sucesso. */
            const bool naoAchou =
                (r.Error().Value() == static_cast<ara::core::ErrorDomain::CodeType>(ids::IdsSensorErrc::ArtefactNotFound));
            const bool semPermissao =
                (r.Error().Value() ==
                 static_cast<ara::core::ErrorDomain::CodeType>(ids::IdsSensorErrc::ArtefactUnreadable));
            chk::Checa(naoAchou || semPermissao, "com ArtefactNotFound, ou ArtefactUnreadable se faltar privilegio");
            if (!(naoAchou || semPermissao))
            {
                std::printf("        veio: %s\n", r.Error().Message());
            }
        }
    }

    return chk::Veredito("test_packet_source");
}
