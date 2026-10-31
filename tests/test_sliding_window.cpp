/* Teste UNITARIO do ids::util::SlidingWindow.
 *
 * Antes deste componente a logica de janela estava escrita INLINE DUAS VEZES
 * dentro do FeatureStreamer::Step, e so era exercitada de raspao pelas features.
 * Aqui ela e testada sozinha, incluindo os dois pontos que a tornam diferente do
 * cmp-cpp-sliding-window de firmware: a janela COMECA VAZIA e Size() e a
 * OCUPACAO, nao a capacidade.
 */
#include <cstdint>
#include <string>
#include <vector>

#include "src/util/SlidingWindow.hpp"
#include "tests/check.hpp"

namespace
{

/// Conta construcoes e destruicoes, para provar que a janela nao vaza nem copia a esmo.
int g_vivos = 0;

/// Tipo NAO trivialmente copiavel: pega implementacao que usaria memcpy.
class Recurso
{
  public:
    explicit Recurso(int v) : m_p(new int(v))
    {
        ++g_vivos;
    }
    Recurso(const Recurso &o) : m_p(new int(*o.m_p))
    {
        ++g_vivos;
    }
    Recurso &operator=(const Recurso &o)
    {
        if (this != &o)
        {
            *m_p = *o.m_p;
        }
        return *this;
    }
    ~Recurso()
    {
        delete m_p;
        --g_vivos;
    }
    int Valor() const
    {
        return *m_p;
    }

  private:
    int *m_p;
};

} // namespace

int main()
{
    using ids::util::SlidingWindow;
    using ids::util::UtilErrc;

    chk::Grupo("Create: validacao");
    {
        chk::Checa(!SlidingWindow<int>::Create(0U).HasValue(), "capacidade zero e recusada");
        const ara::core::Result<SlidingWindow<int>> ruim = SlidingWindow<int>::Create(0U);
        chk::Checa(!ruim.HasValue() && (ruim.Error().Value() == static_cast<int>(UtilErrc::ZeroCapacity)),
                   "o codigo e ZeroCapacity");
        chk::Checa(std::string(ruim.Error().Domain().Name()) == "Util", "no dominio proprio do util");
        chk::Checa(SlidingWindow<int>::Create(1U).HasValue(), "capacidade 1 e aceita");
    }

    chk::Grupo("comeca VAZIA, e Size() e a ocupacao");
    {
        /* E aqui que esta janela difere do componente de firmware, que nasce
         * pre-preenchida e cujo size() devolve a capacidade. O extrator depende da
         * ocupacao: a taxa de repeticao divide por ela. */
        ara::core::Result<SlidingWindow<int>> r = SlidingWindow<int>::Create(3U);
        SlidingWindow<int>                   &w = r.Value();

        chk::ChecaIgual(static_cast<long long>(w.Size()), 0, "recem criada, ocupacao zero");
        chk::ChecaIgual(static_cast<long long>(w.Capacity()), 3, "capacidade e a pedida");
        chk::Checa(w.Empty(), "Empty() e verdadeiro");
        chk::Checa(!w.Full(), "Full() e falso");

        chk::Checa(!w.Push(10).has_value(), "o 1o push nao despeja");
        chk::ChecaIgual(static_cast<long long>(w.Size()), 1, "ocupacao sobe para 1");
        chk::Checa(!w.Push(20).has_value(), "o 2o push nao despeja");
        chk::Checa(!w.Push(30).has_value(), "o 3o push enche sem despejar");
        chk::ChecaIgual(static_cast<long long>(w.Size()), 3, "ocupacao chega a 3");
        chk::Checa(w.Full(), "Full() e verdadeiro");
    }

    chk::Grupo("Push devolve o item DESPEJADO");
    {
        /* E este retorno que permite manter um agregado sobre a janela sem
         * inspeciona-la: a contagem de distintos decrementa no que saiu. */
        ara::core::Result<SlidingWindow<int>> r = SlidingWindow<int>::Create(3U);
        SlidingWindow<int>                   &w = r.Value();
        static_cast<void>(w.Push(1));
        static_cast<void>(w.Push(2));
        static_cast<void>(w.Push(3));

        const ara::core::Optional<int> saiu = w.Push(4);
        chk::Checa(saiu.has_value(), "cheia, o push despeja");
        chk::ChecaIgual(saiu.value(), 1, "e o despejado e o MAIS ANTIGO");
        chk::ChecaIgual(static_cast<long long>(w.Size()), 3, "a ocupacao nao passa da capacidade");

        const ara::core::Optional<int> saiu2 = w.Push(5);
        chk::ChecaIgual(saiu2.value(), 2, "o proximo despejado e o seguinte mais antigo");
    }

    chk::Grupo("At(): indice 0 e o MAIS ANTIGO");
    {
        ara::core::Result<SlidingWindow<int>> r = SlidingWindow<int>::Create(3U);
        SlidingWindow<int>                   &w = r.Value();
        static_cast<void>(w.Push(1));
        static_cast<void>(w.Push(2));
        chk::ChecaIgual(w.At(0U), 1, "parcialmente cheia: At(0) e o mais antigo");
        chk::ChecaIgual(w.At(1U), 2, "At(1) e o seguinte");

        static_cast<void>(w.Push(3));
        static_cast<void>(w.Push(4));   /* despeja o 1 */
        chk::ChecaIgual(w.At(0U), 2, "depois do despejo, At(0) acompanha");
        chk::ChecaIgual(w.At(1U), 3, "At(1)");
        chk::ChecaIgual(w.At(2U), 4, "At(2) e o mais novo");

        /* Da a volta no anel varias vezes: e aqui que aritmetica de indice erra. */
        for (int i = 5; i <= 20; ++i)
        {
            static_cast<void>(w.Push(i));
        }
        chk::ChecaIgual(w.At(0U), 18, "apos varias voltas, At(0) ainda e o mais antigo");
        chk::ChecaIgual(w.At(1U), 19, "At(1)");
        chk::ChecaIgual(w.At(2U), 20, "At(2)");
        chk::ChecaIgual(static_cast<long long>(w.Size()), 3, "e a ocupacao segue na capacidade");
    }

    chk::Grupo("capacidade 1: o caso degenerado");
    {
        ara::core::Result<SlidingWindow<int>> r = SlidingWindow<int>::Create(1U);
        SlidingWindow<int>                   &w = r.Value();
        chk::Checa(!w.Push(7).has_value(), "o 1o push nao despeja");
        chk::ChecaIgual(w.At(0U), 7, "At(0) e o unico item");
        const ara::core::Optional<int> saiu = w.Push(8);
        chk::ChecaIgual(saiu.value(), 7, "o 2o push despeja o 1o");
        chk::ChecaIgual(w.At(0U), 8, "e At(0) passa a ser o novo");
        chk::ChecaIgual(static_cast<long long>(w.Size()), 1, "ocupacao segue 1");
    }

    chk::Grupo("Clear");
    {
        ara::core::Result<SlidingWindow<int>> r = SlidingWindow<int>::Create(3U);
        SlidingWindow<int>                   &w = r.Value();
        static_cast<void>(w.Push(1));
        static_cast<void>(w.Push(2));
        w.Clear();
        chk::ChecaIgual(static_cast<long long>(w.Size()), 0, "Clear zera a ocupacao");
        chk::ChecaIgual(static_cast<long long>(w.Capacity()), 3, "e preserva a capacidade");
        chk::Checa(!w.Push(9).has_value(), "depois do Clear volta a nao despejar");
        chk::ChecaIgual(w.At(0U), 9, "e o indice recomeca");
    }

    chk::Grupo("copia: a Regra de Tres");
    {
        /* O componente de firmware tem destrutor com delete[] e NENHUM construtor
         * de copia, entao copiar uma janela dava double free. Este guarda em
         * std::vector, logo a copia que o compilador gera e correta, e este caso
         * existe para que uma mudanca para ponteiro cru nao passe calada. */
        ara::core::Result<SlidingWindow<int>> r = SlidingWindow<int>::Create(3U);
        SlidingWindow<int>                   &w = r.Value();
        static_cast<void>(w.Push(1));
        static_cast<void>(w.Push(2));

        SlidingWindow<int> copia = w;
        chk::ChecaIgual(static_cast<long long>(copia.Size()), 2, "a copia tem a mesma ocupacao");
        chk::ChecaIgual(copia.At(0U), 1, "e o mesmo conteudo");

        static_cast<void>(copia.Push(3));
        static_cast<void>(copia.Push(4));
        chk::ChecaIgual(static_cast<long long>(w.Size()), 2, "mexer na copia nao mexe na original");
        chk::ChecaIgual(w.At(0U), 1, "o conteudo da original segue intacto");
    }

    chk::Grupo("tipo NAO trivialmente copiavel");
    {
        /* Uma implementacao com memcpy passaria no caso de int e falharia aqui,
         * com double free ou contagem de vivos diferente de zero. */
        g_vivos = 0;
        {
            ara::core::Result<SlidingWindow<Recurso>> r = SlidingWindow<Recurso>::Create(2U);
            SlidingWindow<Recurso>                   &w = r.Value();
            static_cast<void>(w.Push(Recurso(1)));
            static_cast<void>(w.Push(Recurso(2)));
            chk::ChecaIgual(w.At(0U).Valor(), 1, "guarda o objeto corretamente");

            const ara::core::Optional<Recurso> saiu = w.Push(Recurso(3));
            chk::Checa(saiu.has_value(), "despeja");
            chk::ChecaIgual(saiu.value().Valor(), 1, "e o objeto despejado chega intacto");
            chk::ChecaIgual(w.At(1U).Valor(), 3, "o mais novo esta no lugar");
        }
        chk::ChecaIgual(g_vivos, 0, "saldo zero de objetos ao sair do escopo");
    }

    return chk::Veredito("test_sliding_window");
}
