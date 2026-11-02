/**
 * @file      test_sd_raw.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     The StopOfferService frames built by src/nodes/SdRawEmitter.hpp, against the dataset.
 * @version   1.0
 * @date      2026-11-02
 * @copyright Copyright (c) 2026
 *
 * @details   The reference frames are the first StopOffer of each man-in-the-middle capture of the
 *            Kim dataset, printed by tools/stopoffer_detail.py, from the IPv4 header on. If the
 *            builder matches them byte for byte, IPv4 and UDP checksums included, the frame the
 *            injector puts on the bench wire is the one the dataset has, not a lookalike.
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "src/nodes/SdRawEmitter.hpp"
#include "tests/check.hpp"

namespace
{

/**
 * @brief Decode hexadecimal text.
 * @param[in] hex Even number of digits.
 * @return The bytes.
 */
std::vector<std::uint8_t> Hex(const std::string &hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0U; (i + 1U) < hex.size(); i += 2U)
    {
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2U), nullptr, 16)));
    }
    return out;
}

/**
 * @brief Index of the first differing byte, or -1 when equal.
 * @param[in] a One buffer.
 * @param[in] b The other.
 * @return The index, or -1.
 */
long long PrimeiraDiferenca(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b)
{
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    for (std::size_t i = 0U; i < n; ++i)
    {
        if (a[i] != b[i])
        {
            return static_cast<long long>(i);
        }
        else
        {
            /* Same byte. */
        }
    }
    return (a.size() == b.size()) ? -1 : static_cast<long long>(n);
}

/// The VDE's withdrawal as both captures carry it: service 0x1001, instance 1, VDE endpoints.
ids::nodes::StopOfferSpec StopOfferDaVde(std::uint16_t sessao)
{
    ids::nodes::StopOfferSpec s;
    s.service  = 0x1001U;
    s.instance = 0x0001U;
    static_cast<void>(ids::nodes::ParseIpv4("172.18.0.10", s.providerIp));
    s.tcpPort = 30501U;
    s.udpPort = 31097U;
    s.session = sessao;
    return s;
}

} // namespace

int main()
{
    /* mitm_single_attacker.pcap: from the attacker's own address .13 to the ADAS, DF set, UDP
     * checksum filled, IPv4 identification 0xa14d. */
    const std::string kSingle = "45000060a14d40004011410cac12000dac120002771a771a004c5891"
                                "ffff81000000003c0000000101010200c00000000000001001000020"
                                "1001000100000000000000000000001800090400ac12000a00067725"
                                "00090400ac12000a00117979";
    /* mitm_multi_attacker.pcap: source address FORGED as the VDE's .10, no DF, UDP checksum
     * zero, identification 0xd431. */
    const std::string kMulti = "45000060d431000040114e2bac12000aac120002771a771a004c0000"
                               "ffff81000000003c0000000101010200c00000000000001001000020"
                               "1001000100000000000000000000001800090400ac12000a00067725"
                               "00090400ac12000a00117979";

    chk::Grupo("mensagem SD: o StopOffer da VDE");
    {
        const std::vector<std::uint8_t> msg      = ids::nodes::BuildStopOffer(StopOfferDaVde(1U));
        const std::vector<std::uint8_t> esperado = Hex(kSingle.substr(56U));
        chk::ChecaIgual(static_cast<long long>(msg.size()), 68, "68 bytes de SOME/IP-SD");
        chk::ChecaIgual(PrimeiraDiferenca(msg, esperado), -1, "igual ao do dataset, byte a byte");
    }

    /* O checksum UDP deste quadro no dataset (0x5891) NÃO é um checksum completo: é a
     * soma do pseudo-cabeçalho dobrada e sem complemento, o valor parcial que o kernel
     * deixa para a placa terminar (checksum offload), visto porque a captura foi feita
     * na bridge do Docker. Conclusão: o atacante do MITM single mandou o StopOffer por
     * um socket UDP COMUM, do próprio IP. Na bancada ele sai do mesmo jeito, e a captura
     * na bridge mostra o mesmo artefato. */
    chk::Grupo("MITM single: IP próprio, DF, checksum UDP parcial (offload)");
    {
        ids::nodes::RawHeaderSpec h;
        static_cast<void>(ids::nodes::ParseIpv4("172.18.0.13", h.srcIp));
        static_cast<void>(ids::nodes::ParseIpv4("172.18.0.2", h.dstIp));
        h.ipId         = 0xa14dU;
        h.dontFragment = true;
        h.udpChecksum  = false;
        std::vector<std::uint8_t>       d        = ids::nodes::BuildIpv4Udp(h, ids::nodes::BuildStopOffer(StopOfferDaVde(1U)));
        const std::vector<std::uint8_t> esperado = Hex(kSingle);
        chk::ChecaIgual(PrimeiraDiferenca(d, esperado), 26, "tudo igual até o checksum UDP");

        std::vector<std::uint8_t> pseudo;
        ids::nodes::Put32(pseudo, h.srcIp);
        ids::nodes::Put32(pseudo, h.dstIp);
        pseudo.push_back(0U);
        pseudo.push_back(17U);
        ids::nodes::Put16(pseudo, static_cast<std::uint16_t>(d.size() - 20U));
        std::uint32_t soma = ids::nodes::SomaInternet(pseudo.data(), pseudo.size(), 0U);
        while ((soma >> 16U) != 0U)
        {
            soma = (soma & 0xFFFFU) + (soma >> 16U);
        }
        chk::ChecaIgual(static_cast<long long>((esperado[26] << 8) | esperado[27]), static_cast<long long>(soma),
                        "o checksum do dataset é a soma parcial do pseudo-cabeçalho");
        d[26] = esperado[26];
        d[27] = esperado[27];
        chk::ChecaIgual(PrimeiraDiferenca(d, esperado), -1, "e depois dele o resto também é igual");
    }

    chk::Grupo("MITM multi: IP da VDE forjado, sem DF, checksum UDP zero");
    {
        ids::nodes::RawHeaderSpec h;
        static_cast<void>(ids::nodes::ParseIpv4("172.18.0.10", h.srcIp));
        static_cast<void>(ids::nodes::ParseIpv4("172.18.0.2", h.dstIp));
        h.ipId         = 0xd431U;
        h.dontFragment = false;
        h.udpChecksum  = false;
        const std::vector<std::uint8_t> d = ids::nodes::BuildIpv4Udp(h, ids::nodes::BuildStopOffer(StopOfferDaVde(1U)));
        chk::ChecaIgual(PrimeiraDiferenca(d, Hex(kMulti)), -1, "datagrama igual ao do dataset, origem forjada");
    }

    chk::Grupo("a sessão entra no cabeçalho SOME/IP");
    {
        const std::vector<std::uint8_t> msg = ids::nodes::BuildStopOffer(StopOfferDaVde(0x1234U));
        chk::ChecaIgual(static_cast<long long>((msg[10] << 8) | msg[11]), 0x1234, "sessão nos bytes 10 e 11");
    }

    chk::Grupo("endpoint omitido quando a porta é zero");
    {
        ids::nodes::StopOfferSpec s = StopOfferDaVde(1U);
        s.udpPort                   = 0U;
        const std::vector<std::uint8_t> msg = ids::nodes::BuildStopOffer(s);
        chk::ChecaIgual(static_cast<long long>(msg.size()), 68 - 12, "uma opção de 12 bytes a menos");
        chk::ChecaIgual(static_cast<long long>(msg[16U + 4U + 4U + 3U]), 0x10, "uma opção na primeira série");
    }

    return chk::Veredito("test_sd_raw");
}
