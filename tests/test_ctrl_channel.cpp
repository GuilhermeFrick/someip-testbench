/**
 * @file      test_ctrl_channel.cpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Parsing of the injector control commands, without a socket.
 * @version   1.0
 * @date      2026-11-02
 * @copyright Copyright (c) 2026
 */
#include <cstdio>

#include "src/nodes/CtrlChannel.hpp"
#include "tests/check.hpp"

int main()
{
    using K = ids::nodes::CtrlCommand::Kind;

    chk::Grupo("liga, desliga, estado");
    chk::Checa(ids::nodes::ParseCtrl("on").kind == K::On, "on");
    chk::Checa(ids::nodes::ParseCtrl("off\n").kind == K::Off, "off com quebra de linha");
    chk::Checa(ids::nodes::ParseCtrl("status").kind == K::Status, "status");
    chk::Checa(ids::nodes::ParseCtrl("on agora").kind == K::Invalid, "on com argumento sobrando é recusado");

    chk::Grupo("períodos");
    {
        const ids::nodes::CtrlCommand c = ids::nodes::ParseCtrl("set period_ms 50");
        chk::Checa(c.kind == K::PeriodMs, "set period_ms");
        chk::ChecaIgual(c.number, 50, "valor lido");
    }
    chk::Checa(ids::nodes::ParseCtrl("set so_period_ms 1000").kind == K::SoPeriodMs, "set so_period_ms");
    chk::Checa(ids::nodes::ParseCtrl("set period_ms 0").kind == K::Invalid, "período zero é recusado");
    chk::Checa(ids::nodes::ParseCtrl("set period_ms abc").kind == K::Invalid, "período não numérico é recusado");
    chk::Checa(ids::nodes::ParseCtrl("set period_ms").kind == K::Invalid, "período ausente é recusado");

    chk::Grupo("vítima do StopOffer");
    {
        const ids::nodes::CtrlCommand c = ids::nodes::ParseCtrl("set so_target 172.28.0.3");
        chk::Checa(c.kind == K::SoTarget, "set so_target");
        chk::ChecaIgual(static_cast<long long>(c.address), 0xAC1C0003LL, "endereço em ordem de host");
    }
    chk::Checa(ids::nodes::ParseCtrl("set so_target 999.1.1.1").kind == K::Invalid, "endereço inválido é recusado");

    chk::Grupo("desconhecido");
    {
        const ids::nodes::CtrlCommand c = ids::nodes::ParseCtrl("explode");
        chk::Checa(c.kind == K::Invalid, "comando desconhecido");
        chk::Checa(!c.problem.empty(), "e diz o porquê");
    }

    return chk::Veredito("test_ctrl_channel");
}
