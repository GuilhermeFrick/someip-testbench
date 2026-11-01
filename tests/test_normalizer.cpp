/* Teste unitario do Normalizer.
 *
 * Os dois casos que importam nao sao aritmetica: sao as duas coisas que a
 * normalizacao min-max "normalmente" faz e que a referencia NAO faz. Se eu
 * tivesse escrito o Normalizer de cabeca, teria errado as duas:
 *
 *   1. feature constante no treino (xmax == xmin) NAO e erro: divide por 1,0;
 *   2. valor fora da faixa do treino NAO e saturado: sai abaixo de 0 ou acima
 *      de 1 e vai assim para as arvores.
 *
 * Os dois vem de `extract_all()` em src/pipeline.py do repo de reproducao, que e
 * o codigo que normalizou as features com que o modelo foi treinado.
 */
#include <cstdint>
#include <cstdio>
#include <string>

#include "src/ids_sensor/Normalizer.hpp"
#include "tests/check.hpp"

namespace
{

/**
 * @brief Limites simples: xmin = 0 e xmax = 10 em toda feature.
 * @return O par de limites.
 */
ids::BoundsVec Preenche(double valor)
{
    ids::BoundsVec v = ids::BoundsVec();
    for (std::size_t i = 0U; i < ids::kNumFeatures; ++i)
    {
        v[i] = valor;
    }
    return v;
}

/**
 * @brief Escreve um artefato de limites no formato que o Create le.
 * @param[in] caminho  Arquivo a criar.
 * @param[in] nFeat    Numero de features a DECLARAR no cabecalho.
 * @param[in] nEscritas Quantos valores escrever em cada vetor.
 * @param[in] xmin     Valor usado em todo xmin.
 * @param[in] xmax     Valor usado em todo xmax.
 * @return Whether the file could be written.
 * @details nFeat e nEscritas sao separados de proposito, para montar os dois
 *          artefatos corrompidos: um que declara o numero errado e um que declara
 *          o certo mas acaba antes.
 */
bool Escreve(const std::string &caminho, std::uint32_t nFeat, std::size_t nEscritas, double xmin, double xmax)
{
    std::FILE *const fh = std::fopen(caminho.c_str(), "wb");
    if (fh == nullptr)
    {
        return false;
    }
    static_cast<void>(std::fwrite(&nFeat, sizeof(nFeat), 1U, fh));
    for (std::size_t i = 0U; i < nEscritas; ++i)
    {
        static_cast<void>(std::fwrite(&xmin, sizeof(double), 1U, fh));
    }
    for (std::size_t i = 0U; i < nEscritas; ++i)
    {
        static_cast<void>(std::fwrite(&xmax, sizeof(double), 1U, fh));
    }
    return (std::fclose(fh) == 0);
}

/**
 * @brief Caminho de apoio dentro de /tmp.
 * @param[in] nome Nome do arquivo.
 * @return O caminho completo.
 */
std::string Tmp(const char *nome)
{
    return std::string("/tmp/") + nome;
}

} // namespace

int main()
{
    /* ------------------------------------------------------------- aritmetica */
    chk::Grupo("Apply: a conta");
    {
        const ids::Normalizer n = ids::Normalizer::FromBounds(Preenche(0.0), Preenche(10.0));
        ids::FeatureVec       x = ids::FeatureVec();
        x[0]                    = 0.0F;
        x[1]                    = 5.0F;
        x[2]                    = 10.0F;
        const ids::FeatureVec y = n.Apply(x);
        chk::ChecaIgualF(static_cast<double>(y[0]), 0.0, "o proprio xmin vira 0");
        chk::ChecaIgualF(static_cast<double>(y[1]), 0.5, "o meio da faixa vira 0,5");
        chk::ChecaIgualF(static_cast<double>(y[2]), 1.0, "o proprio xmax vira 1");
    }
    {
        /* xmin negativo, para o deslocamento nao poder ser confundido com uma
         * divisao so. */
        ids::BoundsVec xmin = Preenche(-4.0);
        ids::BoundsVec xmax = Preenche(4.0);
        const ids::Normalizer n = ids::Normalizer::FromBounds(xmin, xmax);
        ids::FeatureVec       x = ids::FeatureVec();
        x[0]                    = -4.0F;
        x[1]                    = 0.0F;
        const ids::FeatureVec y = n.Apply(x);
        chk::ChecaIgualF(static_cast<double>(y[0]), 0.0, "xmin negativo tambem vira 0");
        chk::ChecaIgualF(static_cast<double>(y[1]), 0.5, "o zero cai no meio quando a faixa e simetrica");
    }

    /* ------------------------------------------------- feature degenerada */
    chk::Grupo("feature constante no treino: divisor 1,0, NAO erro");
    {
        ids::BoundsVec xmin = Preenche(0.0);
        ids::BoundsVec xmax = Preenche(10.0);
        /* Feature 3 constante em 7: e o caso where(xmax > xmin, ..., 1.0). */
        xmin[3] = 7.0;
        xmax[3] = 7.0;
        /* Feature 4 INVERTIDA, que um artefato trocado produziria. A referencia
         * usa `>` estrito, entao ela tambem cai no divisor 1,0. */
        xmin[4] = 9.0;
        xmax[4] = 2.0;

        const ids::Normalizer n = ids::Normalizer::FromBounds(xmin, xmax);
        chk::ChecaIgualF(n.Range()[3], 1.0, "feature constante tem divisor 1,0");
        chk::ChecaIgualF(n.Range()[4], 1.0, "feature com xmax < xmin tambem tem divisor 1,0");
        chk::ChecaIgualF(n.Range()[0], 10.0, "as outras seguem com xmax-xmin");

        ids::FeatureVec x = ids::FeatureVec();
        x[3]              = 9.0F;
        x[4]              = 9.0F;
        const ids::FeatureVec y = n.Apply(x);
        chk::ChecaIgualF(static_cast<double>(y[3]), 2.0, "constante: sai x-xmin, nao zero nem NaN");
        chk::ChecaIgualF(static_cast<double>(y[4]), 0.0, "invertida: sai x-xmin");

        /* O defeito que este caso existe para pegar: divisor zero. */
        chk::Checa(!(y[3] != y[3]), "e nao e NaN");
        chk::Checa((y[3] < 1e30F) && (y[3] > -1e30F), "e nao e infinito");
    }

    /* ---------------------------------------------------- sem saturacao */
    chk::Grupo("fora da faixa do treino: NAO satura");
    {
        const ids::Normalizer n = ids::Normalizer::FromBounds(Preenche(0.0), Preenche(10.0));
        ids::FeatureVec       x = ids::FeatureVec();
        x[0]                    = 20.0F; /* o dobro do xmax */
        x[1]                    = -10.0F;
        const ids::FeatureVec y = n.Apply(x);
        chk::ChecaIgualF(static_cast<double>(y[0]), 2.0, "acima do xmax sai 2, nao 1");
        chk::ChecaIgualF(static_cast<double>(y[1]), -1.0, "abaixo do xmin sai -1, nao 0");
        /* Isto nao e detalhe: saturar deslocaria a fronteira de decisao exatamente
         * nos valores extremos, que sao os ataques. */
    }

    /* ------------------------------------------------------ artefato */
    chk::Grupo("Create: os caminhos de erro do artefato");
    {
        const ara::core::Result<ids::Normalizer> r = ids::Normalizer::Create(Tmp("nao-existe-minmax.bin"));
        chk::Checa(!r.HasValue(), "arquivo inexistente e recusado");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ArtefactNotFound),
                            "  com ArtefactNotFound");
        }
    }
    {
        /* Declara 12 features: e o artefato do baseline de 12, nao o nosso. Ler
         * isto como se fosse de 16 daria as arvores doze valores deslocados e
         * quatro de lixo, sem erro nenhum. */
        const std::string caminho = Tmp("test_normalizer_12.bin");
        chk::Checa(Escreve(caminho, 12U, 12U, 0.0, 1.0), "artefato de 12 features escrito");
        const ara::core::Result<ids::Normalizer> r = ids::Normalizer::Create(caminho);
        chk::Checa(!r.HasValue(), "artefato que declara 12 features e recusado");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ArtefactCorrupted),
                            "  com ArtefactCorrupted");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }
    {
        /* Declara 16 e acaba na metade. */
        const std::string caminho = Tmp("test_normalizer_curto.bin");
        chk::Checa(Escreve(caminho, static_cast<std::uint32_t>(ids::kNumFeatures), 8U, 0.0, 1.0),
                   "artefato truncado escrito");
        const ara::core::Result<ids::Normalizer> r = ids::Normalizer::Create(caminho);
        chk::Checa(!r.HasValue(), "artefato que acaba antes do fim e recusado");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ArtefactCorrupted),
                            "  com ArtefactCorrupted");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }
    {
        const std::string caminho = Tmp("test_normalizer_ok.bin");
        chk::Checa(Escreve(caminho, static_cast<std::uint32_t>(ids::kNumFeatures), ids::kNumFeatures, -2.0, 6.0),
                   "artefato bem formado escrito");
        const ara::core::Result<ids::Normalizer> r = ids::Normalizer::Create(caminho);
        chk::Checa(r.HasValue(), "artefato bem formado e aceito");
        if (r.HasValue())
        {
            chk::ChecaIgualF(r.Value().Xmin()[0], -2.0, "  xmin voltou do arquivo");
            chk::ChecaIgualF(r.Value().Xmax()[0], 6.0, "  xmax voltou do arquivo");
            chk::ChecaIgualF(r.Value().Range()[0], 8.0, "  e o divisor e xmax-xmin");
            ids::FeatureVec x = ids::FeatureVec();
            x[0]              = 2.0F;
            chk::ChecaIgualF(static_cast<double>(r.Value().Apply(x)[0]), 0.5, "  e a conta usa esses limites");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }

    return chk::Veredito("test_normalizer");
}
