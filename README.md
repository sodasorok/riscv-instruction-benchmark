# riscv-instruction-benchmark
rvv_bench.c — генератор RVV-микробенчмарков.

Сборка (на плате OrangePi RV2):
  gcc -O2 -o rvv_bench rvv_bench.c
 
Использование:
  ./rvv_bench instructions.txt [options]
 
Опции:
    --iterations N        число итераций (по умолч. 10000000)
    --output FILE         куда сохранить результаты (по умолч. stdout)
    --cc PATH             компилятор (по умолч. gcc)
    --cflags "FLAGS"      флаги (по умолч. -O2 -march=rv64gcv -mabi=lp64d -lm)
    --generated-c FILE    куда сохранить сгенерированный C (по умолч. bench_generated.c)
    --only-generate       только сгенерировать C, не компилировать
 
  Формат строки конфига (8 полей через '|'):
    NAME | EXPR_LAT | EXPR_THR | SETUP | VL | LMUL | ESIZE | VTYPE
 
    VTYPE — необязательный явный тип вектора аккумулятора (например vuint32m1_t).
            Если пусто — определяется автоматически по ESIZE/LMUL и наличию 'vf' в EXPR.
            Соответствующий инициализирующий интринсик тоже берётся из VTYPE:
              vfloatXmY_t -> __riscv_vfmv_v_f_fXmY
              vuintXmY_t  -> __riscv_vmv_v_x_uXmY
              vintXmY_t   -> __riscv_vmv_v_x_iXmY
