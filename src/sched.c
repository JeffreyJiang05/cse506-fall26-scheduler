#include <stdio.h>
#include <bpf/bpf.h>

#include "test.skel.h"

int main() {
    struct test_bpf *bpf = NULL;

    bpf = test_bpf__open_and_load();

    printf("hello world\n");
    return 0;
}