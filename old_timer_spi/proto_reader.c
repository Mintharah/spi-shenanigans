#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdio.h>
#include "shm_spi.h"

int main(void)
{
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd == -1) { perror("shm_open"); return 1; }

    spi_shared_t *shm = mmap(NULL, sizeof(spi_shared_t),
                              PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    uint32_t last_seq = UINT32_MAX;

    while (1) {
        sem_wait(&shm->lock);
        uint32_t     seq  = shm->seq;
        stm32_data_t data = shm->data;
        sem_post(&shm->lock);

        if (seq != last_seq) {
            last_seq = seq;
            // Hand off to your protocol — CAN, UDP, custom, etc.
            proto_send(&data, sizeof(data));
        }

        usleep(10000);
    }

    munmap(shm, sizeof(spi_shared_t));
    return 0;
}