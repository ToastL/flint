/* a local header for cimport.fl: types, enums and macros, used without linking any C */
#define LOCAL_LIMIT 64
#define LOCAL_SCALE (2.5)
#define LOCAL_NAME "local"
#define LOCAL_MASK (1u << 4 | LOCAL_LIMIT)
#define LOCAL_TWICE(x) ((x) * 2)

enum color { RED, GREEN = 5, BLUE };

typedef struct point {
    int x, y;
    unsigned char tag[4];
    struct point *next;
} point_t;

typedef int (*binop_fn)(int, int);

struct ops {
    binop_fn apply;
    const char *label;
};

union num { long i; double f; };
