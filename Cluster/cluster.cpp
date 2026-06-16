#include "cluster.h"
#include <QDebug>
#include <cmath>

// QNX: use Qt logging (slog2 disabled due to cross-compilation linker issues)
#define SLOG_ERR(msg)  qWarning()  << "[backend]" << msg
#define SLOG_INFO(msg) qDebug()    << "[backend]" << msg

// QNX-safe replacement for std::clamp (C++17 stdlib incomplete on some QCC targets)
template<typename T>
static inline T qnx_clamp(T val, T lo, T hi)
{
    return val < lo ? lo : (val > hi ? hi : val);
}

// ── Constructor: start the SPI reader thread ─────────────────────────────
VehicleBackend::VehicleBackend(QObject *parent)
    : QObject(parent)
{
    m_spiReader = new SpiReader(this);

    // QueuedConnection: stm32_data_t is delivered from the reader thread
    // into the main (Qt) thread safely
    connect(m_spiReader, &SpiReader::newData,
            this,         &VehicleBackend::onSpiData,
            Qt::QueuedConnection);

    m_spiReader->start();
    SLOG_INFO("SpiReader thread started");
}

// ── Destructor: stop reader cleanly ──────────────────────────────────────
VehicleBackend::~VehicleBackend()
{
    if (m_spiReader) {
        m_spiReader->stop();
        m_spiReader->wait(1000);  // give it up to 1s to exit
    }
}

// ── Slot: called once per new SPI sample (every 100 ms) ──────────────────
void VehicleBackend::onSpiData(stm32_data_t data)
{
    // Map STM32 fields to cluster properties.
    // Adjust these scalers to match your actual STM32 firmware.
    //
    //   field1 (uint16) — sensor A raw ADC   → vibration axis 1
    //   field2 (uint16) — sensor B raw ADC   → vibration axis 2
    //   field3 (int16)  — temperature × 10   → temp (°C)
    //   field4 (uint16) — voltage × 100 mV   → voltage (V)
    //   field5 (uint8)  — status flags
    //   field6 (uint16) — RPM                → speed (after gearing)
    //   field7 (uint32) — STM32 ms timestamp → latency debug only

    const float v = data.field4 / 100.0f;   // 1234 → 12.34 V
    const float t = data.field3 / 10.0f;    // 215  → 21.5 °C
    const float rpm = static_cast<float>(data.field6);

    // Adjust this scale factor for your wheel diameter / gear ratio
    // const float speed_kmh = rpm * 0.05f;
    const float speed_kmh = rpm;

    const float vibA = data.field1 / 1000.0f;
    const float vibB = data.field2 / 1000.0f;
    const float vibTotal = std::sqrt(vibA * vibA + vibB * vibB);

    // Current isn't in the STM32 payload yet — assume estimated from voltage
    // for now, or wire it through field5/extra fields later
    const float i = m_current;  // keep last known until you have a real source

    const float rawPower = qnx_clamp((v * i / MAX_POWER) * 100.f, 0.f, 100.f);
    m_powerSmoothed      = m_powerSmoothed + POWER_SMOOTH * (rawPower - m_powerSmoothed);

    const float battPct  = qnx_clamp(
        (v - VOLT_MIN) / (VOLT_MAX - VOLT_MIN) * 100.f, 0.f, 100.f);

    setSpeed(speed_kmh);
    setVoltage(v);
    setTemp(t);
    setVibTotal(vibTotal);
    setPower(m_powerSmoothed);
    setBattery(battPct);

    evaluateWarnings();
}

// ── Warning logic ─────────────────────────────────────────────────────────
void VehicleBackend::evaluateWarnings()
{
    bool sw   = m_speed    >= SPEED_WARN;
    bool tw   = m_temp     >= TEMP_WARN;
    bool vw   = m_vibTotal >= VIB_WARN;
    bool volw = m_voltage  <= VOLT_WARN;
    bool crit = m_speed    >= SPEED_CRIT
                || m_temp     >= TEMP_WARN  + 20.f
                || m_vibTotal >= VIB_WARN   * 2.f;

    if (sw   != m_speedWarning)   { m_speedWarning   = sw;   emit speedWarningChanged();   }
    if (tw   != m_tempWarning)    { m_tempWarning    = tw;   emit tempWarningChanged();    }
    if (vw   != m_vibWarning)     { m_vibWarning     = vw;   emit vibWarningChanged();     }
    if (volw != m_voltageWarning) { m_voltageWarning = volw; emit voltageWarningChanged(); }
    if (crit != m_criticalAlert)  { m_criticalAlert  = crit; emit criticalAlertChanged();  }
}

// ── Setters ───────────────────────────────────────────────────────────────
void VehicleBackend::setSpeed(float v)    { if (m_speed    != v) { m_speed    = v; emit speedChanged();    } }
void VehicleBackend::setPower(float v)    { if (m_power    != v) { m_power    = v; emit powerChanged();    } }
void VehicleBackend::setBattery(float v)  { if (m_battery  != v) { m_battery  = v; emit batteryChanged();  } }
void VehicleBackend::setTemp(float v)     { if (m_temp     != v) { m_temp     = v; emit tempChanged();     } }
void VehicleBackend::setVibTotal(float v) { if (m_vibTotal != v) { m_vibTotal = v; emit vibTotalChanged(); } }
void VehicleBackend::setCurrent(float v)  { if (m_current  != v) { m_current  = v; emit currentChanged();  } }
void VehicleBackend::setVoltage(float v)  { if (m_voltage  != v) { m_voltage  = v; emit voltageChanged();  } }
