/* Teste dos motores de inferencia.
 *
 * O avaliador compacto e conferido contra o `predict` do proprio XGBoost sobre
 * as mesmas linhas: e um oraculo, nao uma segunda implementacao minha. O modelo
 * e o de PROVA (experiments/bench-inference/make_probe.sh), treinado em dados
 * aleatorios -- o que importa dele e a FORMA (16 features, 5 classes, 25
 * rodadas, profundidade 8), que e o que determina a travessia e o layout.
 *
 * ⚠️ O modelo de prova tem base_score ESCALAR (0,5 expandido para as 5 classes).
 * O modelo adotado tem base_score em VETOR por classe, e e esse o caso que nao
 * cancela no softmax. Este teste, portanto, NAO exercita essa diferenca; ele
 * exercita a travessia, a interleave de arvores por classe e o softmax. Quando o
 * modelo real voltar a estar disponivel, e com ele que este teste tem de rodar.
 *
 * Uso:
 *   test_classifier <tables.bin> <probe_rows.f32> <probe_probs.txt>
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "src/ids_sensor/Classifier.hpp"
#include "tests/check.hpp"

namespace
{

/**
 * @brief Le as linhas de prova.
 * @param[in] caminho Arquivo com float32 cru, 16 por linha.
 * @return As linhas.
 */
std::vector<ids::FeatureVec> LeLinhas(const std::string &caminho)
{
    std::vector<ids::FeatureVec> linhas;
    std::ifstream                in(caminho.c_str(), std::ios::binary);
    ids::FeatureVec              v = ids::FeatureVec();
    while (in.read(reinterpret_cast<char *>(v.data()), static_cast<std::streamsize>(sizeof(v))))
    {
        linhas.push_back(v);
    }
    return linhas;
}

/**
 * @brief Le as probabilidades de referencia.
 * @param[in] caminho Arquivo de texto, uma linha por amostra.
 * @return As probabilidades.
 */
std::vector<ids::ProbVec> LeProbs(const std::string &caminho)
{
    std::vector<ids::ProbVec> probs;
    std::ifstream             in(caminho.c_str());
    std::string               linha;
    while (std::getline(in, linha))
    {
        if (linha.empty() || (linha[0] == '#'))
        {
            continue;
        }
        std::istringstream campos(linha);
        ids::ProbVec       p   = ids::ProbVec();
        bool               bom = true;
        for (std::size_t i = 0U; i < ids::kNumClasses; ++i)
        {
            double v = 0.0;
            if (!(campos >> v))
            {
                bom = false;
                break;
            }
            p[i] = static_cast<float>(v);
        }
        if (bom)
        {
            probs.push_back(p);
        }
    }
    return probs;
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

/**
 * @brief Confere um motor contra as probabilidades do XGBoost.
 * @param[in] motor  Motor a conferir.
 * @param[in] linhas Linhas de prova.
 * @param[in] refs   Probabilidades de referencia, uma por linha.
 * @return None.
 * @details Um so corpo para os dois motores, de proposito: a comparacao tem de ser a MESMA, senao
 *          "os dois concordam com o Python" significa duas coisas diferentes.
 */
void ConfereContraOraculo(const ids::IClassifier &motor, const std::vector<ids::FeatureVec> &linhas,
                          const std::vector<ids::ProbVec> &refs)
{
    const std::size_t n         = (linhas.size() < refs.size()) ? linhas.size() : refs.size();
    double            piorDiff  = 0.0;
    double            piorSoma  = 0.0;
    std::size_t       discordam = 0U;

    for (std::size_t k = 0U; k < n; ++k)
    {
        const ids::ProbVec p    = motor.Predict(linhas[k]);
        double             soma = 0.0;
        for (std::size_t c = 0U; c < ids::kNumClasses; ++c)
        {
            const double d = std::fabs(static_cast<double>(p[c]) - static_cast<double>(refs[k][c]));
            if (d > piorDiff)
            {
                piorDiff = d;
            }
            soma += static_cast<double>(p[c]);
        }
        if (std::fabs(soma - 1.0) > piorSoma)
        {
            piorSoma = std::fabs(soma - 1.0);
        }
        if (ids::ArgMax(p) != ids::ArgMax(refs[k]))
        {
            ++discordam;
            if (discordam <= 5U)
            {
                std::printf("        linha %zu: %s=%d python=%d\n", k, motor.Name(), static_cast<int>(ids::ArgMax(p)),
                            static_cast<int>(ids::ArgMax(refs[k])));
            }
        }
    }

    /* A DECISAO e o que importa, e ela tem de bater em toda linha. */
    chk::ChecaIgual(static_cast<long long>(discordam), 0, "a classe escolhida bate em TODAS as linhas");
    /* As probabilidades batem perto, nao bit a bit: a ordem das somas de margem
     * dentro do XGBoost nao e a mesma, e ambos somam em float. */
    chk::Checa(piorDiff < 1e-5, "e as probabilidades batem dentro de 1e-5");
    chk::Checa(piorSoma < 1e-5, "e cada vetor soma 1");
    std::printf("        %zu linhas; pior diferenca: %.3g; pior desvio da soma: %.3g\n", n, piorDiff, piorSoma);
}

} // namespace

int main(int argc, char **argv)
{
    /* ------------------------------------------------------------- ArgMax */
    chk::Grupo("ArgMax");
    {
        ids::ProbVec p = ids::ProbVec();
        p[0] = 0.1F; p[1] = 0.6F; p[2] = 0.1F; p[3] = 0.1F; p[4] = 0.1F;
        chk::Checa(ids::ArgMax(p) == ids::TrafficClass::Dos, "escolhe a maior");
        p[0] = 0.5F; p[1] = 0.5F; p[2] = 0.0F; p[3] = 0.0F; p[4] = 0.0F;
        chk::Checa(ids::ArgMax(p) == ids::TrafficClass::Normal, "no empate fica com a PRIMEIRA, como o numpy.argmax");
        p[0] = 0.0F; p[1] = 0.0F; p[2] = 0.0F; p[3] = 0.0F; p[4] = 1.0F;
        chk::Checa(ids::ArgMax(p) == ids::TrafficClass::MitmMulti, "acha a ultima classe");
    }

    /* ------------------------------------------- erros de carga do compacto */
    chk::Grupo("CompactClassifier::Create: os caminhos de erro");
    {
        const ara::core::Result<std::unique_ptr<ids::CompactClassifier>> r =
            ids::CompactClassifier::Create(Tmp("nao-existe-tables.bin"));
        chk::Checa(!r.HasValue(), "arquivo inexistente e recusado");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ArtefactNotFound),
                            "  com ArtefactNotFound");
        }
    }
    {
        /* Cabecalho que declara 3 classes: e um modelo de outro problema. Ler como
         * se fosse de 5 daria tres margens boas e duas de lixo, sem erro nenhum. */
        const std::string caminho = Tmp("test_classifier_3cl.bin");
        std::FILE *const  fh      = std::fopen(caminho.c_str(), "wb");
        if (fh != nullptr)
        {
            const std::int32_t cab[4] = {3, 3, 3, 16};
            static_cast<void>(std::fwrite(cab, sizeof(std::int32_t), 4U, fh));
            static_cast<void>(std::fclose(fh));
        }
        const ara::core::Result<std::unique_ptr<ids::CompactClassifier>> r = ids::CompactClassifier::Create(caminho);
        chk::Checa(!r.HasValue(), "tabela que declara 3 classes e recusada");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ArtefactCorrupted),
                            "  com ArtefactCorrupted");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }
    {
        /* Contagens certas e nenhum no: o arquivo acaba onde deveria comecar. */
        const std::string caminho = Tmp("test_classifier_curto.bin");
        std::FILE *const  fh      = std::fopen(caminho.c_str(), "wb");
        if (fh != nullptr)
        {
            const std::int32_t cab[4] = {5, 5, 10, 16};
            static_cast<void>(std::fwrite(cab, sizeof(std::int32_t), 4U, fh));
            static_cast<void>(std::fclose(fh));
        }
        const ara::core::Result<std::unique_ptr<ids::CompactClassifier>> r = ids::CompactClassifier::Create(caminho);
        chk::Checa(!r.HasValue(), "tabela truncada e recusada");
        static_cast<void>(std::remove(caminho.c_str()));
    }
    {
        /* Indice de raiz fora do array de nos. Sem a validacao da carga, isto seria
         * leitura fora dos limites na PRIMEIRA predicao. */
        const std::string caminho = Tmp("test_classifier_raiz.bin");
        std::FILE *const  fh      = std::fopen(caminho.c_str(), "wb");
        if (fh != nullptr)
        {
            const std::int32_t cab[4] = {1, 5, 1, 16};
            static_cast<void>(std::fwrite(cab, sizeof(std::int32_t), 4U, fh));
            const float base[5] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
            static_cast<void>(std::fwrite(base, sizeof(float), 5U, fh));
            const std::int32_t raiz = 99; /* fora dos 1 no */
            static_cast<void>(std::fwrite(&raiz, sizeof(raiz), 1U, fh));
            const float        folha = 1.5F;
            const std::int32_t resto[3] = {-1, 0, 0};
            static_cast<void>(std::fwrite(&folha, sizeof(folha), 1U, fh));
            static_cast<void>(std::fwrite(resto, sizeof(std::int32_t), 3U, fh));
            static_cast<void>(std::fclose(fh));
        }
        const ara::core::Result<std::unique_ptr<ids::CompactClassifier>> r = ids::CompactClassifier::Create(caminho);
        chk::Checa(!r.HasValue(), "raiz apontando fora dos nos e recusada");
        if (!r.HasValue())
        {
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ArtefactCorrupted),
                            "  com ArtefactCorrupted");
        }
        static_cast<void>(std::remove(caminho.c_str()));
    }

    /* --------------------------------------- oraculo: contra o XGBoost */
    if (argc != 4)
    {
        std::printf("\n(pulado: o oraculo precisa de <tables.bin> <rows.f32> <probs.txt>;\n");
        std::printf(" gere com: bash experiments/bench-inference/make_probe.sh)\n");
        return chk::Veredito("test_classifier");
    }

    const std::vector<ids::FeatureVec> linhas = LeLinhas(argv[2]);
    const std::vector<ids::ProbVec>    refs   = LeProbs(argv[3]);

    chk::Grupo("avaliador compacto contra o predict do XGBoost");
    {
        ara::core::Result<std::unique_ptr<ids::CompactClassifier>> r = ids::CompactClassifier::Create(argv[1]);
        chk::Checa(r.HasValue(), "tabelas do modelo de prova carregadas");
        chk::Checa(!linhas.empty(), "linhas de prova lidas");
        chk::ChecaIgual(static_cast<long long>(linhas.size()), static_cast<long long>(refs.size()),
                        "uma referencia por linha");
        if (r.HasValue())
        {
            ConfereContraOraculo(*r.Value(), linhas, refs);
        }
    }

    /* --------------------------------------------------------- TL2cgen */
    chk::Grupo("TL2cgen: linkado, confere; nao linkado, diz que nao esta");
    {
        ara::core::Result<std::unique_ptr<ids::Tl2cgenClassifier>> r = ids::Tl2cgenClassifier::Create();
        if (r.HasValue())
        {
            /* Este binario linkou o C gerado. Aqui se verifica, de uma vez, a
             * declaracao da `union Entry` (tres membros, lidos do header gerado), o
             * zeramento do buffer de resultado antes do predict, que acumula com +=,
             * e o pred_margin = 0 que faz o proprio predict aplicar o softmax. */
            chk::Checa(true, "o C gerado esta linkado e a forma do modelo bate");
            ConfereContraOraculo(*r.Value(), linhas, refs);
        }
        else
        {
            /* Sem o C gerado os simbolos fracos sao nulos. E o unico lugar do
             * projeto onde "a build esta incompleta" e uma resposta de runtime, e e
             * por isso que o ModelNotAvailable existe. */
            chk::ChecaIgual(r.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ModelNotAvailable),
                            "sem o C gerado, da ModelNotAvailable em vez de erro de ligacao");
            std::printf("        (para conferir o TL2cgen tambem, linke build/probe/tl2cgen_c/main.c)\n");
        }
    }

    return chk::Veredito("test_classifier");
}
