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
/* O dominio de erro tem definicao em src/ids_sensor/IdsSensorErrorDomain.cpp,
 * que este teste linka. Antes dele existir, a definicao ficava aqui.    */

/* ------------------------------------------------------------------ */
/* Definicao minima da unica funcao que ainda nao tem implementacao real.
 * O objetivo deste arquivo e exercitar os TIPOS e os TEMPLATES, nao a
 * logica do componente.                                              */
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

    std::printf("\n== Result com tipo SEM construtor padrao (ByteModel) ==\n");
    {
        /* ByteModel nao tem construtor padrao, e e por isso que serve aqui: elimina
         * a implementacao preguicosa de Result com um membro `T value;`. O tipo
         * usado antes era ByteModels, mas ele passou a sair por unique_ptr quando o
         * ByteModel virou posicional, e este teste nao acompanhou a mudanca. */
        ids::ByteModel::LogProbTable t;
        for (std::size_t pos = 0U; pos < ids::kMaxPositions; ++pos)
        {
            t[pos].fill(-1.0);
        }

        Result<ids::ByteModel> r = ids::ByteModel::Create(t, 8U);
        Checa(r.HasValue(), "Result guarda ByteModel, que nao e default-constructible");
        Checa(r.Value().Positions() == 8U, "valor acessivel e correto");

        Result<ids::ByteModel> e = Result<ids::ByteModel>::FromError(ErrorCode(ids::IdsSensorErrc::ArtefactCorrupted));
        Checa(!e.HasValue(), "caminho de erro");
        Checa(e.Error().Value() == 3, "codigo de erro preservado");

        Result<ids::ByteModel> copia = r;
        Checa(copia.HasValue() && (copia.Value().Positions() == 8U), "copia de Result com valor");
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

    std::printf("\n== Result<void>: sucesso sem valor ==\n");
    {
        /* Acrescentado depois dos demais tipos, para uma operacao como oferecer um
         * servico: ou funciona, ou falha com um motivo, e nao ha o que retornar no
         * sucesso. Result<bool> perderia o motivo, e um ErrorCode nu forcaria um
         * codigo "ok" no dominio. */
        const Result<void> ok;
        Checa(ok.HasValue(), "construido sem argumento e sucesso");
        Checa(static_cast<bool>(ok), "converte para true");

        const Result<void> falhou = Result<void>::FromError(ErrorCode(ids::IdsSensorErrc::ArtefactCorrupted));
        Checa(!falhou.HasValue(), "FromError e falha");
        Checa(!static_cast<bool>(falhou), "converte para false");
        Checa(falhou.Error().Value() == 3, "o codigo de erro e preservado");
        Checa(std::string(falhou.Error().Message()).find("layout") != std::string::npos, "e a mensagem tambem");

        const Result<void> viaFromValue = Result<void>::FromValue();
        Checa(viaFromValue.HasValue(), "FromValue e sucesso");

        /* Copia e atribuicao, que e onde um Optional<E> interno mal feito quebra. */
        Result<void> copia = falhou;
        Checa(!copia.HasValue() && (copia.Error().Value() == 3), "copia preserva a falha");
        copia = ok;
        Checa(copia.HasValue(), "atribuicao troca falha por sucesso");
        copia = falhou;
        Checa(!copia.HasValue(), "e sucesso por falha");
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
