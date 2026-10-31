/**
 * @file      check.hpp
 * @author    Guilherme Frick (frickoliveira.ee@gmail.com)
 * @brief     Asserções mínimas compartilhadas pelos testes unitários.
 * @version   1.0
 * @date      2026-10-31
 * @copyright Copyright (c) 2026
 *
 * @details   A §10 do padrão de código pede GoogleTest, e a migração está anotada como pendência
 *            em tests/README.md. Até lá isto evita arrastar uma dependência que precisa ser
 *            buscada da rede dentro do container, sem deixar os testes sem estrutura: cada caso
 *            tem nome, o resultado é contado, e o processo sai diferente de zero se algo falhar.
 */
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace chk
{

/// Quantas asserções falharam no processo.
inline int &Falhas()
{
    static int falhas = 0;
    return falhas;
}

/**
 * @brief Abre um grupo de casos, só para a saída ficar legível.
 * @param[in] titulo Nome do grupo.
 * @return None.
 */
inline void Grupo(const char *titulo)
{
    std::printf("\n== %s ==\n", titulo);
}

/**
 * @brief Verifica uma condição.
 * @param[in] condicao O que deveria ser verdadeiro.
 * @param[in] oQue     Descrição do caso, impressa em qualquer resultado.
 * @return None.
 */
inline void Checa(bool condicao, const char *oQue)
{
    if (condicao)
    {
        std::printf("  ok   %s\n", oQue);
    }
    else
    {
        std::printf("  FALHA %s\n", oQue);
        ++Falhas();
    }
}

/**
 * @brief Verifica igualdade de inteiros, imprimindo os dois valores quando diferem.
 * @param[in] obtido   Valor produzido.
 * @param[in] esperado Valor esperado.
 * @param[in] oQue     Descrição do caso.
 * @return None.
 */
inline void ChecaIgual(long long obtido, long long esperado, const char *oQue)
{
    if (obtido == esperado)
    {
        std::printf("  ok   %s\n", oQue);
    }
    else
    {
        std::printf("  FALHA %s  (obtido=%lld esperado=%lld)\n", oQue, obtido, esperado);
        ++Falhas();
    }
}

/**
 * @brief Verifica igualdade de ponto flutuante EXATA.
 * @param[in] obtido   Valor produzido.
 * @param[in] esperado Valor esperado.
 * @param[in] oQue     Descrição do caso.
 * @return None.
 * @details Exata de propósito. Onde o valor é construído por divisão inteira ou é um zero
 *          definido, uma tolerância esconderia o erro que o caso existe para pegar.
 */
inline void ChecaIgualF(double obtido, double esperado, const char *oQue)
{
    if (obtido == esperado)
    {
        std::printf("  ok   %s\n", oQue);
    }
    else
    {
        std::printf("  FALHA %s  (obtido=%.17g esperado=%.17g)\n", oQue, obtido, esperado);
        ++Falhas();
    }
}

/**
 * @brief Verifica igualdade de ponto flutuante com tolerância relativa.
 * @param[in] obtido   Valor produzido.
 * @param[in] esperado Valor esperado.
 * @param[in] tol      Tolerância relativa aceita.
 * @param[in] oQue     Descrição do caso.
 * @return None.
 */
inline void ChecaPerto(double obtido, double esperado, double tol, const char *oQue)
{
    const double escala = (std::fabs(esperado) > 1.0) ? std::fabs(esperado) : 1.0;
    if (std::fabs(obtido - esperado) <= (tol * escala))
    {
        std::printf("  ok   %s\n", oQue);
    }
    else
    {
        std::printf("  FALHA %s  (obtido=%.17g esperado=%.17g)\n", oQue, obtido, esperado);
        ++Falhas();
    }
}

/**
 * @brief Fecha o processo com o veredito.
 * @param[in] nome Nome do conjunto de testes.
 * @return 0 se tudo passou, 1 caso contrário.
 */
inline int Veredito(const char *nome)
{
    if (Falhas() == 0)
    {
        std::printf("\n%s: TUDO PASSOU\n", nome);
        return 0;
    }
    std::printf("\n%s: %d FALHAS\n", nome, Falhas());
    return 1;
}

} // namespace chk
