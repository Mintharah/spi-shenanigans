#include "SpiReader.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <climits>
#include <QDebug>

SpiReader::SpiReader(QObject *parent) : QThread(parent) {}

void SpiReader::run()
{
    // ── Open the shared memory created by spi_master ──
    // Retry a few times in case spi_master started slightly after us.
    int fd = -1;
    for (int attempt = 0; attempt < 20 && m_running; ++attempt) {
        fd = shm_open(SHM_NAME, O_RDWR, 0);  // O_RDWR so sem_wait/post can update
        if (fd != -1) break;
        usleep(250000);  // 250 ms between attempts
    }
    if (fd == -1) {
        qWarning("SpiReader: shm_open failed — is spi_master running?");
        return;
    }

    m_shm = (spi_shared_t*)mmap(NULL, sizeof(spi_shared_t),
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m_shm == MAP_FAILED) {
        qWarning("SpiReader: mmap failed");
        return;
    }

    uint32_t last_seq = UINT32_MAX;  // force the first read to register as new

    // ── Poll loop ──
    while (m_running) {
        sem_wait(&m_shm->lock);
        uint32_t     seq  = m_shm->seq;
        stm32_data_t data = m_shm->data;
        sem_post(&m_shm->lock);

        if (seq != last_seq) {
            last_seq = seq;
            emit newData(data);   // delivered to the Qt main thread via queued connection
        }

        usleep(10000);  // 10 ms — well above the 100 ms SPI tick rate
    }

    munmap(m_shm, sizeof(spi_shared_t));
    m_shm = nullptr;
}

void SpiReader::stop()
{
    m_running = false;
}
