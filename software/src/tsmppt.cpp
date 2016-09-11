#include <errno.h>
#include <QsLog.h>
#include <qtimer.h>
#include "tsmppt.h"

const int REG_V_PU          = 0;
const int REG_I_PU          = 2;
const int REG_VER_SW        = 4;
const int REG_FIRST_DYN     = 24;
const int REG_V_BAT         = 24;
const int REG_V_PV          = 27;
const int REG_I_PV          = 29;
const int REG_I_CC          = 28;
const int REG_T_BAT         = 37;
const int REG_CHARGE_STATE  = 50;
const int REG_POUT          = 58;
const int REG_WHC_DAILY     = 68;
const int REG_POUT_MAX_DAILY= 70;
const int REG_LAST_DYN      = 79;
const int REG_EHW_VERSION   = 57549;
const int REG_ESERIAL       = 57536;

static struct _TsmpptDynVals
{
    double m_v_pu;      // Voltage scaling
    double m_i_pu;      // Current scaling
    double m_v_bat;     // Battery voltage
    double m_t_bat;     // Battery temperature
    double m_i_cc;      // Charging current
    double m_v_pv;      // PV array voltage 
    double m_pout;      // Output power
    double m_i_pv;      // PV array current
    double m_p_max;     // Max power, daily
    double m_whc;       // Watt hours, daily
    int m_cs;           // Charge state
} TsmpptDynVals;

static struct _TsmpptStatVals
{
    QString     m_fw_ver;
    QString     m_hw_ver;
    uint64_t    m_serial;
} TsmpptStatVals;

Tsmppt::Tsmppt(const QString &IPAddress, const int port, int slave, QObject *parent):
QObject(parent), mInitialized(false), mTimer(new QTimer(this))
{
    QLOG_DEBUG() << "Tsmppt::Tsmppt(" << IPAddress << ", " << port << ")";
    mCtx = modbus_new_tcp_pi(IPAddress.toStdString().c_str(), QString::number(port).toStdString().c_str());
    Q_ASSERT(mCtx != NULL);
    // modbus_set_debug(mCtx, true);
    modbus_set_error_recovery(mCtx, MODBUS_ERROR_RECOVERY_LINK);
    modbus_set_response_timeout(mCtx, 20, 0);
    modbus_set_byte_timeout(mCtx, 10, 0);
    modbus_set_slave(mCtx, slave);
    mTimer->setInterval(2000);
    mTimer->start();
    connect(mTimer, SIGNAL(timeout()), this, SLOT(onTimeout()));
}

Tsmppt::~Tsmppt()
{
    QLOG_DEBUG() << "Tsmppt::~Tsmppt()";
    modbus_close(mCtx);
    modbus_free(mCtx);
}

void Tsmppt::readInputRegisters(int addr, int nb, uint16_t *dest)
{
    if(modbus_read_input_registers(mCtx, addr, nb, dest) == -1)
    {
        if (errno == ECONNRESET)
        {
            // Try one more time before giving up:
            if(modbus_read_input_registers(mCtx, addr, nb, dest) == -1)
            {
                QLOG_ERROR() << "MODBUS:" << modbus_strerror(errno) << "Trying to reconnect";
                emit connectionLost();
            }
        }
        else if (errno != ETIMEDOUT)
        {
            QLOG_ERROR() << "MODBUS:" << modbus_strerror(errno) << "Trying to reconnect";
            emit connectionLost();
        }
    }
}

bool Tsmppt::initialize()
{
    Q_ASSERT(mCtx != NULL);

    QLOG_DEBUG() << "Tsmppt::initialize(start)";

    if (modbus_connect(mCtx) == -1)
    {
        QLOG_ERROR() << "MODBUS:" << modbus_strerror(errno);
        return false;
    }

    uint16_t regs[6];
    readInputRegisters(REG_V_PU, 6, regs);
    
    // Voltage scaling:
    TsmpptDynVals.m_v_pu = (float)regs[1];
    TsmpptDynVals.m_v_pu /= 65536.0;
    TsmpptDynVals.m_v_pu += (float)regs[0];

    // Current scaling:
    TsmpptDynVals.m_i_pu = (float)regs[3];
    TsmpptDynVals.m_i_pu /= 65536.0;
    TsmpptDynVals.m_i_pu += (float)regs[2];
    QLOG_DEBUG() << "Tsmppt: m_v_pu =" << TsmpptDynVals.m_v_pu << " m_i_pu =" << TsmpptDynVals.m_i_pu;

    // Firmware version:
    uint16_t ver = ((regs[4] >> 12) & 0x0f) * 1000;
    ver += ((regs[4] >> 8) & 0x0f) * 100;
    ver += ((regs[4] >> 4) & 0x0f) * 10;
    ver += regs[4] & 0x0f;
    TsmpptStatVals.m_fw_ver = QString::number(ver);

    // Read hardware version:
    readInputRegisters(REG_EHW_VERSION, 1, regs);
    TsmpptStatVals.m_hw_ver = QString::number(regs[0] >> 8) + "." + QString::number(regs[0] & 0xff);

    // Read serial number:
    readInputRegisters(REG_ESERIAL, 4, regs);
    TsmpptStatVals.m_serial = (uint64_t)((regs[0] & 0xff) - 0x30) * 10000000;
    TsmpptStatVals.m_serial += (uint64_t)((regs[0] >> 8) - 0x30) * 1000000;
    TsmpptStatVals.m_serial += (uint64_t)((regs[1] & 0xff) - 0x30) * 100000;
    TsmpptStatVals.m_serial += (uint64_t)((regs[1] >> 8) - 0x30) * 10000;
    TsmpptStatVals.m_serial += (uint64_t)((regs[2] & 0xff) - 0x30) * 1000;
    TsmpptStatVals.m_serial += (uint64_t)((regs[2] >> 8) - 0x30) * 100;
    TsmpptStatVals.m_serial += (uint64_t)((regs[3] & 0xff) - 0x30) * 10;
    TsmpptStatVals.m_serial += (uint64_t)((regs[3] >> 8) - 0x30);

    mInitialized = true;
    emit tsmpptConnected();
    QLOG_DEBUG() << "Tsmppt::initialize(end)";
    return true;
}

void Tsmppt::updateValues()
{
    uint16_t reg[REG_LAST_DYN-REG_FIRST_DYN+1];

    QLOG_DEBUG() << "Tsmppt::updateValues()";

    readInputRegisters(REG_FIRST_DYN, REG_LAST_DYN-REG_FIRST_DYN+1, reg);

    // Battery voltage:
    double temp = (double)reg[REG_V_BAT-REG_FIRST_DYN] * TsmpptDynVals.m_v_pu / 32768.0;
    setBatteryVoltage(temp);

    // Battery temperature:
    temp = (double)(int16_t)reg[REG_T_BAT-REG_FIRST_DYN];
    setBatteryTemperature(temp);

    // Charge current:
    temp = (double)reg[REG_I_CC-REG_FIRST_DYN] * TsmpptDynVals.m_i_pu / 32768.0;
    setChargingCurrent(temp);
    
    // MPPT output power:
    temp = (double)reg[REG_POUT-REG_FIRST_DYN] * TsmpptDynVals.m_i_pu * TsmpptDynVals.m_v_pu / 131072.0;
    setOutputPower(temp);

    // PV array voltage:
    temp = (double)reg[REG_V_PV-REG_FIRST_DYN]  * TsmpptDynVals.m_v_pu / 32768.0;
    setArrayVoltage(temp);

    // PV array current:
    temp = (double)reg[REG_I_PV-REG_FIRST_DYN] * TsmpptDynVals.m_i_pu / 32768.0;
    setArrayCurrent(temp);

   
    

    // Whc daily:
    temp = (double)reg[REG_WHC_DAILY-REG_FIRST_DYN];
    setWattHoursDaily(temp);

    // Pmax daily:
    temp = (double)reg[REG_POUT_MAX_DAILY-REG_FIRST_DYN] * TsmpptDynVals.m_i_pu * TsmpptDynVals.m_v_pu / 131072.0;
    setMaxPowerDaily(temp);

    // Charge state:
    setChargeState(reg[REG_CHARGE_STATE-REG_FIRST_DYN]);
}

QString Tsmppt::firmwareVersion() const
{
    return TsmpptStatVals.m_fw_ver;
}

QString Tsmppt::hardwareVersion() const
{
    return TsmpptStatVals.m_hw_ver;
}

uint64_t Tsmppt::serialNumber() const
{
    return TsmpptStatVals.m_serial;
}

double Tsmppt::batteryVoltage() const
{
    return TsmpptDynVals.m_v_bat;
}

void Tsmppt::setBatteryVoltage(double v)
{
    if (TsmpptDynVals.m_v_bat == v)
        return;
    TsmpptDynVals.m_v_bat = v;
    emit batteryVoltageChanged();
}

double Tsmppt::batteryTemperature() const
{
    return TsmpptDynVals.m_t_bat;
}

void Tsmppt::setBatteryTemperature(double v)
{
    if (TsmpptDynVals.m_t_bat == v)
        return;
    TsmpptDynVals.m_t_bat = v;
    emit batteryTemperatureChanged();
}

double Tsmppt::chargingCurrent() const
{
    return TsmpptDynVals.m_i_cc;
}

double Tsmppt::outputPower() const
{
    return TsmpptDynVals.m_pout;
}

void Tsmppt::setChargingCurrent(double v)
{
    if (TsmpptDynVals.m_i_cc == v)
        return;
    TsmpptDynVals.m_i_cc = v;
    emit chargingCurrentChanged();
}

void Tsmppt::setOutputPower(double v)
{
    if (TsmpptDynVals.m_pout == v)
        return;
    TsmpptDynVals.m_pout = v;
    emit outputPowerChanged();
}

double Tsmppt::arrayCurrent() const
{
    return TsmpptDynVals.m_i_pv;
}

void Tsmppt::setArrayCurrent(double v)
{
    if (TsmpptDynVals.m_i_pv == v)
        return;
    TsmpptDynVals.m_i_pv = v;
    emit arrayCurrentChanged();
}

double Tsmppt::arrayVoltage() const
{
    return TsmpptDynVals.m_v_pv;
}

void Tsmppt::setArrayVoltage(double v)
{
    if (TsmpptDynVals.m_v_pv == v)
        return;
    TsmpptDynVals.m_v_pv = v;
    emit arrayVoltageChanged();
}

double Tsmppt::maxPowerDaily() const
{
    return TsmpptDynVals.m_p_max;
}

void Tsmppt::setMaxPowerDaily(double v)
{
    if (TsmpptDynVals.m_p_max == v)
        return;
    TsmpptDynVals.m_p_max = v;
    emit maxPowerDailyChanged();
}

double Tsmppt::wattHoursDaily() const
{
    return TsmpptDynVals.m_whc;
}

void Tsmppt::setWattHoursDaily(double v)
{
    if (TsmpptDynVals.m_whc == v)
        return;
    TsmpptDynVals.m_whc = v;
    emit wattHoursDailyChanged();
}

int Tsmppt::chargeState() const
{
    // Will translate the TSMPPT charge state according to this table:
    // TSMPPT:              BlueSolar: 
    // 0 START              0 OFF
    // 1 NIGHT_CHECK        0 OFF
    // 2 DISCOCNNECT        0 OFF
    // 3 NIGHT              0 OFF
    // 4 FAULT              2 FAULT
    // 5 BULK               3 BULK
    // 6 ABSORPTION         4 ABSORPTION
    // 7 FLOAT              5 FLOAT
    // 8 EQUALIZE           7 EQUALIZE
    // 9 SLAVE              11 OTHER
    const int bscs[10] = {0,0,0,0,2,3,4,5,7,11};
    return bscs[TsmpptDynVals.m_cs];
}

void Tsmppt::setChargeState(int v)
{
    if (TsmpptDynVals.m_cs == v)
        return;
    TsmpptDynVals.m_cs = v;
    emit chargeStateChanged();
}

QString Tsmppt::productName() const
{
    return QString("TriStar MPPT 60");
}

void Tsmppt::onTimeout()
{
    if (!mInitialized)
    {
    mTimer->stop();
        initialize();
        mTimer->start();
        return;
    }
    updateValues();
}
