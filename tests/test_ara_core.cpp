/* Exercita de verdade o subconjunto ara::core com os tipos reais do ids_sensor.
 *
 * Cabecalho compilar nao prova nada para template: Result e Optional usam
 * aligned_storage com placement new, e o erro tipico ali (destrutor que nao
 * roda, ou que roda duas vezes) so aparece em execucao.
 */
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include "src/ids_sensor/Sensor.hpp"

/* ------------------------------------------------------------------ */
/* O dominio de erro precisa de definicao para o ADL fechar.          */
namespace ids
{
const ara::core::ErrorDomain &GetIdsSensorErrorDomain() noexcept
{
    static const IdsSensorErrorDomain domain;
    return domain;
}
} // namespace ids

/* ------------------------------------------------------------------ */
/* Definicoes minimas das funcoes que o componente ainda declara sem
 * implementar. Nao sao a implementacao real: o objetivo deste teste e
 * exercitar os TIPOS e os TEMPLATES, nao a logica.                   */
namespace ids
{
ByteModels ByteModels::Create(const ByteModel::LogProbTable &someip,
                              const ByteModel::LogProbTable &sd,
                              const ByteModel::LogProbTable &l4)
{
    return ByteModels(ByteModel(someip), ByteModel(sd), ByteModel(l4));
}
} // namespace ids

namespace ara
{
namespace core
{
Result<InstanceSpecifier> InstanceSpecifier::Create(const std::string &metaModelIdentifier)
{
    /* O ara::core real teria seu proprio dominio de erro; aqui reuso o do
     * componente, porque este arquivo e teste.                           */
    if (metaModelIdentifier.empty())
    {
        return Result<InstanceSpecifier>::FromError(ErrorCode(ids::IdsSensorErrc::InvalidConfiguration));
    }
    return Result<InstanceSpecifier>(InstanceSpecifier(metaModelIdentifier));
}
} // namespace core
} // namespace ara

/* ------------------------------------------------------------------ */
/* Tipo que conta construcoes e destruicoes, para provar que o
 * aligned_storage esta gerenciando o ciclo de vida certo.            */
static int g_vivos = 0;
static int g_destruidos = 0;

class Contado
{
  public:
    explicit Contado(int v) : m_v(v)
    {
        ++g_vivos;
    }
    Contado(const Contado &o) : m_v(o.m_v)
    {
        ++g_vivos;
    }
    Contado(Contado &&o) noexcept : m_v(o.m_v)
    {
        ++g_vivos;
    }
    Contado &operator=(const Contado &) = default;
    Contado &operator=(Contado &&) = default;
    ~Contado()
    {
        --g_vivos;
        ++g_destruidos;
    }
    int Value() const
    {
        return m_v;
    }

  private:
    int m_v;
};

/* ------------------------------------------------------------------ */
static int g_falhas = 0;

static void Checa(bool cond, const char *o_que)
{
    if (!cond)
    {
        std::printf("  FALHOU: %s\n", o_que);
        ++g_falhas;
    }
    else
    {
        std::printf("  ok: %s\n", o_que);
    }
}

int main()
{
    using namespace ara::core;

    std::printf("== ErrorCode pelo ADL (sem string) ==\n");
    {
        const ErrorCode ec = ids::IdsSensorErrc::ArtefactNotFound;
        Checa(ec.Value() == 1, "valor do enumerador preservado");
        Checa(std::string(ec.Domain().Name()) == "IdsSensor", "dominio resolvido por ADL");
        Checa(std::string(ec.Message()) == "artefact file not found", "mensagem vem do dominio");
        const ErrorCode outro = ids::IdsSensorErrc::ArtefactCorrupted;
        Checa(ec != outro, "codigos diferentes comparam diferentes");
        Checa(ec == ErrorCode(ids::IdsSensorErrc::ArtefactNotFound), "mesmo codigo compara igual");
        const ErrorCode comDados = ErrorCode(ids::IdsSensorErrc::ArtefactUnreadable, 13);
        Checa(comDados.SupportData() == 13, "support data (errno) carregado");
        Checa(std::string(ErrorCode(static_cast<ids::IdsSensorErrc>(99)).Message()).find("unknown") == 0U,
              "valor nao mapeado cai na mensagem generica, nao em nullptr");
    }

    std::printf("\n== Span sobre buffer real ==\n");
    {
        const std::uint8_t buf[5] = {10U, 20U, 30U, 40U, 50U};
        const ids::PayloadView v(buf);
        Checa(v.size() == 5U, "tamanho deduzido do array");
        Checa(v[2] == 30U, "indexacao");
        Checa(!v.empty(), "nao vazio");
        Checa(v.size_bytes() == 5U, "size_bytes");
        const ids::PayloadView sub = v.subspan(1U, 2U);
        Checa((sub.size() == 2U) && (sub[0] == 20U), "subspan");
        const ids::PayloadView truncado = v.subspan(3U, 99U);
        Checa(truncado.size() == 2U, "subspan satura no fim, nao estoura");
        const ids::PayloadView fora = v.subspan(9U, 1U);
        Checa(fora.empty(), "subspan com offset alem do fim da vazio");
        const ids::PayloadView vazio;
        Checa(vazio.empty() && (vazio.data() == nullptr), "vista default e vazia");
        std::size_t soma = 0U;
        for (const std::uint8_t b : v)
        {
            soma += b;
        }
        Checa(soma == 150U, "iteravel em range-for");
    }

    std::printf("\n== Result com tipo SEM construtor padrao (ByteModels) ==\n");
    {
        ids::ByteModel::LogProbTable t;
        t.fill(-1.0F);
        const ids::ByteModels m = ids::ByteModels::Create(t, t, t);
        Result<ids::ByteModels> r = m;
        Checa(r.HasValue(), "Result guarda ByteModels, que nao e default-constructible");
        Checa(r.Value().Someip().LogProb()[0] == -1.0F, "valor acessivel e correto");
        Result<ids::ByteModels> e = Result<ids::ByteModels>::FromError(ErrorCode(ids::IdsSensorErrc::ArtefactCorrupted));
        Checa(!e.HasValue(), "caminho de erro");
        Checa(e.Error().Value() == 3, "codigo de erro preservado");
        Result<ids::ByteModels> copia = r;
        Checa(copia.HasValue() && (copia.Value().Sd().LogProb()[5] == -1.0F), "copia de Result com valor");
        copia = e;
        Checa(!copia.HasValue(), "atribuicao troca valor por erro");
        copia = r;
        Checa(copia.HasValue(), "atribuicao troca erro por valor");
    }

    std::printf("\n== Result com tipo MOVE-ONLY (unique_ptr) ==\n");
    {
        Result<std::unique_ptr<Contado>> r = Result<std::unique_ptr<Contado>>::FromValue(std::unique_ptr<Contado>(new Contado(7)));
        Checa(r.HasValue() && (r.Value()->Value() == 7), "Result<unique_ptr> move-only funciona");
        Result<std::unique_ptr<Contado>> movido = std::move(r);
        Checa(movido.HasValue() && (movido.Value()->Value() == 7), "move de Result preserva o valor");
    }
    Checa(g_vivos == 0, "nenhum Contado vazou pelo Result move-only");

    std::printf("\n== ciclo de vida: o destrutor roda uma vez, nem zero nem duas ==\n");
    {
        g_vivos = 0;
        g_destruidos = 0;
        {
            Optional<Contado> o = Contado(1);
            Checa(o.has_value() && (o.value().Value() == 1), "Optional guarda o valor");
            Optional<Contado> c = o;
            Checa(c.has_value(), "copia");
            c.reset();
            Checa(!c.has_value(), "reset esvazia");
            Optional<Contado> m = std::move(o);
            Checa(m.has_value() && !o.has_value(), "move esvazia a origem");
        }
        Checa(g_vivos == 0, "saldo zero ao sair do escopo");
        Checa(g_destruidos > 0, "destrutores de fato rodaram");
    }

    std::printf("\n== Optional com os tipos reais do pipeline ==\n");
    {
        const Optional<ids::Frame> fim;
        Checa(!fim.has_value(), "fim de fluxo e Optional vazio");
        ids::Frame f;
        f.ts = 1.5;
        const Optional<ids::Frame> um = f;
        Checa(um.has_value() && (um.value().ts == 1.5), "Optional<Frame>");
        ids::FeatureVec x{};
        x[15] = 9.0F;
        const Optional<ids::FeatureVec> ov = x;
        Checa(ov.has_value() && (ov.value()[15] == 9.0F), "Optional<FeatureVec>");
        const Optional<ids::FeatureVec> semVetor;
        Checa(semVetor.value_or(ids::FeatureVec{})[15] == 0.0F, "value_or no caminho vazio");
    }

    std::printf("\n== Result<ParsedPacket>, o retorno do parser ==\n");
    {
        ids::ParsedPacket p;
        p.ts = 2.5;
        p.flow.src = 0x0A000001U;
        Result<ids::ParsedPacket> ok = p;
        Checa(ok.HasValue() && (ok.Value().flow.src == 0x0A000001U), "caminho de sucesso");
        Result<ids::ParsedPacket> ruim = Result<ids::ParsedPacket>::FromError(ErrorCode(ids::IdsSensorErrc::MalformedPacket));
        Checa(!ruim.HasValue(), "caminho de falha");
        Checa(std::string(ruim.Error().Message()).find("SOME/IP") != std::string::npos, "mensagem util no erro");
        Checa(static_cast<bool>(ok) && !static_cast<bool>(ruim), "conversao explicita para bool");
    }

    std::printf("\n== InstanceSpecifier ==\n");
    {
        const Result<InstanceSpecifier> r = InstanceSpecifier::Create("ids_sensor/SecurityEvents/Dos");
        Checa(r.HasValue(), "caminho valido aceito");
        if (r.HasValue())
        {
            Checa(r.Value().ToString() == "ids_sensor/SecurityEvents/Dos", "caminho preservado");
        }
        const Result<InstanceSpecifier> vazio = InstanceSpecifier::Create("");
        Checa(!vazio.HasValue(), "caminho vazio rejeitado, sem lancar excecao");
    }

    std::printf("\n== FlowKey e hash ==\n");
    {
        ids::FlowKey a;
        a.src = 1U;
        a.dst = 2U;
        a.sport = 3U;
        a.dport = 4U;
        a.proto = 17U;
        ids::FlowKey b = a;
        const ids::FlowKeyHash h;
        Checa(a == b, "chaves iguais");
        Checa(h(a) == h(b), "hash estavel para chaves iguais");
        b.dport = 5U;
        Checa(!(a == b), "chaves diferentes");
        Checa(h(a) != h(b), "hash distingue a porta de destino");
    }

    std::printf("\n%s (%d falhas)\n", (g_falhas == 0) ? "TUDO PASSOU" : "HOUVE FALHA", g_falhas);
    return (g_falhas == 0) ? 0 : 1;
}
