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

    /* latencia: 1 linha por chamada */
    double t0 = now_s();
    for (int i = 0; i < reps; ++i) {
        float *row = X + (size_t)(i % n_rows) * n_feat;
        snprintf(ai, sizeof ai,
                 "{\"data\":[%llu,false],\"shape\":[1,%d],\"typestr\":\"<f4\",\"version\":3}",
                 (unsigned long long)(uintptr_t)row, n_feat);
        XGBoosterPredictFromDense(bst, ai, CFG, NULL, &shape, &dim, &out);
    }
    double us_xgb = (now_s() - t0) / reps * 1e6;

    printf("=== libxgboost (C API, XGBoosterPredictFromDense, nthread=1) ===\n");
    printf("  latencia        : %9.2f us/pred\n", us_xgb);
    printf("  paridade        : desvio max %.3e, classes divergentes %d/%d\n\n",
           worst_xgb, mism_xgb, n_rows);

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

        t0 = now_s();
        for (int i = 0; i < reps; ++i) {
            float *row = X + (size_t)(i % n_rows) * n_feat;
            for (int j = 0; j < n_feat; ++j) inst[j].fvalue = row[j];
            memset(res, 0, (size_t)n_class * sizeof(float));
            predict(inst, 0, res);
        }
        double us_tl = (now_s() - t0) / reps * 1e6;

        printf("=== TL2cgen (C99 gerado) ===\n");
        printf("  latencia        : %9.2f us/pred\n", us_tl);
        printf("  paridade        : desvio max %.3e, classes divergentes %d/%d\n\n",
               worst_tl, mism_tl, n_rows);

        printf("=== VEREDITO ===\n");
        printf("  TL2cgen e %.1fx %s que a libxgboost\n",
               us_tl > 0 ? (us_xgb / us_tl) : 0.0, us_xgb > us_tl ? "MAIS RAPIDO" : "mais lento");
        free(inst); free(res);
    }
#else
    printf("=== TL2cgen ===\n  nao compilado (rode prepare.py e recompile com build.sh)\n\n");
#endif

    free(X); free(REF);
    return 0;
}
