#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
int main(void) {
    int64_t n = 50000000, count = 0;
    bool* c = calloc(n + 1, 1);
    for (int64_t i = 2; i <= n; i++) if (!c[i]) { count++; for (int64_t j = i * i; j <= n; j += i) c[j] = true; }
    printf("%lld\n", (long long)count); free(c);
}
