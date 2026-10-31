/* Teste UNITARIO do ByteModel e do HammingDistance.
 *
 * O teste de paridade cobre estas duas funcoes de raspao, so pelos valores que a
 * sequencia sintetica produz. Aqui cada ramo e atingido de proposito, incluindo
 * os caminhos de erro da carga do artefato, que a paridade nunca exercita porque
 * sempre recebe um arquivo bom.
 */
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "src/ids_sensor/ByteModel.hpp"
#include "tests/check.hpp"

namespace
{

/**
 * @brief Tabela em que logP[posicao][valor] = -(posicao + 1), independente do valor.
 * @return A tabela.
 * @details Depender so da POSICAO e o que torna um erro de indexacao posicional visivel:
 *          se o codigo usasse sempre a linha 0, todo payload daria o mesmo resultado.
 */
ids::ByteModel::LogProbTable TabelaPorPosicao()
{
    ids::ByteModel::LogProbTable t;
    for (std::size_t pos = 0U; pos < ids::kMaxPositions; ++pos)
    {
        for (std::size_t val = 0U; val < ids::kByteValues; ++val)
        {
            t[pos][val] = -(static_cast<double>(pos) + 1.0);
        }
    }
    return t;
}

/**
 * @brief Tabela em que logP[posicao][valor] = -(valor + 1), independente da posicao.
 * @return A tabela.
 */
ids::ByteModel::LogProbTable TabelaPorValor()
{
    ids::ByteModel::LogProbTable t;
    for (std::size_t pos = 0U; pos < ids::kMaxPositions; ++pos)
    {
        for (std::size_t val = 0U; val < ids::kByteValues; ++val)
        {
            t[pos][val] = -(static_cast<double>(val) + 1.0);
        }
    }
    return t;
}

ids::PayloadView Vista(const std::vector<std::uint8_t> &v)
{
    return ids::PayloadView(v.data(), v.size());
}

/**
 * @brief Escreve um artefato de tabelas, opcionalmente estragado.
 * @param[in] caminho   Onde escrever.
 * @param[in] posicoes  Contagens a declarar para as tres camadas.
 * @param[in] truncaEm  Byte em que truncar o arquivo; 0 escreve inteiro.
 * @return Whether the file was written.
 */
bool EscreveArtefato(const std::string &caminho, const std::size_t posicoes[3], std::size_t truncaEm)
{
    std::FILE *f = std::fopen(caminho.c_str(), "wb");
    if (f == nullptr)
    {
        return false;
    }
    std::vector<unsigned char> buffer;
    for (std::size_t camada = 0U; camada < 3U; ++camada)
    {
        const std::uint32_t n = static_cast<std::uint32_t>(posicoes[camada]);
        const unsigned char *p = reinterpret_cast<const unsigned char *>(&n);
        buffer.insert(buffer.end(), p, p + sizeof(n));
        for (std::size_t i = 0U; i < posicoes[camada]; ++i)
        {
            for (std::size_t v = 0U; v < ids::kByteValues; ++v)
            {
                const double d  = -1.5;
                const unsigned char *pd = reinterpret_cast<const unsigned char *>(&d);
                buffer.insert(buffer.end(), pd, pd + sizeof(d));
            }
        }
    }
    const std::size_t escrever = ((truncaEm != 0U) && (truncaEm < buffer.size())) ? truncaEm : buffer.size();
    static_cast<void>(std::fwrite(buffer.data(), 1U, escrever, f));
    static_cast<void>(std::fclose(f));
    return true;
}

} // namespace

int main()
{
    /* ---------------------------------------------------------- HammingDistance */
    chk::Grupo("HammingDistance: todos os ramos");
    {
        const std::vector<std::uint8_t> vazio;
        const std::vector<std::uint8_t> a   = {1U, 2U, 3U, 4U};
        const std::vector<std::uint8_t> aIgual = {1U, 2U, 3U, 4U};
        const std::vector<std::uint8_t> b   = {1U, 9U, 3U, 9U};
        const std::vector<std::uint8_t> maior = {1U, 2U, 3U, 4U, 5U, 6U};
        const std::vector<std::uint8_t> menor = {1U, 2U};

        chk::ChecaIgual(ids::HammingDistance(Vista(vazio), Vista(vazio)), 0, "dois vazios dao zero");
        chk::ChecaIgual(ids::HammingDistance(Vista(a), Vista(vazio)), 4, "um vazio da o tamanho do outro");
        chk::ChecaIgual(ids::HammingDistance(Vista(vazio), Vista(a)), 4, "simetrico no caso do vazio");
        chk::ChecaIgual(ids::HammingDistance(Vista(a), Vista(aIgual)), 0, "iguais dao zero");
        chk::ChecaIgual(ids::HammingDistance(Vista(a), Vista(b)), 2, "duas posicoes diferentes");
        chk::ChecaIgual(ids::HammingDistance(Vista(a), Vista(maior)), 2, "prefixo igual, so a diferenca de comprimento");
        chk::ChecaIgual(ids::HammingDistance(Vista(maior), Vista(a)), 2, "simetrico no comprimento");
        chk::ChecaIgual(ids::HammingDistance(Vista(a), Vista(menor)), 2, "encurtou: a diferenca de comprimento conta");
        chk::ChecaIgual(ids::HammingDistance(Vista(b), Vista(maior)), 4, "duas diferencas mais dois de comprimento");
    }

    /* -------------------------------------------------------- ByteModel::Create */
    chk::Grupo("ByteModel::Create: validacao");
    {
        const ids::ByteModel::LogProbTable t = TabelaPorPosicao();
        chk::Checa(!ids::ByteModel::Create(t, 0U).HasValue(), "zero posicoes e recusado");
        chk::Checa(!ids::ByteModel::Create(t, ids::kMaxPositions + 1U).HasValue(), "acima do maximo e recusado");
        const ara::core::Result<ids::ByteModel> ok = ids::ByteModel::Create(t, 64U);
        chk::Checa(ok.HasValue(), "64 posicoes e aceito");
        if (ok.HasValue())
        {
            chk::ChecaIgual(static_cast<long long>(ok.Value().Positions()), 64, "Positions() devolve o L dado");
        }
        const ara::core::Result<ids::ByteModel> ruim = ids::ByteModel::Create(t, 0U);
        chk::Checa(!ruim.HasValue() && (ruim.Error().Value() == static_cast<int>(ids::IdsSensorErrc::InvalidConfiguration)),
                   "o codigo de erro e InvalidConfiguration");
    }

    /* --------------------------------------------------------- ByteModel::Score */
    chk::Grupo("ByteModel::Score: o modelo e POSICIONAL");
    {
        /* Com logP = -(pos+1), um payload de n bytes soma -(1+2+...+n). Se o codigo
         * usasse sempre a posicao 0, a soma seria -n. */
        const ara::core::Result<ids::ByteModel> r = ids::ByteModel::Create(TabelaPorPosicao(), 4U);
        chk::Checa(r.HasValue(), "modelo com L=4 criado");
        const ids::ByteModel &m = r.Value();

        const std::vector<std::uint8_t> vazio;
        const std::vector<std::uint8_t> um    = {7U};
        const std::vector<std::uint8_t> tres  = {7U, 7U, 7U};
        const std::vector<std::uint8_t> exato = {7U, 7U, 7U, 7U};
        const std::vector<std::uint8_t> longo = {7U, 7U, 7U, 7U, 7U, 7U};

        chk::ChecaIgualF(m.LogLik(Vista(vazio)), 0.0, "payload vazio soma zero");
        chk::ChecaIgualF(m.CrossEntropy(Vista(vazio)), 0.0, "payload vazio tem entropia zero, nao NaN");
        chk::ChecaIgualF(m.LogLik(Vista(um)), -1.0, "um byte usa a posicao 0");
        chk::ChecaIgualF(m.LogLik(Vista(tres)), -6.0, "tres bytes somam -(1+2+3), nao -3");
        chk::ChecaIgualF(m.LogLik(Vista(exato)), -10.0, "L bytes somam -(1+2+3+4)");
        chk::ChecaIgualF(m.LogLik(Vista(longo)), -10.0, "acima de L trunca em L");

        /* A entropia divide pelos bytes PONTUADOS, nao por L. Com 3 bytes e L=4,
         * -(-6)/3 = 2. Dividir por L daria 1,5. */
        chk::ChecaIgualF(m.CrossEntropy(Vista(tres)), 2.0, "entropia divide por k=3, nao por L=4");
        chk::ChecaIgualF(m.CrossEntropy(Vista(longo)), 2.5, "acima de L divide por L, que e o k truncado");

        const ids::PayloadScores dois = m.Score(Vista(tres));
        chk::ChecaIgualF(dois.logLik, m.LogLik(Vista(tres)), "Score concorda com LogLik");
        chk::ChecaIgualF(dois.crossEntropy, m.CrossEntropy(Vista(tres)), "Score concorda com CrossEntropy");
    }

    chk::Grupo("ByteModel::Score: o VALOR do byte tambem indexa");
    {
        const ara::core::Result<ids::ByteModel> r = ids::ByteModel::Create(TabelaPorValor(), 8U);
        const ids::ByteModel                  &m = r.Value();
        const std::vector<std::uint8_t>        v = {0U, 1U, 2U};
        /* logP = -(valor+1): -(0+1) + -(1+1) + -(2+1) = -6 */
        chk::ChecaIgualF(m.LogLik(Vista(v)), -6.0, "soma usa o valor do byte como coluna");
        const std::vector<std::uint8_t> maiores = {255U};
        chk::ChecaIgualF(m.LogLik(Vista(maiores)), -256.0, "byte 255 indexa a ultima coluna");
    }

    /* ------------------------------------------------------- ByteModels::Create */
    chk::Grupo("ByteModels::Create: caminhos de erro do artefato");
    {
        const ara::core::Result<std::unique_ptr<ids::ByteModels>> inexistente =
            ids::ByteModels::Create("/tmp/nao-existe-de-jeito-nenhum.bin");
        chk::Checa(!inexistente.HasValue(), "arquivo inexistente e recusado");
        chk::Checa(!inexistente.HasValue() &&
                       (inexistente.Error().Value() == static_cast<int>(ids::IdsSensorErrc::ArtefactNotFound)),
                   "o codigo e ArtefactNotFound");

        const std::size_t certas[3] = {ids::kSomeipPositions, ids::kSdPositions, ids::kL4Positions};
        const std::size_t erradas[3] = {ids::kSomeipPositions, 999U, ids::kL4Positions};

        const std::string pBom = "/tmp/bm_bom.bin";
        chk::Checa(EscreveArtefato(pBom, certas, 0U), "artefato bom escrito");
        const ara::core::Result<std::unique_ptr<ids::ByteModels>> bom = ids::ByteModels::Create(pBom);
        chk::Checa(bom.HasValue(), "artefato com as dimensoes certas e aceito");
        if (bom.HasValue())
        {
            chk::ChecaIgual(static_cast<long long>(bom.Value()->Someip().Positions()),
                            static_cast<long long>(ids::kSomeipPositions), "L do someip");
            chk::ChecaIgual(static_cast<long long>(bom.Value()->Sd().Positions()),
                            static_cast<long long>(ids::kSdPositions), "L do sd");
            chk::ChecaIgual(static_cast<long long>(bom.Value()->L4().Positions()),
                            static_cast<long long>(ids::kL4Positions), "L do l4");
        }

        const std::string pErrado = "/tmp/bm_dim.bin";
        static_cast<void>(EscreveArtefato(pErrado, erradas, 0U));
        const ara::core::Result<std::unique_ptr<ids::ByteModels>> dim = ids::ByteModels::Create(pErrado);
        chk::Checa(!dim.HasValue(), "contagem de posicoes diferente do contrato e recusada");
        chk::Checa(!dim.HasValue() && (dim.Error().Value() == static_cast<int>(ids::IdsSensorErrc::ArtefactCorrupted)),
                   "o codigo e ArtefactCorrupted");

        const std::string pCurto = "/tmp/bm_curto.bin";
        static_cast<void>(EscreveArtefato(pCurto, certas, 5000U));
        const ara::core::Result<std::unique_ptr<ids::ByteModels>> curto = ids::ByteModels::Create(pCurto);
        chk::Checa(!curto.HasValue(), "arquivo truncado e recusado em vez de ler lixo");

        const std::string pVazio = "/tmp/bm_vazio.bin";
        std::FILE        *fv     = std::fopen(pVazio.c_str(), "wb");
        if (fv != nullptr)
        {
            static_cast<void>(std::fclose(fv));
        }
        chk::Checa(!ids::ByteModels::Create(pVazio).HasValue(), "arquivo vazio e recusado");

        /* "Nao existe" e "existe e nao da para ler" sao problemas diferentes para
         * quem tem de consertar o deploy. O @retval do cabecalho prometia
         * ArtefactUnreadable desde o inicio, mas a implementacao so devolvia
         * ArtefactNotFound: foi este levantamento de cobertura que achou. */
        const std::string pSemPermissao = "/tmp/bm_sem_permissao.bin";
        static_cast<void>(EscreveArtefato(pSemPermissao, certas, 0U));
        if (::chmod(pSemPermissao.c_str(), 0) == 0)
        {
            const ara::core::Result<std::unique_ptr<ids::ByteModels>> negado = ids::ByteModels::Create(pSemPermissao);
            if (negado.HasValue())
            {
                /* Rodando como root, a permissao nao barra nada. */
                std::printf("  (pulado: o processo le o arquivo mesmo sem permissao; root?)\n");
            }
            else
            {
                chk::ChecaIgual(negado.Error().Value(), static_cast<int>(ids::IdsSensorErrc::ArtefactUnreadable),
                                "arquivo sem permissao da ArtefactUnreadable, nao ArtefactNotFound");
                chk::Checa(negado.Error().SupportData() != 0, "e o errno da plataforma vem no support data");
            }
            static_cast<void>(::chmod(pSemPermissao.c_str(), 0600));
        }
        else
        {
            std::printf("  (pulado: nao consegui remover a permissao do arquivo)\n");
        }
    }

    return chk::Veredito("test_bytemodel");
}
