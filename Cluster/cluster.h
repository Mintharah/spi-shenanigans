#ifndef CLUSTER_H
#define CLUSTER_H

#pragma once
#include <QObject>
#include <QString>
#include <qqml.h>
#include "../shm_spi.h"
#include "SpiReader.h"

class VehicleBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(float speed      READ speed      NOTIFY speedChanged)
    Q_PROPERTY(float power      READ power      NOTIFY powerChanged)
    Q_PROPERTY(float battery    READ battery    NOTIFY batteryChanged)
    Q_PROPERTY(float temp       READ temp       NOTIFY tempChanged)
    Q_PROPERTY(float vibTotal   READ vibTotal   NOTIFY vibTotalChanged)
    Q_PROPERTY(float current    READ current    NOTIFY currentChanged)
    Q_PROPERTY(float voltage    READ voltage    NOTIFY voltageChanged)

    Q_PROPERTY(bool speedWarning    READ speedWarning   NOTIFY speedWarningChanged)
    Q_PROPERTY(bool tempWarning     READ tempWarning    NOTIFY tempWarningChanged)
    Q_PROPERTY(bool vibWarning      READ vibWarning     NOTIFY vibWarningChanged)
    Q_PROPERTY(bool voltageWarning  READ voltageWarning NOTIFY voltageWarningChanged)
    Q_PROPERTY(bool criticalAlert   READ criticalAlert  NOTIFY criticalAlertChanged)

public:
    explicit VehicleBackend(QObject *parent = nullptr);
    ~VehicleBackend();

    static constexpr float SPEED_WARN   = 130.0f;
    static constexpr float SPEED_CRIT   = 160.0f;
    static constexpr float TEMP_WARN    = 80.0f;
    static constexpr float VIB_WARN     = 2.0f;
    static constexpr float CURRENT_WARN = 50.0f;
    static constexpr float MAX_POWER    = 1000.0f;
    static constexpr float POWER_SMOOTH = 0.15f;
    // For a 12V lead-acid pack
    static constexpr float VOLT_MIN  = 10.0f;   // 0% battery
    static constexpr float VOLT_MAX  = 12.6f;   // 100% battery
    static constexpr float VOLT_WARN = 10.8f;   // warn at ~25% charge

    float speed()    const { return m_speed;    }
    float power()    const { return m_power;    }
    float battery()  const { return m_battery;  }
    float temp()     const { return m_temp;     }
    float vibTotal() const { return m_vibTotal; }
    float current()  const { return m_current;  }
    float voltage()  const { return m_voltage;  }

    bool speedWarning()   const { return m_speedWarning;   }
    bool tempWarning()    const { return m_tempWarning;    }
    bool vibWarning()     const { return m_vibWarning;     }
    bool voltageWarning() const { return m_voltageWarning; }
    bool criticalAlert()  const { return m_criticalAlert;  }

public slots:
    void onSpiData(stm32_data_t data);

signals:
    void speedChanged();
    void powerChanged();
    void batteryChanged();
    void tempChanged();
    void vibTotalChanged();
    void currentChanged();
    void voltageChanged();

    void speedWarningChanged();
    void tempWarningChanged();
    void vibWarningChanged();
    void voltageWarningChanged();
    void criticalAlertChanged();

private:
    void evaluateWarnings();

    void setSpeed(float v);
    void setPower(float v);
    void setBattery(float v);
    void setTemp(float v);
    void setVibTotal(float v);
    void setCurrent(float v);
    void setVoltage(float v);

    SpiReader *m_spiReader = nullptr;

    float m_speed    = 0.f;
    float m_power    = 0.f;
    float m_battery  = 100.f;
    float m_temp     = 0.f;
    float m_vibTotal = 0.f;
    float m_current  = 0.f;
    float m_voltage  = 0.f;
    float m_powerSmoothed = 0.f;

    bool m_speedWarning   = false;
    bool m_tempWarning    = false;
    bool m_vibWarning     = false;
    bool m_voltageWarning = false;
    bool m_criticalAlert  = false;
};

#endif // CLUSTER_H
