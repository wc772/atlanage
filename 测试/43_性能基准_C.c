



#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);

#define TICK() GetTickCount64()


long long fib(long long n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

void test_fib(void) {
    unsigned long long t0 = TICK();
    long long r = fib(30);
    unsigned long long t1 = TICK();
    printf("  [1] fib(30)=%lld  耗时=%llums\n", r, t1 - t0);
}


void test_accumulate(void) {
    unsigned long long t0 = TICK();
    long long sum = 0;
    for (long long i = 1; i <= 1000000; i++) {
        sum += i;
    }
    unsigned long long t1 = TICK();
    printf("  [2] 累加(100万)=%lld  耗时=%llums\n", sum, t1 - t0);
}


void test_nested(void) {
    unsigned long long t0 = TICK();
    long long sum = 0;
    for (int i = 0; i < 500; i++) {
        for (int j = 0; j < 500; j++) {
            sum += i * j;
        }
    }
    unsigned long long t1 = TICK();
    printf("  [3] 嵌套500x500 sum=%lld  耗时=%llums\n", sum, t1 - t0);
}


void test_branch(void) {
    unsigned long long t0 = TICK();
    long long a = 0, b = 0;
    for (long long i = 0; i < 1000000; i++) {
        if (i % 2 == 0) a++; else b++;
    }
    unsigned long long t1 = TICK();
    printf("  [4] 分支(100万) a=%lld b=%lld  耗时=%llums\n", a, b, t1 - t0);
}


static int empty_func(void) { return 0; }

void test_call(void) {
    unsigned long long t0 = TICK();
    for (int i = 0; i < 500000; i++) {
        empty_func();
    }
    unsigned long long t1 = TICK();
    printf("  [5] 空调用(50万)  耗时=%llums\n", t1 - t0);
}


void test_string(void) {
    unsigned long long t0 = TICK();
    char buf[2000] = "";
    for (int i = 0; i < 1000; i++) {
        char tmp[2] = "x";
        strcat(buf, tmp);
    }
    unsigned long long t1 = TICK();
    printf("  [6] 字符串拼接x1000 len=%zu  耗时=%llums\n", strlen(buf), t1 - t0);
}


void test_compute(void) {
    unsigned long long t0 = TICK();
    long long d = 0;
    for (long long i = 0; i < 1000000; i++) {
        long long x = i % 100;
        long long y = i / 100;
        d += (x * x + y * y) / 2;
    }
    unsigned long long t1 = TICK();
    printf("  [7] 坐标运算(100万) d=%lld  耗时=%llums\n", d, t1 - t0);
}


int main(void) {
    printf("================================\n");
    printf("  C 性能基准测试 v2.0 (gcc -O2)\n");
    printf("================================\n\n");

    test_fib();
    test_accumulate();
    test_nested();
    test_branch();
    test_call();
    test_string();
    test_compute();

    printf("\n--------------------------------\n");
    return 0;
}
