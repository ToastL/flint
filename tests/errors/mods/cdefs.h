/* a helper header for the C import error tests */
struct opaque;
struct flags { unsigned a : 1; unsigned b : 3; };
struct opaque *make_opaque(void);
#define TWICE(x) ((x) * 2)
#define ADDR (&something)
typedef struct { int q, r; } pair_t;
pair_t make_pair(int a);
