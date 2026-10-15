// Motor de inferencia. Esta e a interface que permite trocar de motor sem
// tocar no resto do sensor, e comparar dois motores no MESMO harness, o que
// mantem a comparacao honesta (harness constante, variavel isolada).
#ifndef IDS_SENSOR_CLASSIFIER_HPP
#define IDS_SENSOR_CLASSIFIER_HPP

#include <memory>
#include <string>

#include "types.hpp"

namespace ids {

class IClassifier {
public:
    virtual ~IClassifier() = default;

    /// Classifica UM vetor de features ja normalizado.
    /// Nao aloca. `out` recebe as probabilidades por classe.
    virtual void Predict(const FeatureVec &x, ProbVec &out) const = 0;

    /// Nome do motor, para aparecer nas metricas.
    virtual const char *Name() const = 0;
};

/// Devolve a classe de maior probabilidade.
Klass ArgMax(const ProbVec &p);

/// Motor de producao: C99 gerado pelo TL2cgen, compilado junto no build do
/// modelo selecionado. Sem dependencia de runtime.
class Tl2cgenClassifier final : public IClassifier {
public:
    Tl2cgenClassifier();
    void Predict(const FeatureVec &x, ProbVec &out) const override;
    const char *Name() const override { return "tl2cgen"; }
};

/// Melhoria reservada: modelo como tabelas percorridas por laco limitado.
/// Medido 2,2x mais rapido que o TL2cgen, com paridade exata
/// (ver experiments/bench-inference/RESULTADOS.md).
///
/// PENDENTE antes de promover: tratar a direcao `missing` do XGBoost. Hoje o
/// prototipo sempre compara, o que so nao divergiu porque os dados nao tem NaN.
class CompactClassifier final : public IClassifier {
public:
    /// Carrega a tabela de nos. O modelo e DADO: trocar de modelo nao recompila.
    explicit CompactClassifier(const std::string &tables_path);
    ~CompactClassifier() override;

    void Predict(const FeatureVec &x, ProbVec &out) const override;
    const char *Name() const override { return "compacto"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ids

#endif  // IDS_SENSOR_CLASSIFIER_HPP
