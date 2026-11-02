/* Confere o PLANO que o no generico deriva da matriz, sem subir barramento.
 *
 * O no generico faz duas coisas separaveis: deriva da matriz o que publicar e o
 * que assinar, e depois roda uma pilha vsomeip fazendo exatamente isso. Só a
 * segunda metade precisa de barramento. Este teste cobre a primeira.
 *
 * Essa separacao nao e enfeite. A correlacao de eventgroups do extrator JA
 * esteve errada de um jeito que parecia certo: usar "os destinos do evento sao
 * SUBCONJUNTO dos assinantes do grupo" casa sempre que o evento tem um destino
 * so, e o sintoma era quatro eventgroups para quatro eventos -- que e tambem a
 * aparencia da resposta correta. A licao e que eventgroup se confere contra uma
 * matriz conhecida, nao a olho.
 *
 * As asercoes sao de duas naturezas:
 *   - ESTRUTURAIS, validas para qualquer matriz: todo no da tabela planeja sem
 *     reclamacao, toda publicacao tem grupo, toda assinatura cita grupo que de
 *     fato carrega o evento;
 *   - de VOLUME, especificas da matriz recuperada, e guardadas por FindNode:
 *     um consumidor que assina UM eventgroup de um servico de quatro eventos
 *     tem de assinar UM evento, nao quatro. E essa conta que a classe de erro
 *     acima estraga, e e ela que decide se o volume de trafego reproduzido bate
 *     com o do barramento de referencia.
 */
#include <cstdio>
#include <set>
#include <string>

#include "src/nodes/MatrixPlan.hpp"
#include "tests/check.hpp"

namespace tbl = ids::matrix::table;
namespace nds = ids::nodes;

namespace
{

/* Quantos eventos um servico declara. */
std::size_t QuantosEventos(const char *servico)
{
    return tbl::EventsOf(servico).count;
}

/* Um grupo de um servico carrega um evento? */
bool GrupoCarrega(const char *servico, ids::ecu::EventgroupId grupo, ids::ecu::EventId evento)
{
    const tbl::Table<tbl::GroupDesc> grupos = tbl::GroupsOf(servico);
    for (std::size_t g = 0U; g < grupos.count; ++g)
    {
        if (grupos.items[g].id == grupo)
        {
            for (std::size_t k = 0U; k < grupos.items[g].eventCount; ++k)
            {
                if (grupos.items[g].events[k] == evento)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

int main()
{
    const tbl::Table<tbl::NodeDesc> nos = tbl::Nodes();

    chk::Grupo("todo no da matriz planeja sem reclamacao");
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        const nds::NodePlan plano = nds::PlanFor(nos.items[i].name);
        const std::string   oQue  = std::string("no ") + nos.items[i].name;
        /* Um problema aqui significa que a matriz e a tabela gerada discordam, o
         * que os outros dois testes existem para impedir. Achar um aqui e a
         * ultima linha de defesa, nao o caminho normal. */
        chk::Checa((plano.node == &nos.items[i]) && plano.problem.empty(),
                   (plano.problem.empty() ? oQue : (oQue + ": " + plano.problem)).c_str());
    }

    chk::Grupo("um nome que nao existe nao planeja");
    {
        const nds::NodePlan plano = nds::PlanFor("nao_existe_este_no");
        chk::Checa(plano.node == nullptr, "no inexistente devolve node nulo");
        chk::Checa(!plano.problem.empty(), "e diz o que esta errado");
        chk::Checa(plano.publishes.empty() && plano.consumes.empty(), "e nao planeja nada");
    }

    chk::Grupo("publicacoes: uma por evento de cada servico oferecido");
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        const tbl::NodeDesc &no    = nos.items[i];
        const nds::NodePlan  plano = nds::PlanFor(no.name);

        std::size_t esperadas = 0U;
        for (std::size_t k = 0U; k < no.provideCount; ++k)
        {
            esperadas += QuantosEventos(no.provides[k]);
        }
        chk::Checa(plano.publishes.size() == esperadas,
                   (std::string("no ") + no.name + ": publicacoes batem com os eventos oferecidos").c_str());

        /* Um evento sem eventgroup nao e ofertavel: o vsomeip precisa de um grupo
         * para anunciar, e o ProvideEvent recusa lista vazia. */
        bool bom = true;
        for (std::size_t k = 0U; k < plano.publishes.size() && bom; ++k)
        {
            const nds::EventPlan &ep = plano.publishes[k];
            bom                      = (!ep.eventgroups.empty()) && (ep.service != 0U) && (ep.instance != 0U) && (ep.notifier != 0U);
            for (std::size_t g = 0U; bom && (g < ep.eventgroups.size()); ++g)
            {
                bom = GrupoCarrega(ep.serviceName.c_str(), ep.eventgroups[g], ep.notifier);
            }
        }
        chk::Checa(bom, (std::string("  ") + no.name + ": todo evento tem grupo, e o grupo o carrega").c_str());
    }

    chk::Grupo("assinaturas: so os eventgroups que a matriz nomeia");
    for (std::size_t i = 0U; i < nos.count; ++i)
    {
        const tbl::NodeDesc &no    = nos.items[i];
        const nds::NodePlan  plano = nds::PlanFor(no.name);

        chk::Checa(plano.consumes.size() == no.consumeCount,
                   (std::string("no ") + no.name + ": um plano de consumo por servico consumido").c_str());

        bool bom = true;
        for (std::size_t k = 0U; (k < plano.consumes.size()) && bom; ++k)
        {
            const nds::ConsumePlan &cp = plano.consumes[k];
            const tbl::ConsumeDesc &cd = no.consumes[k];

            bom = (cp.service != 0U) && (cp.instance != 0U) && (!cp.subscriptions.empty());

            std::map<ids::ecu::EventId, std::vector<ids::ecu::EventgroupId> >::const_iterator it = cp.subscriptions.begin();
            for (; bom && (it != cp.subscriptions.end()); ++it)
            {
                /* Sem repeticao e em ordem: o evento que cai em dois grupos
                 * assinados vira UMA assinatura com os dois, nao duas. */
                const std::set<ids::ecu::EventgroupId> unicos(it->second.begin(), it->second.end());
                bom = (unicos.size() == it->second.size()) && (!it->second.empty());

                for (std::size_t g = 0U; bom && (g < it->second.size()); ++g)
                {
                    /* O grupo carrega o evento... */
                    bom = GrupoCarrega(cd.service, it->second[g], it->first);
                    /* ...e, quando a matriz nomeia grupos, e um dos nomeados. */
                    if (bom && (cd.groupCount > 0U))
                    {
                        bool nomeado = false;
                        for (std::size_t h = 0U; (h < cd.groupCount) && (!nomeado); ++h)
                        {
                            nomeado = (cd.eventgroups[h] == it->second[g]);
                        }
                        bom = nomeado;
                    }
                }
            }
        }
        chk::Checa(bom, (std::string("  ") + no.name + ": todo grupo assinado carrega o evento e foi pedido").c_str());
    }

    chk::Grupo("o grupo nao citado NAO entra");
    {
        /* O erro que esta classe de teste existe para pegar: tratar todo
         * consumidor como se assinasse tudo. Para cada consumo que nomeia
         * grupos, a conta de assinaturas tem de ser a dos eventos DESSES grupos
         * -- nunca a de todos os eventos do servico. */
        bool        conferiuAlgum = false;
        bool        bom           = true;
        std::size_t discriminou   = 0U;
        for (std::size_t i = 0U; i < nos.count; ++i)
        {
            const tbl::NodeDesc &no    = nos.items[i];
            const nds::NodePlan  plano = nds::PlanFor(no.name);
            for (std::size_t k = 0U; (k < plano.consumes.size()) && bom; ++k)
            {
                const tbl::ConsumeDesc &cd = no.consumes[k];
                if (cd.groupCount == 0U)
                {
                    continue;
                }
                conferiuAlgum = true;

                std::set<ids::ecu::EventId>      esperados;
                const tbl::Table<tbl::GroupDesc> grupos = tbl::GroupsOf(cd.service);
                for (std::size_t g = 0U; g < grupos.count; ++g)
                {
                    bool pedido = false;
                    for (std::size_t h = 0U; (h < cd.groupCount) && (!pedido); ++h)
                    {
                        pedido = (cd.eventgroups[h] == grupos.items[g].id);
                    }
                    if (pedido)
                    {
                        for (std::size_t e = 0U; e < grupos.items[g].eventCount; ++e)
                        {
                            esperados.insert(grupos.items[g].events[e]);
                        }
                    }
                }

                bom = (plano.consumes[k].subscriptions.size() == esperados.size());
                /* Discrimina de verdade so quando o servico tem mais eventos do
                 * que os grupos pedidos carregam; caso contrario "assina tudo" e
                 * "assina o pedido" dao o mesmo numero. */
                if (esperados.size() < QuantosEventos(cd.service))
                {
                    ++discriminou;
                }
            }
        }
        if (conferiuAlgum)
        {
            chk::Checa(bom, "assina exatamente os eventos dos grupos pedidos");
            chk::Checa(discriminou > 0U, "e ao menos um caso distingue isso de assinar tudo");
        }
        else
        {
            /* A matriz da bancada usa a forma curta (assina todos os grupos), e
             * ai nao ha o que distinguir. E a matriz recuperada que exercita
             * isto, e ela passa pelo mesmo gerador. */
            std::printf("  (nenhum consumo nomeia eventgroup nesta matriz: nada a distinguir)\n");
        }
    }

    std::printf("\n%zu nos planejados\n", nos.count);
    return chk::Veredito("test_matrix_plan");
}
