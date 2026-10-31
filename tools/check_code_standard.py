#!/usr/bin/env python3
"""Confere os cabecalhos do ids_sensor contra o checklist da secao 12 do padrao.

So checa o que e mecanicamente verificavel. O que depende de julgamento
(Builder, outcome<T>, camadas interface/arch) fica para o relatorio.
"""
import glob
import io
import os
import re
import sys

D = sys.argv[1]
falhas = []
TAGS = ["@file", "@author", "@brief", "@version", "@date", "@copyright"]


def erro(arq, regra, detalhe=""):
    falhas.append((os.path.basename(arq), regra, detalhe))


for arq in sorted(glob.glob(os.path.join(D, "*.hpp"))):
    s = io.open(arq, encoding="utf-8").read()
    linhas = s.split("\n")
    base = os.path.basename(arq)

    # ---- secao 3: guardas e namespace
    if "#pragma once" not in s:
        erro(arq, "3 #pragma once ausente")
    if re.search(r"#ifndef\s+\w+_HPP", s):
        erro(arq, "3 ainda usa guarda #ifndef")
    if "using namespace" in s:
        erro(arq, "3 using namespace proibido")
    # So conta fora de comentario: o types.hpp cita [[nodiscard]] justamente
    # para documentar por que NAO o usa.
    for i, ln in enumerate(s.split(chr(10)), 1):
        corpo = ln.strip()
        if corpo.startswith("*") or corpo.startswith("//") or corpo.startswith("/*"):
            continue
        if "[[nodiscard]]" in ln:
            erro(arq, "AUTOSAR [[nodiscard]] e C++17; o alvo e C++14", "linha %d" % i)
        if re.match(r"^\s*#\s*define\s+\w+", ln):
            erro(arq, "AUTOSAR A16-0-1 proibe #define (so include guard e #include)", "linha %d" % i)
    abertos = re.findall(r"^namespace (\w+)$", s, re.M)
    if not abertos:
        erro(arq, "3 nenhum namespace em Allman (nome em linha propria)")
    for ns in abertos:
        if ("} // namespace %s" % ns) not in s:
            erro(arq, "3 fecho de namespace sem o comentario exigido", ns)
    if re.search(r"^namespace \w+[ 	]*\{", s, re.M):
        erro(arq, "3 namespace com chave na mesma linha")

    # ---- secao 4: cabecalho Doxygen
    for t in TAGS:
        if t not in s:
            erro(arq, "4 falta a tag %s" % t)
    m = re.search(r"@file\s+(\S+)", s)
    if m and m.group(1) != base:
        erro(arq, "4 @file nao casa com o nome do arquivo", m.group(1))
    # grupo Doxygen
    if "@addtogroup" not in s and "@defgroup" not in s:
        erro(arq, "4 sem grupo Doxygen")

    # ---- secao 1: formatacao
    if "\t" in s:
        erro(arq, "1 TAB encontrado")
    for i, ln in enumerate(linhas, 1):
        if len(ln) > 150:
            erro(arq, "1 linha acima de 150 colunas", "linha %d (%d)" % (i, len(ln)))
        # Allman: chave de abertura nao fica no fim da linha de declaracao
        if re.search(r"^\s*(class|struct|enum class|if|for|while|namespace)\b.*\{\s*$", ln):
            erro(arq, "1 chave de abertura no fim da linha (nao Allman)", "linha %d" % i)
        # PointerAlignment: Right -> nunca "tipo* nome"
        if re.search(r"\w\*\s+\w", ln) and "//" not in ln.split("*")[0]:
            erro(arq, "1 ponteiro alinhado a esquerda", "linha %d" % i)

    # ---- secao 7: especificadores de acesso com 2 espacos
    for i, ln in enumerate(linhas, 1):
        if re.match(r"^\s*(public|protected|private):\s*$", ln):
            ind = len(ln) - len(ln.lstrip())
            if ind != 2:
                erro(arq, "7 especificador de acesso com indentacao != 2", "linha %d (%d)" % (i, ind))

    # ---- secao 6: tipos e casts
    for i, ln in enumerate(linhas, 1):
        if re.search(r"\((int|long|unsigned|short|char)\s*\*?\)\s*\w", ln):
            erro(arq, "6 cast estilo C", "linha %d" % i)
        if re.search(r"^\s*(int|long|unsigned|short)\s+\w+\s*[;=]", ln):
            erro(arq, "6 tipo inteiro nao fixo", "linha %d" % i)

    # ---- secao 5: membros com m_, nunca sufixo _
    for i, ln in enumerate(linhas, 1):
        if re.search(r"^\s{4}[\w:<>,\s&*]+\s\w+_\s*[;{=]", ln) and "m_" not in ln:
            erro(arq, "5 membro com sufixo _ (padrao e m_)", "linha %d" % i)

    # ---- secao 4: todo membro/campo/enumerador tem ///<
    dentro = False
    for i, ln in enumerate(linhas, 1):
        if re.match(r"^\s*(struct|enum class)\s+\w+", ln):
            dentro = True
            continue
        if dentro and re.match(r"^\};", ln):
            dentro = False
            continue
        if dentro and re.match(r"^\s{4}[A-Za-z_][\w:<>,\s&*]*\s+\w+\s*(=|\{|;|:)", ln):
            if "///<" not in ln and "(" not in ln:
                erro(arq, "4 campo sem ///<", "linha %d: %s" % (i, ln.strip()[:60]))

print("Arquivos conferidos: %d" % len(glob.glob(os.path.join(D, "*.hpp"))))
print()
if not falhas:
    print("checklist mecanico da secao 12: SEM FALHAS")
else:
    print("FALHAS (%d):" % len(falhas))
    for a, r, d in falhas:
        print("  %-22s %-50s %s" % (a, r, d))
