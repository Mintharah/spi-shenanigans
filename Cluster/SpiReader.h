#ifndef SPIREADER_H
#define SPIREADER_H

#include <QThread>
#include "../shm_spi.h"

// Background thread that polls the SPI shared-memory block produced by the
// spi_master process. On every new sample (detected via a changed seq counter)
// it emits newData() to be picked up by VehicleBackend::onSpiData via a
// queued connection.
class SpiReader : public QThread {
    Q_OBJECT
public:
    explicit SpiReader(QObject *parent = nullptr);
    void stop();

signals:
    void newData(stm32_data_t data);

protected:
    void run() override;

private:
    volatile bool  m_running = true;
    spi_shared_t  *m_shm     = nullptr;
};

#endif // SPIREADER_H
