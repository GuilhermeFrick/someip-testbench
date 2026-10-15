/* Head-to-head: libxgboost (C API) x TL2cgen (C99 gerado), 1 linha por chamada.
 *
 * Mede os dois no MESMO laco, com o MESMO relogio e as MESMAS linhas de entrada,
 * e confere a paridade numerica contra o oraculo Python (ref_probs.bin).
 *
 * Compile com build.sh (ele localiza a libxgboost e o C gerado pelo TL2cgen).
 */
#define _POSIX_C_SOURCE 199309L   /* clock_gettime / CLOCK_MONOTONIC sob -std=c99 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

/* ---------- C API da libxgboost (declarada aqui p/ nao depender dos headers) ---------- */
#ifdef USE_XGB_HEADER
#include <xgboost/c_api.h>
#else
typedef void *BoosterHandle;
typedef void *DMatrixHandle;
typedef uint64_t bst_ulong;
extern int XGBoosterCreate(const void *dmats[], bst_ulong len, BoosterHandle *out);
extern int XGBoosterLoadModel(BoosterHandle handle, const char *fname);
extern int XGBoosterSetParam(BoosterHandle handle, const char *name, const char *value);
extern int XGBoosterPredictFromDense(BoosterHandle handle, char const *values,
                                     char const *config, DMatrixHandle m,
                                     bst_ulong const **out_shape, bst_ulong *out_dim,
                                     float const **out_result);
extern const char *XGBGetLastError(void);
#endif

/* ---------- predictor gerado pelo TL2cgen (opcional) ---------- */
#ifdef HAVE_TL2CGEN
#include "header.h"   /* traz union Entry, predict_multiclass, get_num_class, ... */
#endif

#define XGB_OK(call)                                                        \
    do {                                                                    \
        if ((call) != 0) {                                                  \
            fprintf(stderr, "xgboost: %s\n", XGBGetLastError());            \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* le "key": <int> de um json simples, sem dependencia externa */
static int json_int(const char *buf, const char *key, int fallback)
{
    const char *p = strstr(buf, key);
    if (!p) return fallback;
    p = strchr(p, ':');
    return p ? atoi(p + 1) : fallback;
}

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    ((char *)buf)[n] = '\0';
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

/* maior desvio absoluto entre duas linhas de probabilidades */
static double max_abs_diff(const float *a, const float *b, int n)
{
    double m = 0.0;
    for (int i = 0; i < n; ++i) {
        double d = fabs((double)a[i] - (double)b[i]);
        if (d > m) m = d;
    }
    return m;
}

static int argmax(const float *v, int n)
{
    int best = 0;
    for (int i = 1; i < n; ++i) if (v[i] > v[best]) best = i;
    return best;
}

/* ---------- estatisticas: a media esconde a cauda, entao gravamos cada chamada ---------- */
typedef struct { double mean, p50, p95, p99, min, max; } stats_t;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ordena `s` no lugar; a media e calculada antes de ordenar */
static stats_t compute_stats(double *s, int n)
{
    stats_t st;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += s[i];
    st.mean = sum / n;
    qsort(s, (size_t)n, sizeof(double), cmp_d);
    st.min = s[0];
    st.max = s[n - 1];
    st.p50 = s[(int)(0.50 * (n - 1))];
    st.p95 = s[(int)(0.95 * (n - 1))];
    st.p99 = s[(int)(0.99 * (n - 1))];
    return st;
}

static void print_stats(stats_t st)
{
    printf("  latencia (us)   : media %8.2f | p50 %8.2f | p95 %8.2f | p99 %8.2f | max %8.2f\n",
           st.mean, st.p50, st.p95, st.p99, st.max);
}

static void json_stats(FILE *f, const char *key, stats_t st, int last)
{
    fprintf(f, "    \"%s\": {\"mean\": %.4f, \"p50\": %.4f, \"p95\": %.4f, "
               "\"p99\": %.4f, \"min\": %.4f, \"max\": %.4f}%s\n",
            key, st.mean, st.p50, st.p95, st.p99, st.min, st.max, last ? "" : ",");
}

/* ---------- avaliador compacto: o modelo como DADO, nao como codigo ----------
 * 16 bytes por no. Laco limitado, sem recursao e sem alocacao no caminho quente. */
typedef struct { float thr; int32_t feat; int32_t left; int32_t right; } node_t;

#define DEPTH_GUARD 64   /* teto do laco: torna o WCET analisavel */

static void softmax(float *v, int n)
{
    float mx = v[0];
    for (int i = 1; i < n; ++i) if (v[i] > mx) mx = v[i];
    double sum = 0.0;
    for (int i = 0; i < n; ++i) { v[i] = expf(v[i] - mx); sum += v[i]; }
    for (int i = 0; i < n; ++i) v[i] /= (float)sum;
}

static void eval_tables(const node_t *nodes, const int32_t *roots, int n_trees,
                        int n_class, const float *base, const float *x, float *margin)
{
    /* base_score e um vetor por classe neste modelo, entao e a margem inicial */
    memcpy(margin, base, (size_t)n_class * sizeof(float));
    for (int t = 0; t < n_trees; ++t) {
        int32_t i = roots[t];
        for (int d = 0; d < DEPTH_GUARD; ++d) {
            if (nodes[i].feat < 0) break;                      /* folha */
            i = (x[nodes[i].feat] < nodes[i].thr) ? nodes[i].left : nodes[i].right;
        }
        margin[t % n_class] += nodes[i].thr;                   /* folha guarda o valor */
    }
    softmax(margin, n_class);
}

int main(int argc, char **argv)
{
    const char *dir   = (argc > 1) ? argv[1] : "build";
    const char *model = (argc > 2) ? argv[2] : "model_16.json";
    const int   reps  = (argc > 3) ? atoi(argv[3]) : 2000;

    char path[1024];
    snprintf(path, sizeof path, "%s/meta.json", dir);
    char *meta = slurp(path, NULL);
    if (!meta) return 1;
    const int n_rows  = json_int(meta, "n_rows", 0);
    const int n_feat  = json_int(meta, "n_feat", 16);
    const int n_class = json_int(meta, "n_class", 5);
    free(meta);
    printf("linhas=%d features=%d classes=%d repeticoes=%d\n\n", n_rows, n_feat, n_class, reps);

    snprintf(path, sizeof path, "%s/rows.bin", dir);
    float *X = slurp(path, NULL);
    snprintf(path, sizeof path, "%s/ref_probs.bin", dir);
    float *REF = slurp(path, NULL);
    if (!X || !REF) return 1;

    /* ================= 1. libxgboost, caminho inplace ================= */
    BoosterHandle bst = NULL;
    XGB_OK(XGBoosterCreate(NULL, 0, &bst));
    XGB_OK(XGBoosterLoadModel(bst, model));
    XGB_OK(XGBoosterSetParam(bst, "nthread", "1"));

    /* "missing" e obrigatorio; NaN e o mesmo default do inplace_predict do Python */
    static const char *CFG =
        "{\"type\":0,\"training\":false,\"iteration_begin\":0,"
        "\"iteration_end\":0,\"strict_shape\":false,\"missing\":NaN}";

    char ai[256];
    bst_ulong const *shape = NULL;
    bst_ulong dim = 0;
    float const *out = NULL;

    /* paridade: compara as n_rows linhas contra o oraculo */
    double worst_xgb = 0.0;
    int mism_xgb = 0;
    for (int r = 0; r < n_rows; ++r) {
        float *row = X + (size_t)r * n_feat;
        snprintf(ai, sizeof ai,
                 "{\"data\":[%llu,false],\"shape\":[1,%d],\"typestr\":\"<f4\",\"version\":3}",
                 (unsigned long long)(uintptr_t)row, n_feat);
        XGB_OK(XGBoosterPredictFromDense(bst, ai, CFG, NULL, &shape, &dim, &out));
        double d = max_abs_diff(out, REF + (size_t)r * n_class, n_class);
        if (d > worst_xgb) worst_xgb = d;
        if (argmax(out, n_class) != argmax(REF + (size_t)r * n_class, n_class)) ++mism_xgb;
    }

    /* latencia: 1 linha por chamada, gravando cada amostra */
    double *S = malloc((size_t)reps * sizeof(double));
    for (int i = 0; i < reps; ++i) {
        float *row = X + (size_t)(i % n_rows) * n_feat;
        snprintf(ai, sizeof ai,
                 "{\"data\":[%llu,false],\"shape\":[1,%d],\"typestr\":\"<f4\",\"version\":3}",
                 (unsigned long long)(uintptr_t)row, n_feat);
        double a = now_s();
        XGBoosterPredictFromDense(bst, ai, CFG, NULL, &shape, &dim, &out);
        S[i] = (now_s() - a) * 1e6;
    }
    stats_t st_xgb = compute_stats(S, reps);
    double us_xgb = st_xgb.mean;

    printf("=== libxgboost (C API, XGBoosterPredictFromDense, nthread=1) ===\n");
    print_stats(st_xgb);
    printf("  paridade        : desvio max %.3e, classes divergentes %d/%d\n\n",
           worst_xgb, mism_xgb, n_rows);

    /* resultados dos motores opcionais, para o JSON no final */
    stats_t st_tl = {0}, st_tb = {0};
    int has_tl = 0, has_tb = 0, mis_tl = 0, mis_tb = 0;
    double par_tl = 0.0, par_tb = 0.0;
    int tb_nodes = 0;

    /* ================= 2. TL2cgen, C99 gerado ================= */
#ifdef HAVE_TL2CGEN
    {
        union Entry *inst = calloc((size_t)n_feat, sizeof(union Entry));
        float *res = calloc((size_t)n_class, sizeof(float));

        double worst_tl = 0.0;
        int mism_tl = 0;
        for (int r = 0; r < n_rows; ++r) {
            float *row = X + (size_t)r * n_feat;
            for (int j = 0; j < n_feat; ++j) inst[j].fvalue = row[j];
            /* predict acumula com +=, entao o buffer PRECISA ser zerado antes.
             * pred_margin=0 faz o proprio predict aplicar o softmax (postprocess). */
            memset(res, 0, (size_t)n_class * sizeof(float));
            predict(inst, 0, res);
            double d = max_abs_diff(res, REF + (size_t)r * n_class, n_class);
            if (d > worst_tl) worst_tl = d;
            if (argmax(res, n_class) != argmax(REF + (size_t)r * n_class, n_class)) ++mism_tl;
        }

        for (int i = 0; i < reps; ++i) {
            float *row = X + (size_t)(i % n_rows) * n_feat;
            double a = now_s();
            for (int j = 0; j < n_feat; ++j) inst[j].fvalue = row[j];
            memset(res, 0, (size_t)n_class * sizeof(float));
            predict(inst, 0, res);
            S[i] = (now_s() - a) * 1e6;
        }
        st_tl = compute_stats(S, reps);
        has_tl = 1;

        printf("=== TL2cgen (C99 gerado) ===\n");
        print_stats(st_tl);
        printf("  paridade        : desvio max %.3e, classes divergentes %d/%d\n\n",
               worst_tl, mism_tl, n_rows);
        par_tl = worst_tl; mis_tl = mism_tl;
        free(inst); free(res);
    }
#else
    printf("=== TL2cgen ===\n  nao compilado (rode prepare.py e recompile com build.sh)\n\n");
#endif

    /* ================= 3. avaliador compacto (tabelas const) ================= */
    snprintf(path, sizeof path, "%s/tables.bin", dir);
    {
        size_t tlen = 0;
        unsigned char *blob = slurp(path, &tlen);
        if (!blob) {
            printf("=== avaliador compacto ===\n  tables.bin ausente"
                   " (rode export_tables.py)\n\n");
        } else {
            const int32_t *hdr = (const int32_t *)blob;
            int n_trees = hdr[0], nc = hdr[1], n_nodes = hdr[2];
            const float *base = (const float *)(hdr + 4);
            const int32_t *roots = (const int32_t *)(base + nc);
            const node_t *nodes = (const node_t *)(roots + n_trees);
            printf("=== avaliador compacto (tabelas const, 16 B/no) ===\n");
            printf("  arvores=%d nos=%d tabela=%.2f MB\n",
                   n_trees, n_nodes, (double)n_nodes * sizeof(node_t) / 1e6);

            float *mg = calloc((size_t)nc, sizeof(float));

            double worst = 0.0;
            int mism = 0;
            for (int r = 0; r < n_rows; ++r) {
                eval_tables(nodes, roots, n_trees, nc, base, X + (size_t)r * n_feat, mg);
                double d = max_abs_diff(mg, REF + (size_t)r * n_class, n_class);
                if (d > worst) worst = d;
                if (argmax(mg, nc) != argmax(REF + (size_t)r * n_class, n_class)) ++mism;
            }

            for (int i = 0; i < reps; ++i) {
                double a = now_s();
                eval_tables(nodes, roots, n_trees, nc, base,
                            X + (size_t)(i % n_rows) * n_feat, mg);
                S[i] = (now_s() - a) * 1e6;
            }
            st_tb = compute_stats(S, reps);
            has_tb = 1; par_tb = worst; mis_tb = mism; tb_nodes = n_nodes;

            print_stats(st_tb);
            printf("  paridade        : desvio max %.3e, classes divergentes %d/%d\n\n",
                   worst, mism, n_rows);
            free(mg); free(blob);
        }
    }

    /* ================= resumo e JSON para os graficos ================= */
    printf("=== RESUMO (speedup sobre a libxgboost, pela mediana) ===\n");
    printf("  libxgboost          : %8.2f us  (1,0x)\n", st_xgb.p50);
    if (has_tl)
        printf("  TL2cgen             : %8.2f us  (%.1fx)\n", st_tl.p50, st_xgb.p50 / st_tl.p50);
    if (has_tb)
        printf("  avaliador compacto  : %8.2f us  (%.1fx)\n", st_tb.p50, st_xgb.p50 / st_tb.p50);
    printf("\n");

    snprintf(path, sizeof path, "%s/results.json", dir);
    FILE *jf = fopen(path, "w");
    if (jf) {
        fprintf(jf, "{\n  \"setup\": {\"n_rows\": %d, \"n_feat\": %d, \"n_class\": %d, "
                    "\"reps\": %d, \"n_nodes\": %d},\n",
                n_rows, n_feat, n_class, reps, tb_nodes);
        fprintf(jf, "  \"latency_us\": {\n");
        json_stats(jf, "libxgboost_c_api", st_xgb, !has_tl && !has_tb);
        if (has_tl) json_stats(jf, "tl2cgen", st_tl, !has_tb);
        if (has_tb) json_stats(jf, "avaliador_compacto", st_tb, 1);
        fprintf(jf, "  },\n");
        fprintf(jf, "  \"paridade\": {\n");
        fprintf(jf, "    \"libxgboost_c_api\": {\"max_abs_diff\": %.6e, \"divergentes\": %d},\n",
                worst_xgb, mism_xgb);
        if (has_tl)
            fprintf(jf, "    \"tl2cgen\": {\"max_abs_diff\": %.6e, \"divergentes\": %d},\n",
                    par_tl, mis_tl);
        fprintf(jf, "    \"avaliador_compacto\": {\"max_abs_diff\": %.6e, \"divergentes\": %d}\n",
                par_tb, mis_tb);
        fprintf(jf, "  }\n}\n");
        fclose(jf);
        printf("[ok] metricas em %s\n", path);
    }

    free(S); free(X); free(REF);
    return 0;
}
