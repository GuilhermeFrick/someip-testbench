/* Mede a latencia do avaliador compacto para UMA tabela (uma configuracao da varredura).
 *
 * Programa minimo de proposito unico: nao linka a libxgboost nem o TL2cgen,
 * entao compila em segundos e pode ser rodado uma vez por configuracao.
 *
 *   sweep_eval <tables.bin> <rows.bin> <n_feat> <reps>
 *
 * Imprime uma linha JSON com mediana, p99 e numero de nos.
 */
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

typedef struct { float thr; int32_t feat; int32_t left; int32_t right; } node_t;

#define DEPTH_GUARD 64

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

static void eval_tables(const node_t *nodes, const int32_t *roots, int n_trees,
                        int n_class, const float *base, const float *x, float *margin)
{
    memcpy(margin, base, (size_t)n_class * sizeof(float));
    for (int t = 0; t < n_trees; ++t) {
        int32_t i = roots[t];
        for (int d = 0; d < DEPTH_GUARD; ++d) {
            if (nodes[i].feat < 0) break;
            i = (x[nodes[i].feat] < nodes[i].thr) ? nodes[i].left : nodes[i].right;
        }
        margin[t % n_class] += nodes[i].thr;
    }
    /* softmax omitido: custo fixo em n_class, irrelevante para a comparacao */
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "uso: %s <tables.bin> <rows.bin> <n_feat> <reps>\n", argv[0]);
        return 2;
    }
    const char *tpath = argv[1], *rpath = argv[2];
    const int n_feat = atoi(argv[3]), reps = atoi(argv[4]);

    size_t rlen = 0;
    unsigned char *blob = slurp(tpath, NULL);
    float *X = slurp(rpath, &rlen);
    if (!blob || !X) return 1;
    const int n_rows = (int)(rlen / (sizeof(float) * (size_t)n_feat));

    const int32_t *hdr = (const int32_t *)blob;
    const int n_trees = hdr[0], n_class = hdr[1], n_nodes = hdr[2];
    const float *base = (const float *)(hdr + 4);
    const int32_t *roots = (const int32_t *)(base + n_class);
    const node_t *nodes = (const node_t *)(roots + n_trees);

    float *mg = calloc((size_t)n_class, sizeof(float));
    double *S = malloc((size_t)reps * sizeof(double));

    for (int i = 0; i < 200; ++i)            /* aquecimento */
        eval_tables(nodes, roots, n_trees, n_class, base, X + (size_t)(i % n_rows) * n_feat, mg);

    for (int i = 0; i < reps; ++i) {
        const float *row = X + (size_t)(i % n_rows) * n_feat;
        double a = now_s();
        eval_tables(nodes, roots, n_trees, n_class, base, row, mg);
        S[i] = (now_s() - a) * 1e6;
    }
    qsort(S, (size_t)reps, sizeof(double), cmp_d);

    printf("{\"p50\": %.4f, \"p99\": %.4f, \"n_nodes\": %d, \"n_trees\": %d}\n",
           S[(int)(0.50 * (reps - 1))], S[(int)(0.99 * (reps - 1))], n_nodes, n_trees);

    free(mg); free(S); free(X); free(blob);
    return 0;
}
