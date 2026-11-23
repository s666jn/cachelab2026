// student id: 2025200686
// please change the above line to your student id

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

void printSummary(int hits, int misses, int evictions)
{
  printf("hits:%d misses:%d evictions:%d\n", hits, misses, evictions);
  FILE *output_fp = fopen(".csim_results", "w");
  assert(output_fp);
  fprintf(output_fp, "%d %d %d\n", hits, misses, evictions);
  fclose(output_fp);
}

void printHelp(const char *name)
{
  printf(
      "Usage: %s [-hv] -s <num> -E <num> -b <num> -t <file>\n"
      "Options:\n"
      "  -h         Print this help message.\n"
      "  -v         Optional verbose flag.\n"
      "  -s <num>   Number of set index bits.\n"
      "  -E <num>   Number of lines per set.\n"
      "  -b <num>   Number of block offset bits.\n"
      "  -t <file>  Trace file.\n\n"
      "Examples:\n"
      "  linux>  %s -s 4 -E 1 -b 4 -t traces/yi.trace\n"
      "  linux>  %s -v -s 8 -E 2 -b 4 -t traces/yi.trace\n",
      name, name, name);
}

typedef struct {
    int verbose;    // -v
    int s;          // -s
    int E;          // -E
    int b;          // -b
    char *tracefile;// -t
} Config;

typedef struct {
    int valid;
    unsigned long tag;
    unsigned long long lru;   // 越大越新；0 表示从未使用
} cache_line;

typedef struct {
    int s, E, b;
    int num_sets;             // 2^s
    cache_line *lines;        // 扁平数组，长度 num_sets * E
    unsigned long long lru_clock;
    // 统计
    long hits, misses, evictions;
    int verbose;
} cache_t;

static int init_cache(cache_t *c, int s, int E, int b) {
    c->s = s;
    c->E = E;
    c->b = b;
    c->num_sets = (int)(1UL << s);          // 用 1UL 避免 int 移位溢出
    size_t n = (size_t)c->num_sets * (size_t)E;
    c->lines = calloc(n, sizeof(cache_line)); // calloc 保证 valid=0
    if (!c->lines) {
        fprintf(stderr, "calloc failed\n");
        return -1;
    }
    c->lru_clock = 0;
    c->hits = c->misses = c->evictions = 0;
    return 0;
}

static void free_cache(cache_t *c) {
    free(c->lines);
    c->lines = NULL;
}

/* ---------- 单次访问：返回 1=hit, 0=miss ---------- */
static int access_cache(cache_t *c, unsigned long addr) {
    // 地址拆解：| tag | set index(s 位) | block offset(b 位) |
    unsigned long set = (addr >> c->b) & ((1UL << c->s) - 1);
    unsigned long tag = addr >> (c->b + c->s);
    cache_line *base = &c->lines[(size_t)set * c->E];

    // 1) 查找命中：比较 valid 和 tag
    for (int i = 0; i < c->E; i++) {
        if (base[i].valid && base[i].tag == tag) {
            base[i].lru = ++c->lru_clock;   // 刷新为最新
            c->hits++;
            return 1;
        }
    }

    // 2) miss
    c->misses++;

    // 2a) 有空行：直接放入
    for (int i = 0; i < c->E; i++) {
        if (!base[i].valid) {
            base[i].valid = 1;
            base[i].tag = tag;
            base[i].lru = ++c->lru_clock;
            return 0;
        }
    }

    // 2b) set 满了：驱逐 lru 最小的（最久未使用）
    int victim = 0;
    for (int i = 1; i < c->E; i++) {
        if (base[i].lru < base[victim].lru)
            victim = i;
    }
    base[victim].tag = tag;                 // valid 本就是 1，不必改
    base[victim].lru = ++c->lru_clock;
    c->evictions++;
    return 0;
}

/* ---------- 读 trace 文件并模拟 ---------- */
static void simulate(cache_t *c, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }

    char op;
    unsigned long addr;
    int size;
    int reg;

    // 前导空格 " %c" 用于跳过换行/空白；" %lx,%d" 解析地址和大小
    while (fscanf(fp, " %c %lx,%d %d", &op, &addr, &size, &reg) == 4) {
        if (op == 'I') continue;            // 指令访问，csim 忽略

        if (op == 'L' || op == 'S') {
            int hit = access_cache(c, addr);
            if (c->verbose)
                printf("%c %lx,%d %s\n", op, addr, size, hit ? "hit" : "miss");
        } else if (op == 'M') {
            // M = load + store：访问两次
            int hit1 = access_cache(c, addr);   // 第一次可能 miss
            if (c->verbose)
                printf("%c %lx,%d %s\n", op, addr, size, hit1 ? "hit" : "miss");
            access_cache(c, addr);              // 第二次必然 hit
        }
    }
    fclose(fp);
}

int main(int argc, char *argv[])
{   
    Config cfg = {0, -1, -1, -1, NULL};
    int opt;
    char *endptr;

    while ((opt = getopt(argc, argv, "hvs:E:b:t:")) != -1) {
        switch (opt) {
        case 'h':
            printHelp(argv[0]);
            exit(0);
        case 'v':
            cfg.verbose = 1;
            break;
        case 's':
            cfg.s = (int)strtol(optarg, &endptr, 10);
            if (endptr == optarg ||*endptr != '\0' || cfg.s < 0) {
                fprintf(stderr, "Invalid -s value: %s\n", optarg);
                exit(1);
            }
            break;
        case 'E':
            cfg.E = (int)strtol(optarg, &endptr, 10);
            if (endptr == optarg ||*endptr != '\0' || cfg.E <= 0) {
                fprintf(stderr, "Invalid -E value: %s\n", optarg);
                exit(1);
            }
            break;
        case 'b':
            cfg.b = (int)strtol(optarg, &endptr, 10);
            if (endptr == optarg ||*endptr != '\0' || cfg.b < 0) {
                fprintf(stderr, "Invalid -b value: %s\n", optarg);
                exit(1);
            }
            break;
        case 't':
            cfg.tracefile = optarg;
            break;
        case '?':   // 未知选项或缺少参数
        default:
            printHelp(argv[0]);
            exit(1);
        }
    }

    cache_t cache;
    if (init_cache(&cache, cfg.s, cfg.E, cfg.b) != 0) return 1;
    cache.verbose = cfg.verbose;
    
    simulate(&cache, cfg.tracefile);

    printSummary(cache.hits, cache.misses, cache.evictions);

    free_cache(&cache);
  
    return 0;
}
