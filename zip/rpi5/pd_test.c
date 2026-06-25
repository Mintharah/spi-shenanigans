/*
 * pd_test.c -- RP1 live level diagnostic (QNX, MAP_PHYS)
 *
 * Reads GPIO17 input level directly from the RP1 GPIO_STATUS register
 * (bit 9 = INFROMPAD) to verify whether the pad is actually low or whether
 * rpi_gpio_input() is returning stale/cached data.
 *
 * Also prints the pad control register pull field so both can be compared.
 *
 * Build:
 *   qcc -Vgcc_ntoaarch64le -std=gnu11 -O2 pd_test.c -o pd_test
 *   sudo ./pd_test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#define GPIO_PIN           17

/* RP1 pad control (pull config) */
#define PAD_BASE           0x400d0000u
#define PAD_MAP_SIZE       0x100u
/* bits [4:3]: 00=none 01=pull-up 10=pull-down */
#define PULL_MASK          (0x3u << 3)
#define PULL_DOWN_BITS     (0x2u << 3)

/* RP1 GPIO block (live status) */
#define GPIO_BASE          0x400e0000u
#define GPIO_MAP_SIZE      0x1000u
/* Each GPIO has two 4-byte registers: STATUS (offset 0) and CTRL (offset 4).
 * GPIO_STATUS bit 9 = INFROMPAD = live pad sample after input override. */
#define GPIO_STATUS_STRIDE 8u          /* bytes per pin */
#define GPIO_STATUS_INFROMPAD_BIT 9u

#define POLL_MS            50

static const char *pull_name(uint32_t reg)
{
    switch ((reg >> 3) & 0x3u) {
    case 0: return "NONE";
    case 1: return "UP";
    case 2: return "DOWN";
    default: return "???";
    }
}

static void ms_sleep(int ms)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

static volatile uint32_t *map_phys(uint32_t base, size_t size)
{
    volatile uint32_t *p = mmap(NULL, size,
                                PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                MAP_SHARED | MAP_PHYS,
                                NOFD, (off_t)base);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap MAP_PHYS 0x%08x size 0x%zx: %s\n",
                base, size, strerror(errno));
        return NULL;
    }
    return p;
}

int main(void)
{
    volatile uint32_t *pad_base = map_phys(PAD_BASE, PAD_MAP_SIZE);
    if (!pad_base) return 1;

    volatile uint32_t *gpio_base = map_phys(GPIO_BASE, GPIO_MAP_SIZE);
    if (!gpio_base) { munmap((void*)pad_base, PAD_MAP_SIZE); return 1; }

    volatile uint32_t *pad_reg    = &pad_base[GPIO_PIN];
    /* GPIO_STATUS for pin N is at byte offset N*8; as uint32_t index: N*2 */
    volatile uint32_t *status_reg = &gpio_base[GPIO_PIN * 2];

    printf("[pd_test] PAD   base=0x%08x pin=%d reg=0x%08" PRIx32 " pull=%s\n",
           PAD_BASE, GPIO_PIN, *pad_reg, pull_name(*pad_reg));
    printf("[pd_test] GPIO  base=0x%08x STATUS=0x%08" PRIx32
           " INFROMPAD=%d\n",
           GPIO_BASE, *status_reg,
           (int)((*status_reg >> GPIO_STATUS_INFROMPAD_BIT) & 1u));

    /* ensure pull-down is set */
    {
        uint32_t v = *pad_reg;
        v &= ~PULL_MASK;
        v |=  PULL_DOWN_BITS;
        *pad_reg = v;
        printf("[pd_test] pull-down written: pad reg now 0x%08" PRIx32
               " pull=%s\n", *pad_reg, pull_name(*pad_reg));
    }

    printf("\n[pd_test] polling PAD + live INFROMPAD every %d ms\n", POLL_MS);
    printf("[pd_test] start motor_controller in another terminal now\n\n");

    uint32_t prev_pad    = *pad_reg;
    uint32_t prev_status = *status_reg;
    long iter = 0;

    for (;;) {
        ms_sleep(POLL_MS);
        uint32_t cur_pad    = *pad_reg;
        uint32_t cur_status = *status_reg;
        int level = (int)((cur_status >> GPIO_STATUS_INFROMPAD_BIT) & 1u);

        if (cur_pad != prev_pad || cur_status != prev_status) {
            printf("[pd_test] iter=%ld (~%ld ms)\n", iter, iter * POLL_MS);
            if (cur_pad != prev_pad)
                printf("  PAD:    0x%08" PRIx32 " (%s) -> 0x%08" PRIx32
                       " (%s)\n",
                       prev_pad, pull_name(prev_pad),
                       cur_pad,  pull_name(cur_pad));
            if (cur_status != prev_status)
                printf("  STATUS: 0x%08" PRIx32 " -> 0x%08" PRIx32
                       "  INFROMPAD=%d\n",
                       prev_status, cur_status, level);
            prev_pad    = cur_pad;
            prev_status = cur_status;
        }
        iter++;
    }

    munmap((void*)pad_base,  PAD_MAP_SIZE);
    munmap((void*)gpio_base, GPIO_MAP_SIZE);
    return 0;
}