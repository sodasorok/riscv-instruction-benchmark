/*
 * rvv_bench.c — генератор RVV-микробенчмарков.
 *
 * Сборка (на плате OrangePi RV2):
 *   gcc -O2 -o rvv_bench rvv_bench.c
 *
 * Использование:
 *   ./rvv_bench instructions.txt [options]
 *
 * Опции:
 *   --iterations N        число итераций (по умолч. 10000000)
 *   --output FILE         куда сохранить результаты (по умолч. stdout)
 *   --cc PATH             компилятор (по умолч. gcc)
 *   --cflags "FLAGS"      флаги (по умолч. -O2 -march=rv64gcv -mabi=lp64d -lm)
 *   --generated-c FILE    куда сохранить сгенерированный C (по умолч. bench_generated.c)
 *   --only-generate       только сгенерировать C, не компилировать
 *
 * Формат строки конфига (8 полей через '|'):
 *   NAME | EXPR_LAT | EXPR_THR | SETUP | VL | LMUL | ESIZE | VTYPE
 *
 *   VTYPE — необязательный явный тип вектора аккумулятора (например vuint32m1_t).
 *           Если пусто — определяется автоматически по ESIZE/LMUL и наличию 'vf' в EXPR.
 *           Соответствующий инициализирующий интринсик тоже берётся из VTYPE:
 *             vfloatXmY_t -> __riscv_vfmv_v_f_fXmY
 *             vuintXmY_t  -> __riscv_vmv_v_x_uXmY
 *             vintXmY_t   -> __riscv_vmv_v_x_iXmY
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_ENTRIES 256
#define MAX_FIELD_LEN 1024
#define MAX_LINE_LEN 2048
#define MAX_PATH_LEN 512
#define MAX_CMD_LEN 4096
#define MAX_SAFE_NAME 128

typedef struct
{
    char name[MAX_FIELD_LEN];
    char expr_lat[MAX_FIELD_LEN];
    char expr_thr[MAX_FIELD_LEN];
    char setup[MAX_FIELD_LEN];
    int vl;
    char lmul[8];
    int esize;
    char vtype_override[MAX_FIELD_LEN]; /* поле 8, может быть пустым */
    char safe_name[MAX_SAFE_NAME];
} BenchEntry;

static void trim(char *s)
{
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start]))
        start++;
    if (start)
        memmove(s, s + start, strlen(s) - start + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

static void make_safe_name(const char *name, char *out, size_t out_size)
{
    size_t i = 0;
    for (; name[i] && i < out_size - 1; i++)
        out[i] = isalnum((unsigned char)name[i]) ? name[i] : '_';
    out[i] = '\0';
}

/* Разбивает строку по '|', возвращает число полей. */
static int split_pipe(char *line, char **fields, int max_fields)
{
    int count = 0;
    char *p = line;
    while (count < max_fields)
    {
        fields[count++] = p;
        p = strchr(p, '|');
        if (!p)
            break;
        *p = '\0';
        p++;
    }
    return count;
}

/* Подставляет needle -> replacement в строке expr, результат в out. */
int str_replace_all(const char *src, const char *old_sub, const char *new_sub, char *dest, size_t dest_size) {
    size_t old_len = strlen(old_sub);
    size_t new_len = strlen(new_sub);
    
    size_t current_len = 0;
    int replace_count = 0;
    const char *cursor = src;
    const char *match;

    // Цикл пока находим вхождения old_sub в src
    while ((match = strstr(cursor, old_sub)) != NULL) {
        // Считаем длину участка ДО совпадения
        size_t len_before = match - cursor;
        // Копируем текст ДО совпадения
        memcpy(dest + current_len, cursor, len_before);
        current_len += len_before;

        // Копируем новую подстроку (замену)
        memcpy(dest + current_len, new_sub, new_len);
        current_len += new_len;

        // Сдвигаем курсор чтения за пределы найденного старого слова
        cursor = match + old_len;
        replace_count++;
    }

    // Копируем оставшийся хвост строки
    size_t len_left = strlen(cursor);

    memcpy(dest + current_len, cursor, len_left);
    current_len += len_left;

    // Обязательно добавляем терминирующий ноль в самый конец
    dest[current_len] = '\0';

    return replace_count;
}

static void write_setup(FILE *f, const char *setup, const char *indent)
{
    if (!setup || !*setup)
        return;
    char buf[MAX_FIELD_LEN];
    strncpy(buf, setup, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, ";");
    while (tok)
    {
        trim(tok);
        if (*tok)
            fprintf(f, "%s%s;\n", indent, tok);
        tok = strtok(NULL, ";");
    }
}

/* -------------------------------------------------------------------------
 * Определение типов вектора и инициализатора
 *
 * Если задан vtype_override — используем его напрямую.
 * Иначе — выводим из esize/lmul и наличия "vf"/"_f32"/"_f64" в expr_lat.
 * ------------------------------------------------------------------------- */
static int entry_is_float(const BenchEntry *e)
{
    if (*e->vtype_override)
        return strncmp(e->vtype_override, "vfloat", 6) == 0;
    return strstr(e->expr_lat, "vfloat") != NULL ||
           strstr(e->expr_lat, "_f32") != NULL ||
           strstr(e->expr_lat, "_f64") != NULL ||
           strstr(e->expr_lat, "vf") != NULL;
}

static int entry_is_uint(const BenchEntry *e)
{
    if (*e->vtype_override)
        return strncmp(e->vtype_override, "vuint", 5) == 0;
    return 0; /* без override unsigned не определяем */
}

/* Возвращает C-тип вектора аккумулятора. */
static const char *c_vector_type(const BenchEntry *e)
{
    static char buf[64];
    if (*e->vtype_override)
        return e->vtype_override;
    if (entry_is_float(e))
        snprintf(buf, sizeof(buf), "vfloat%d%s_t", e->esize, e->lmul);
    else
        snprintf(buf, sizeof(buf), "vint%d%s_t", e->esize, e->lmul);
    return buf;
}

/* Возвращает интринсик для заполнения вектора скаляром. */
static const char *c_vfmv(const BenchEntry *e)
{
    static char buf[64];
    if (*e->vtype_override)
    {
        /* Разбираем override: vfloatXmY_t / vuintXmY_t / vintXmY_t */
        if (strncmp(e->vtype_override, "vfloat", 6) == 0)
        {
            /* vfloat32m1_t -> __riscv_vfmv_v_f_f32m1 */
            char inner[32];
            /* inner = "32m1" из "vfloat32m1_t" */
            strncpy(inner, e->vtype_override + 6, sizeof(inner) - 1);
            inner[sizeof(inner) - 1] = '\0';
            char *t = strstr(inner, "_t");
            if (t)
                *t = '\0';
            snprintf(buf, sizeof(buf), "__riscv_vfmv_v_f_f%s", inner);
        }
        else if (strncmp(e->vtype_override, "vuint", 5) == 0)
        {
            /* vuint32m1_t -> __riscv_vmv_v_x_u32m1 */
            char inner[32];
            strncpy(inner, e->vtype_override + 5, sizeof(inner) - 1); /* "32m1_t" */
            inner[sizeof(inner) - 1] = '\0';
            char *t = strstr(inner, "_t");
            if (t)
                *t = '\0';
            snprintf(buf, sizeof(buf), "__riscv_vmv_v_x_u%s", inner);
        }
        else
        {
            /* vint32m1_t -> __riscv_vmv_v_x_i32m1 */
            char inner[32];
            strncpy(inner, e->vtype_override + 4, sizeof(inner) - 1); /* "32m1_t" */
            inner[sizeof(inner) - 1] = '\0';
            char *t = strstr(inner, "_t");
            if (t)
                *t = '\0';
            snprintf(buf, sizeof(buf), "__riscv_vmv_v_x_i%s", inner);
        }
        return buf;
    }
    if (entry_is_float(e))
        snprintf(buf, sizeof(buf), "__riscv_vfmv_v_f_f%d%s", e->esize, e->lmul);
    else
        snprintf(buf, sizeof(buf), "__riscv_vmv_v_x_i%d%s", e->esize, e->lmul);
    return buf;
}

/* Возвращает интринсик извлечения скаляра из вектора — нужен, чтобы
 * "использовать" результат после цикла и не дать компилятору
 * выкинуть весь бенчмарк как мёртвый код (см. bench_lat/_thr). */
static const char *c_extract_scalar(const BenchEntry *e)
{
    static char buf[80];
    if (entry_is_float(e))
        snprintf(buf, sizeof(buf), "__riscv_vfmv_f_s_f%d%s_f%d", e->esize, e->lmul, e->esize);
    else if (entry_is_uint(e))
        snprintf(buf, sizeof(buf), "__riscv_vmv_x_s_u%d%s_u%d", e->esize, e->lmul, e->esize);
    else
        snprintf(buf, sizeof(buf), "__riscv_vmv_x_s_i%d%s_i%d", e->esize, e->lmul, e->esize);
    return buf;
}

static const char *c_vsetvl(const BenchEntry *e)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "__riscv_vsetvl_e%d%s(%d)", e->esize, e->lmul, e->vl);
    return buf;
}

static const char *init_val_lat(const BenchEntry *e) { return entry_is_float(e) ? "1.0" : "1"; }
static const char *init_val_step(const BenchEntry *e) { return entry_is_float(e) ? "0.00001" : "1"; }
static const char *init_val_thr(const BenchEntry *e) { return entry_is_float(e) ? "1.0" : "1"; }
static const char *step_val_thr(const BenchEntry *e) { return entry_is_float(e) ? "1.0001" : "2"; }

static int parse_config(const char *path, BenchEntry *entries, int max_entries)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "[ERROR] Не могу открыть файл: %s\n", path);
        return -1;
    }

    int count = 0;
    char line[MAX_LINE_LEN];
    int lineno = 0;

    while (fgets(line, sizeof(line), f))
    {
        lineno++;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        trim(line);
        if (!*line || line[0] == '#')
            continue;
        if (count >= max_entries)
        {
            fprintf(stderr, "[WARN] Достигнут лимит %d записей.\n", max_entries);
            break;
        }

        char copy[MAX_LINE_LEN];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        char *fields[8];
        int nf = split_pipe(copy, fields, 8);
        if (nf < 7)
        {
            fprintf(stderr, "[WARN] Строка %d: ожидается минимум 7 полей через '|', пропускаю.\n", lineno);
            continue;
        }

        BenchEntry *e = &entries[count];
        memset(e, 0, sizeof(*e));
        for (int i = 0; i < nf; i++)
            trim(fields[i]);

        strncpy(e->name, fields[0], MAX_FIELD_LEN - 1);
        strncpy(e->expr_lat, fields[1], MAX_FIELD_LEN - 1);
        strncpy(e->expr_thr, fields[2], MAX_FIELD_LEN - 1);
        strncpy(e->setup, fields[3], MAX_FIELD_LEN - 1);
        e->vl = atoi(fields[4]);
        strncpy(e->lmul, fields[5], 7);
        e->esize = atoi(fields[6]);
        if (nf >= 8)
            strncpy(e->vtype_override, fields[7], MAX_FIELD_LEN - 1);

        make_safe_name(e->name, e->safe_name, MAX_SAFE_NAME);
        count++;
    }
    fclose(f);
    return count;
}

static void generate_c(FILE *out, const BenchEntry *entries, int n, uint64_t iterations)
{
    fprintf(out,
            "/* AUTO-GENERATED by rvv_bench.c */\n"
            "#include <stdio.h>\n"
            "#include <stdint.h>\n"
            "#include <riscv_vector.h>\n"
            "\n"
            "#define ITERATIONS %lluULL\n"
            "\n"
            "/* CPU ~1600 MHz, таймер ~24 MHz -> 1600/24 ~= 66.667. */\n"
            "#define CYCLE_RATIO 66.667\n"
            "\n"
            "static uint64_t read_ticks(void) {\n"
            "    uint64_t ticks;\n"
            "    asm volatile(\"rdtime %%0\" : \"=r\"(ticks));\n"
            "    return ticks;\n"
            "}\n\n",
            (unsigned long long)iterations);

    for (int i = 0; i < n; i++)
    {
        const BenchEntry *e = &entries[i];
        const char *vtype = c_vector_type(e);
        const char *vsetvl = c_vsetvl(e);
        const char *vfmv = c_vfmv(e);

        /* Нужно ли присваивать результат в v_res / v1..v4?
         * Если expr содержит "v_res" / "vN" — да.
         * Иначе — аккумулятор объявлен в SETUP, просто вызываем как statement. */
        int lat_has_vres = (strstr(e->expr_lat, "v_res") != NULL);
        int thr_has_vN = (strstr(e->expr_thr, "vN") != NULL);

        /* ---- latency ---- */
        fprintf(out,
                "static void bench_lat_%s(void) {\n"
                "    size_t vl = %s;\n"
                "    %s v_res  = %s(%s, vl);\n"
                "    %s v_step = %s(%s, vl);\n",
                e->safe_name, vsetvl,
                vtype, vfmv, init_val_lat(e),
                vtype, vfmv, init_val_step(e));
        write_setup(out, e->setup, "    ");
        fprintf(out,
                "    uint64_t start = read_ticks();\n"
                "    for (uint64_t ii = 0; ii < ITERATIONS; ++ii) {\n");
        if (lat_has_vres)
        {
            fprintf(out,
                    "        v_res = %s;\n"
                    "        v_res = %s;\n"
                    "        v_res = %s;\n"
                    "        v_res = %s;\n",
                    e->expr_lat, e->expr_lat, e->expr_lat, e->expr_lat);
        }
        else
        {
            fprintf(out,
                    "        %s;\n"
                    "        %s;\n"
                    "        %s;\n"
                    "        %s;\n",
                    e->expr_lat, e->expr_lat, e->expr_lat, e->expr_lat);
        }
        fprintf(out, "    }\n"
                     "    uint64_t end = read_ticks();\n");
        if (lat_has_vres)
        {
            fprintf(out,
                    "    volatile double sink = (double)%s(v_res);\n"
                    "    (void)sink;\n",
                    c_extract_scalar(e));
        }
        fprintf(out,
                "    uint64_t total_ticks = end - start;\n"
                "    double total_cycles = (double)total_ticks * CYCLE_RATIO;\n"
                "    double n_ops = (double)ITERATIONS * 4.0;\n"
                "    printf(\"[Latency]    %%-30s | total_cycles=%%.0f | latency =%%.3f\\n\",\n"
                "           \"%s\", total_cycles, total_cycles / n_ops);\n"
                "}\n\n",
                e->name);

        /* ---- throughput ---- */
        /* Заменяем "vN" на v1..v4 в expr_thr */
        char thr1[MAX_FIELD_LEN], thr2[MAX_FIELD_LEN],
            thr3[MAX_FIELD_LEN], thr4[MAX_FIELD_LEN], buf[MAX_FIELD_LEN];
        str_replace_all(e->expr_thr, "vN", "v1", thr1, sizeof(thr1));
        str_replace_all(e->expr_thr, "vN", "v2", buf, sizeof(thr2));
        str_replace_all(buf, "pbuf", "pbuf + vl", thr2, sizeof(thr2));

        str_replace_all(e->expr_thr, "vN", "v3", buf, sizeof(thr3));
        str_replace_all(buf, "pbuf", "pbuf + vl*2", thr3, sizeof(thr3));
        
        str_replace_all(e->expr_thr, "vN", "v4", buf, sizeof(thr4));
        str_replace_all(buf, "pbuf", "pbuf + vl*3", thr4, sizeof(thr4));

        fprintf(out,
                "static void bench_thr_%s(void) {\n"
                "    size_t vl = %s;\n",
                e->safe_name, vsetvl);
        if (thr_has_vN)
        {
            fprintf(out,
                    "    %s v1     = %s(%s, vl);\n"
                    "    %s v2     = %s(%s, vl);\n"
                    "    %s v3     = %s(%s, vl);\n"
                    "    %s v4     = %s(%s, vl);\n",
                    vtype, vfmv, init_val_thr(e),
                    vtype, vfmv, init_val_thr(e),
                    vtype, vfmv, init_val_thr(e),
                    vtype, vfmv, init_val_thr(e));
        }
        fprintf(out, "    %s v_step = %s(%s, vl);\n",
                vtype, vfmv, step_val_thr(e));
        write_setup(out, e->setup, "    ");
        fprintf(out,
                "    uint64_t start = read_ticks();\n"
                "    for (uint64_t ii = 0; ii < ITERATIONS; ++ii) {\n");
        if (thr_has_vN)
        {
            fprintf(out,
                    "        v1 = %s;\n"
                    "        v2 = %s;\n"
                    "        v3 = %s;\n"
                    "        v4 = %s;\n",
                    thr1, thr2, thr3, thr4);
        }
        else
        {
            fprintf(out,
                    "        %s;\n"
                    "        %s;\n"
                    "        %s;\n"
                    "        %s;\n",
                    thr1, thr2, thr3, thr4);
        }
        fprintf(out, "    }\n"
                     "    uint64_t end = read_ticks();\n");
        if (thr_has_vN)
        {
            const char *ex = c_extract_scalar(e);
            fprintf(out,
                    "    volatile double sink = (double)%s(v1) + (double)%s(v2)\n"
                    "                         + (double)%s(v3) + (double)%s(v4);\n"
                    "    (void)sink;\n",
                    ex, ex, ex, ex);
        }
        fprintf(out,
                "    uint64_t total_ticks = end - start;\n"
                "    double total_cycles = (double)total_ticks * CYCLE_RATIO;\n"
                "    double n_ops = (double)ITERATIONS * 4.0;\n"
                "    printf(\"[Throughput] %%-30s | total_cycles=%%.0f | throughput=%%.2f\\n\",\n"
                "           \"%s\", total_cycles, total_cycles / n_ops);\n"
                "}\n\n",
                e->name);
    }

    /* main */
    fprintf(out,
            "int main(void) {\n"
            "    printf(\"=== RVV Microbenchmarks (ITERATIONS=%%llu) ===\\n\\n\",\n"
            "           (unsigned long long)ITERATIONS);\n");
    for (int i = 0; i < n; i++)
    {
        const BenchEntry *e = &entries[i];
        fprintf(out,
                "    printf(\"--- %s ---\\n\");\n"
                "    bench_lat_%s();\n"
                "    bench_thr_%s();\n"
                "    printf(\"\\n\");\n",
                e->name, e->safe_name, e->safe_name);
    }
    fprintf(out, "    return 0;\n}\n");
}

typedef struct
{
    char config[MAX_PATH_LEN];
    uint64_t iterations;
    char output[MAX_PATH_LEN];
    char cc[MAX_PATH_LEN];
    char cflags[MAX_PATH_LEN];
    char generated_c[MAX_PATH_LEN];
    int only_generate;
} Args;

static void args_defaults(Args *a)
{
    strncpy(a->config, "", MAX_PATH_LEN - 1);
    a->iterations = 10000000ULL;
    strncpy(a->output, "", MAX_PATH_LEN - 1);
    strncpy(a->cc, "gcc-14", MAX_PATH_LEN - 1);
    strncpy(a->cflags, "-O2 -march=rv64gcv -mabi=lp64d -lm", MAX_PATH_LEN - 1);
    strncpy(a->generated_c, "bench_generated.c", MAX_PATH_LEN - 1);
    a->only_generate = 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Использование: %s instructions.txt [опции]\n"
            "\n"
            "Опции:\n"
            "  --iterations N        число итераций (по умолч. 10000000)\n"
            "  --output FILE         файл для результатов (по умолч. stdout)\n"
            "  --cc PATH             компилятор (по умолч. gcc)\n"
            "  --cflags \"FLAGS\"      флаги (по умолч. -O2 -march=rv64gcv -mabi=lp64d -lm)\n"
            "  --generated-c FILE    куда сохранить C-файл (по умолч. bench_generated.c)\n"
            "  --only-generate       только сгенерировать C, не компилировать\n"
            "\n"
            "Формат конфига (8 полей через '|', 8-е необязательно):\n"
            "  NAME | EXPR_LAT | EXPR_THR | SETUP | VL | LMUL | ESIZE | VTYPE\n",
            prog);
}

static int parse_args(int argc, char **argv, Args *a)
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return -1;
    }
    strncpy(a->config, argv[1], MAX_PATH_LEN - 1);
    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
            a->iterations = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            strncpy(a->output, argv[++i], MAX_PATH_LEN - 1);
        else if (!strcmp(argv[i], "--cc") && i + 1 < argc)
            strncpy(a->cc, argv[++i], MAX_PATH_LEN - 1);
        else if (!strcmp(argv[i], "--cflags") && i + 1 < argc)
            strncpy(a->cflags, argv[++i], MAX_PATH_LEN - 1);
        else if (!strcmp(argv[i], "--generated-c") && i + 1 < argc)
            strncpy(a->generated_c, argv[++i], MAX_PATH_LEN - 1);
        else if (!strcmp(argv[i], "--only-generate"))
            a->only_generate = 1;
        else
        {
            fprintf(stderr, "[ERROR] Неизвестный аргумент: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    Args args;
    args_defaults(&args);
    if (parse_args(argc, argv, &args) != 0)
        return 1;

    static BenchEntry entries[MAX_ENTRIES];
    int n = parse_config(args.config, entries, MAX_ENTRIES);
    if (n <= 0)
    {
        fprintf(stderr, "[ERROR] Конфиг пуст или содержит ошибки.\n");
        return 1;
    }
    fprintf(stderr, "[INFO] Загружено инструкций: %d\n", n);

    FILE *cf = fopen(args.generated_c, "w");
    if (!cf)
    {
        fprintf(stderr, "[ERROR] Не могу создать файл: %s\n", args.generated_c);
        return 1;
    }
    generate_c(cf, entries, n, args.iterations);
    fclose(cf);
    fprintf(stderr, "[INFO] C-код сохранён в %s\n", args.generated_c);

    if (args.only_generate)
    {
        fprintf(stderr, "[INFO] --only-generate: компиляция пропущена.\n");
        return 0;
    }

    char bin_path[] = "/tmp/rvv_bench_bin";
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "%s %s -o %s %s", args.cc, args.cflags, bin_path, args.generated_c);
    fprintf(stderr, "[CC] %s\n", cmd);
    if (system(cmd) != 0)
    {
        fprintf(stderr, "[ERROR] Компиляция провалилась.\n");
        return 1;
    }

    fprintf(stderr, "[RUN] Запускаю %s ...\n", bin_path);
    if (*args.output)
    {
        char run_cmd[MAX_CMD_LEN];
        snprintf(run_cmd, sizeof(run_cmd), "%s > %s", bin_path, args.output);
        if (system(run_cmd) != 0)
        {
            fprintf(stderr, "[ERROR] Программа завершилась с ошибкой.\n");
            return 1;
        }
        fprintf(stderr, "[INFO] Результаты сохранены в %s\n", args.output);
    }
    else
    {
        if (system(bin_path) != 0)
        {
            fprintf(stderr, "[ERROR] Программа завершилась с ошибкой.\n");
            return 1;
        }
    }
    return 0;
}
