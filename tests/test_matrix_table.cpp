/* Teste da tabela de RUNTIME da matriz (build/cfg/CommMatrixTable.hpp).
 *
 * O test_comm_matrix confere o cabecalho de identificadores contra o YAML, por
 * round-trip. Este cobre a outra saida do gerador, que responde a outra
 * pergunta: o no GENERICO consegue percorrer a matriz em tempo de execucao e
 * achar o que precisa?
 *
 * O que importa aqui nao e o valor de um identificador -- e a ESTRUTURA:
 * ponteiros que nao sao nulos, contagens que batem, nome que resolve, e a
 * ligacao entre eventgroup e evento intacta. Uma tabela com um ponteiro solto
 * compila perfeitamente e so quebra quando um no tentar subir.
 *
 * Este teste roda contra a matriz da BANCADA (build/cfg). A matriz recuperada
 * do baseline passa pelo mesmo gerador, e o degrau que a usa e quem a exercita.
 */
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "build/cfg/CommMatrixTable.hpp"
#include "tests/check.hpp"

namespace tb = ids::matrix::table;

int main()
{
    chk::Grupo("a tabela existe e e percorrivel");
    const tb::Table<tb::NodeDesc>    nos      = tb::Nodes();
    const tb::Table<tb::ServiceDesc> servicos = tb::Services();

    chk::Checa(nos.count > 0U, "ha nos na tabela");
    chk::Checa(servicos.count > 0U, "ha servicos na tabela");
    chk::Checa(nos.items != nullptr, "o ponteiro dos nos nao e nulo");
    chk::Checa(servicos.items != nullptr, "o ponteiro dos servicos nao e nulo");

    chk::Grupo("todo no tem os campos preenchidos");
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        const tb::NodeDesc &n = nos.items[i];
        const std::string   oQue = std::string("no ") + ((n.name != nullptr) ? n.name : "<nulo>");
        bool                bom  = (n.name != nullptr) && (n.appName != nullptr) && (n.ip != nullptr);
        bom = bom && (std::strlen(n.name) > 0U) && (std::strlen(n.appName) > 0U) && (std::strlen(n.ip) > 0U);
        /* Um no sem servico algum e legitimo: e o caso do monitor, que nem
         * oferece nem consome -- ele captura. O que nao pode e contagem sem
         * ponteiro, ou ponteiro sem contagem. */
        bom = bom && ((n.provideCount == 0U) == (n.provides == nullptr));
        bom = bom && ((n.consumeCount == 0U) == (n.consumes == nullptr));
        chk::Checa(bom, oQue.c_str());
    }

    chk::Grupo("FindNode resolve todo no, e so eles");
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        const tb::NodeDesc *const achado = tb::FindNode(nos.items[i].name);
        chk::Checa(achado == &nos.items[i], nos.items[i].name);
    }
    chk::Checa(tb::FindNode("nao_existe_este_no") == nullptr, "um nome inexistente devolve nulo");
    chk::Checa(tb::FindService("NaoExisteEsteServico") == nullptr, "idem para servico");

    chk::Grupo("todo servico tem pelo menos um endpoint");
    for (std::size_t i = 0U; i < servicos.count; ++i)
    {
        const tb::ServiceDesc &s = servicos.items[i];
        /* Um servico sem transporte nenhum nao e ofertavel. E o tipo de furo que
         * so aparece quando um no tenta subir, horas depois. */
        chk::Checa(s.hasTcp || s.hasUdp, (std::string("servico ") + s.name + " tem transporte").c_str());
        if (s.hasTcp)
        {
            chk::Checa(s.tcpPort != 0U, (std::string("  ") + s.name + ": porta TCP nao e zero").c_str());
        }
        if (s.hasUdp)
        {
            chk::Checa(s.udpPort != 0U, (std::string("  ") + s.name + ": porta UDP nao e zero").c_str());
        }
    }

    chk::Grupo("eventos e eventgroups, e a ligacao entre eles");
    for (std::size_t i = 0U; i < servicos.count; ++i)
    {
        const tb::ServiceDesc        &s       = servicos.items[i];
        const tb::Table<tb::EventDesc> eventos = tb::EventsOf(s.name);
        const tb::Table<tb::GroupDesc> grupos  = tb::GroupsOf(s.name);

        chk::Checa(eventos.count > 0U, (std::string("servico ") + s.name + " tem eventos").c_str());

        std::set<unsigned> declarados;
        for (std::size_t k = 0U; k < eventos.count; ++k)
        {
            declarados.insert(static_cast<unsigned>(eventos.items[k].id));
        }

        /* Todo evento citado num eventgroup tem de existir na lista de eventos do
         * mesmo servico. Um grupo que aponta para um evento inexistente faz o
         * vsomeip oferecer um evento que ninguem publica, sem reclamar. */
        bool ligacaoOk = true;
        for (std::size_t g = 0U; (g < grupos.count) && ligacaoOk; ++g)
        {
            const tb::GroupDesc &grupo = grupos.items[g];
            ligacaoOk = ligacaoOk && (grupo.eventCount > 0U) && (grupo.events != nullptr);
            for (std::size_t k = 0U; ligacaoOk && (k < grupo.eventCount); ++k)
            {
                ligacaoOk = (declarados.count(static_cast<unsigned>(grupo.events[k])) == 1U);
            }
        }
        chk::Checa(ligacaoOk, (std::string("  ") + s.name + ": todo evento de grupo existe no servico").c_str());
    }

    chk::Grupo("o que os nos consomem existe de fato");
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        const tb::NodeDesc &n = nos.items[i];
        bool                bom = true;
        for (std::size_t k = 0U; (k < n.provideCount) && bom; ++k)
        {
            bom = (tb::FindService(n.provides[k]) != nullptr);
        }
        for (std::size_t k = 0U; (k < n.consumeCount) && bom; ++k)
        {
            const tb::ConsumeDesc &c = n.consumes[k];
            bom = (tb::FindService(c.service) != nullptr);
            /* groupCount zero significa "todos os eventgroups", e nesse caso o
             * ponteiro tem de ser nulo. Contagem e ponteiro em desacordo seria
             * leitura fora dos limites na primeira assinatura. */
            bom = bom && ((c.groupCount == 0U) == (c.eventgroups == nullptr));
            /* E todo eventgroup citado tem de existir no servico. */
            const tb::Table<tb::GroupDesc> grupos = tb::GroupsOf(c.service);
            for (std::size_t g = 0U; bom && (g < c.groupCount); ++g)
            {
                bool achou = false;
                for (std::size_t h = 0U; (h < grupos.count) && !achou; ++h)
                {
                    achou = (grupos.items[h].id == c.eventgroups[g]);
                }
                bom = achou;
            }
        }
        chk::Checa(bom, (std::string("no ") + n.name + ": servicos e grupos citados existem").c_str());
    }

    chk::Grupo("a tabela e UMA so, apesar de viver em cabecalho");
    {
        /* C++14 nao tem inline variable, entao a tabela mora em statics dentro de
         * funcoes inline. Se isso estivesse errado, cada chamada devolveria
         * endereco diferente -- e o no generico guardaria ponteiros para copias
         * que o proximo acesso nao reconheceria. */
        chk::Checa(tb::Nodes().items == tb::Nodes().items, "duas chamadas a Nodes() dao o mesmo endereco");
        chk::Checa(tb::Services().items == tb::Services().items, "idem para Services()");
        if (servicos.count > 0U)
        {
            const char *const nome = servicos.items[0].name;
            chk::Checa(tb::EventsOf(nome).items == tb::EventsOf(nome).items, "idem para EventsOf()");
        }
    }

    std::printf("\n%zu nos, %zu servicos na tabela\n", nos.count, servicos.count);
    return chk::Veredito("test_matrix_table");
}
