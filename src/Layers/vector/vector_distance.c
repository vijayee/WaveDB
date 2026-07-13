#include "vector_internal.h"
#include <math.h>

float vl_distance(const float *a, const float *b, int dim, vl_distance_t metric) {
    switch (metric) {
        case VL_DIST_L2: {
            float sum = 0.0f;
            for (int i = 0; i < dim; i++) {
                float d = a[i] - b[i];
                sum += d * d;
            }
            return sqrtf(sum);
        }
        case VL_DIST_COSINE: {
            float dot = 0.0f, na = 0.0f, nb = 0.0f;
            for (int i = 0; i < dim; i++) {
                dot += a[i] * b[i];
                na  += a[i] * a[i];
                nb  += b[i] * b[i];
            }
            if (na == 0.0f || nb == 0.0f) return 1.0f;
            return 1.0f - dot / (sqrtf(na) * sqrtf(nb));
        }
        case VL_DIST_DOT: {
            float dot = 0.0f;
            for (int i = 0; i < dim; i++) dot += a[i] * b[i];
            return -dot;
        }
        default:
            return 0.0f;
    }
}