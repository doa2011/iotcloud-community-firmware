#include <m1.h>
#include "bmp3_vendor/bmp3.h"
#include <assert.h>
#include <stdio.h>

mico_mutex_t m1_lock;
m1_state_t m1_state;
static uint32_t now;
static uint8_t registers[2][128], pointer[2];
static bool present[2], fail_combined, fail_initialize;
static unsigned writes[2], reads, calibration_reads, resets, held;
static uint16_t scd_pending, scd_fail_command, scd_pressure;
static uint32_t scd_due;
static unsigned pressure_attempts, pressure_writes, scd_finalizes;
static bool scd_crc_bad;

uint32_t mico_rtos_get_time(void) { return now; }
void mico_thread_msleep(uint32_t ms) { now += ms; }
void mico_rtos_lock_mutex(mico_mutex_t *lock) { (void)lock; assert(!held++); }
void mico_rtos_unlock_mutex(mico_mutex_t *lock) { (void)lock; assert(held-- == 1); }
OSStatus MicoI2cInitialize(mico_i2c_device_t *d)
{
    assert(d->port == MICO_I2C_1 && (d->address == 0x62 || (d->address >= 0x76 && d->address <= 0x77)));
    return fail_initialize ? kTimeoutErr : kNoErr;
}
OSStatus MicoI2cFinalize(mico_i2c_device_t *d)
{ assert(d->address==0x62 && !held);scd_finalizes++;return kNoErr; }
OSStatus MicoI2cBuildCombinedMessage(mico_i2c_message_t *m,const void *tx,void *rx,uint16_t nt,uint16_t nr,uint16_t retry)
{ assert(retry==1); *m=(mico_i2c_message_t){tx,rx,nt,nr}; return 0; }
OSStatus MicoI2cBuildTxMessage(mico_i2c_message_t *m,const void *tx,uint16_t n,uint16_t retry)
{ return MicoI2cBuildCombinedMessage(m,tx,NULL,n,0,retry); }
OSStatus MicoI2cBuildRxMessage(mico_i2c_message_t *m,void *rx,uint16_t n,uint16_t retry)
{ return MicoI2cBuildCombinedMessage(m,NULL,rx,0,n,retry); }
static OSStatus scd_transfer(mico_i2c_message_t *m)
{
    assert((int32_t)(now-scd_due)>=0);
    assert(!(m->tx_len && m->rx_len)); /* SCD requires STOP-separated reads. */
    if(m->tx_len) {
        assert(m->tx_len==2 || m->tx_len==5);
        uint16_t cmd=(uint16_t)m->tx[0]<<8|m->tx[1];
        assert(!scd_pending || cmd==0x3f86);
        if(cmd==0xe000) {
            assert(m->tx_len==5 && scd40_crc(m->tx+2)==m->tx[4]);
            uint16_t hpa=(uint16_t)m->tx[2]<<8|m->tx[3];
            assert(hpa>=700 && hpa<=1200);pressure_attempts++;
            if(cmd==scd_fail_command)return kTimeoutErr;
            scd_pressure=hpa;pressure_writes++;scd_due=now+1;
            if(hpa==987)assert(m->tx[2]==0x03 && m->tx[3]==0xdb && m->tx[4]==0x42);
        } else {
            assert(m->tx_len==2);
            assert(cmd==0x3f86 || cmd==0x3682 || cmd==0x21b1 || cmd==0xe4b8 || cmd==0xec05);
            if(cmd==scd_fail_command)return kTimeoutErr;
            scd_pending=(cmd==0x3682 || cmd==0xe4b8 || cmd==0xec05)?cmd:0;
            scd_due=now+(cmd==0x3f86?500:scd_pending?1:0);
        }
    } else {
        assert(scd_pending && m->rx_len==(scd_pending==0xe4b8?3u:9u));
        uint16_t words[3]={1,0,0};
        if(scd_pending==0x3682){words[0]=0x1234;words[1]=0xabcd;words[2]=0x0102;}
        if(scd_pending==0xec05){words[0]=800;words[1]=26214;words[2]=32768;}
        for(unsigned i=0;i<m->rx_len/3;i++) {
            m->rx[i*3]=words[i]>>8;m->rx[i*3+1]=words[i]&0xff;
            m->rx[i*3+2]=scd40_crc(m->rx+i*3);
        }
        if(scd_crc_bad)m->rx[m->rx_len-1]^=1;
        scd_pending=0;
    }
    return kNoErr;
}
OSStatus MicoI2cTransfer(mico_i2c_device_t *d,mico_i2c_message_t *m,uint16_t count)
{
    assert(count==1 && !held);
    if(d->address==0x62)return scd_transfer(m);
    unsigned i=d->address-0x76;
    if (!present[i] || (fail_combined && m->tx_len && m->rx_len)) return kTimeoutErr;
    if (m->tx_len) {
        pointer[i]=m->tx[0];
        if (m->tx_len>1) {
            assert(m->tx_len==2); /* Adapter configures one register per write. */
            assert(m1_barometer_model(registers[i][0])); /* Never configure unknown IDs. */
            writes[i]++;
            assert(pointer[i]+m->tx_len-1<=sizeof(registers[i]));
            memcpy(registers[i]+pointer[i],m->tx+1,m->tx_len-1);
            if (pointer[i]==BMP3_REG_CMD && m->tx[1]==BMP3_SOFT_RESET) {
                registers[i][BMP3_REG_PWR_CTRL]=0;
                registers[i][BMP3_REG_ERR]=0;
                resets++;
            }
        }
    }
    if (m->rx_len) {
        assert(m->rx_len<=21); /* Bounds vendor get_regs VLA for stack review. */
        reads++;
        assert(pointer[i]+m->rx_len<=sizeof(registers[i]));
        if (pointer[i]==BMP3_REG_CALIB_DATA) { assert(m->rx_len==21); calibration_reads++; }
        memcpy(m->rx,registers[i]+pointer[i],m->rx_len);
    }
    return 0;
}

static void attach(unsigned address,uint8_t id)
{
    unsigned i=address-0x76;
    present[i]=true;
    memset(registers[i],0,sizeof(registers[i]));
    registers[i][0]=id;
    registers[i][BMP3_REG_SENS_STATUS]=0x70;
    /* Captured device vector: a decoding regression, not an accuracy reference. */
    const uint8_t trim[]={0x5e,0x6d,0xde,0x48,0xf6,0xd3,0x01,0x6f,0xf6,0x23,0x00,
        0x7b,0x62,0x49,0x79,0xf3,0xf6,0x64,0x42,0x16,0xc4};
    memcpy(registers[i]+BMP3_REG_CALIB_DATA,trim,sizeof(trim));
    uint32_t pressure=8463968,temperature=8663552;
    for(unsigned j=0;j<3;j++) {
        registers[i][BMP3_REG_DATA+j]=pressure>>(8*j);
        registers[i][BMP3_REG_DATA+3+j]=temperature>>(8*j);
    }
}
static void tick(uint32_t at) { now=at; m1_barometer_tick(); assert(!held); }
static void sample(uint32_t at,uint8_t id,uint8_t address)
{
    tick(at);
    assert(m1_state.barometer.ready && m1_state.barometer.received);
    assert(m1_state.barometer.chip_id==id && m1_state.barometer.address==address);
    assert(m1_state.barometer.pressure_centi_pa==8157697);
    assert(m1_barometer_fresh(&m1_state.barometer,now));
}
static void scd_tick(uint32_t at) { now=at;m1_scd40_tick();assert(!held); }
static void pressure(uint32_t value,uint32_t at)
{
    m1_state.barometer=(barometer_state_t){.ready=true,.received=true,
        .pressure_centi_pa=value,.updated_ms=at};
}
static void scd_driver_tests(void)
{
    m1_state.barometer=(barometer_state_t){0};
    scd_tick(200000);scd_tick(200499);assert(!scd_pending);
    scd_tick(200500);assert(scd_pending==0x3682);
    scd_tick(200501);assert(scd_pending==0x3682);
    scd_tick(200502);assert(m1_state.scd40.ready && !strcmp(m1_state.scd40.serial,"1234abcd0102"));
    scd_tick(200502);assert(scd_pressure==1013 && pressure_writes==1);
    scd_tick(200502);assert(pressure_writes==1 && !m1_state.scd40.received);
    /* The real Bosch production driver produces the pressure used by SCD40.
     * Run the same SCD-then-BMP owner order as M1/uart.c. */
    attach(0x76,0x50);scd_tick(200503);m1_barometer_tick();
    scd_tick(201503);m1_barometer_tick();
    assert(m1_state.barometer.pressure_centi_pa==8157697);
    scd_tick(201504);assert(scd_pressure==815 && pressure_writes==2);
    pressure(7000000,201505);scd_tick(201505);assert(scd_pressure==700);
    pressure(12000000,201505);unsigned n=pressure_writes;
    scd_tick(201505);assert(pressure_writes==n); /* Full 1 ms command wait. */
    scd_tick(201506);assert(scd_pressure==1200);
    pressure(6999999,201507);scd_tick(201507);assert(scd_pressure==1013);
    n=pressure_writes;pressure(12000001,201508);scd_tick(201508);assert(pressure_writes==n);
    pressure(9870000,201509);scd_tick(201509);assert(scd_pressure==987);
    m1_state.barometer.ready=false;scd_tick(201510);assert(scd_pressure==1013);
    n=pressure_writes;pressure(9500000,191512);scd_tick(201511);assert(scd_pressure==950);
    scd_tick(201512);assert(scd_pressure==1013 && pressure_writes==n+2); /* Age exactly 10 s. */
    pressure(9879999,201513);scd_tick(201513);assert(scd_pressure==987);
    n=pressure_writes;pressure(9870000,201514);scd_tick(201514);assert(pressure_writes==n);
    m1_state.barometer.received=false;scd_tick(201515);assert(scd_pressure==1013);
    /* No pressure command may interrupt a pending response. */
    scd_tick(205502);assert(scd_pending==0xe4b8);
    pressure(7000000,205503);n=pressure_writes;scd_tick(205503);assert(pressure_writes==n);
    scd_tick(205504);assert(scd_pending==0xec05 && pressure_writes==n);
    scd_tick(205505);assert(!m1_state.scd40.received);
    scd_tick(205506);assert(m1_state.scd40.samples==1 && m1_state.scd40.co2_ppm==800);
    assert(m1_state.scd40.temperature_milli_c==25000 && m1_state.scd40.humidity_milli_pct==50000);
    scd_tick(205507);assert(scd_pressure==700 && pressure_writes==n+1);
    /* A failed write is never cached as applied; recovery resends it. */
    pressure(9000000,205508);scd_fail_command=0xe000;n=pressure_writes;
    scd_tick(205508);assert(!m1_state.scd40.ready && m1_state.scd40.errors==1);
    assert(m1_state.scd40.last_command==0xe000 && m1_state.scd40.last_error==kTimeoutErr);
    assert(pressure_writes==n && pressure_attempts==n+1 && scd_pressure==700);
    scd_fail_command=0;scd_tick(210507);assert(!scd_finalizes);
    scd_tick(210508);assert(scd_finalizes==1);
    scd_tick(211008);scd_tick(211010);scd_tick(211011);
    assert(m1_state.scd40.ready && scd_pressure==900 && pressure_writes==n+1);
    /* Bad response CRC cannot publish a sample. Reinit reapplies even an
     * unchanged pressure after an MCU/sensor recovery. */
    pressure(9000000,216009);
    scd_tick(216010);scd_crc_bad=true;scd_tick(216012);scd_crc_bad=false;
    assert(!m1_state.scd40.ready && m1_state.scd40.crc_errors==1 && m1_state.scd40.samples==1);
    scd_tick(221012);scd_tick(221512);scd_tick(221514);
    pressure(9000000,221514);n=pressure_writes;scd_tick(221515);
    assert(m1_state.scd40.ready && pressure_writes==n+1 && scd_pressure==900);
    scd_tick(226514);scd_tick(226516);scd_tick(226518);
    assert(m1_state.scd40.samples==2 && m1_state.scd40.updated_ms==226518);
    puts("SCD40 production driver: Bosch pressure, limits/default/stale/recovery, CRC, command waits and shared-owner scheduling PASS");
}
int main(void)
{
    assert(!m1_barometer_model(0) && !m1_barometer_model(0x58));
    assert(!strcmp(m1_barometer_model(0x50),"BMP388"));
    assert(!strcmp(m1_barometer_model(0x60),"BMP390"));
    tick(2999); assert(!reads && !m1_state.barometer.attempts);
    tick(3000); assert(!m1_state.barometer.ready && m1_state.barometer.errors==1);
    tick(12000); assert(m1_state.barometer.attempts==1); /* Bounded absent-device retries. */
    attach(0x76,0x58); attach(0x77,0x60);
    fail_combined=true; /* Exercise ROM STOP-separated fallback. */
    tick(13000);
    assert(m1_state.barometer.ready && !m1_state.barometer.received);
    assert(!writes[0] && writes[1] && resets==1 && calibration_reads==1);
    sample(14000,0x60,0x77);
    assert(m1_barometer_fresh(&m1_state.barometer,m1_state.barometer.updated_ms+9999));
    assert(!m1_barometer_fresh(&m1_state.barometer,m1_state.barometer.updated_ms+10000));
    present[1]=false;
    tick(15000); assert(!m1_state.barometer.ready && !m1_barometer_fresh(&m1_state.barometer,now));
    fail_combined=false;
    attach(0x76,0x50);
    tick(25000); assert(m1_state.barometer.ready && !m1_state.barometer.received);
    sample(26000,0x50,0x76);
    registers[0][BMP3_REG_SENS_STATUS]=0x10; /* Present but ADC never ready. */
    tick(36000); assert(!m1_state.barometer.ready);
    registers[0][BMP3_REG_SENS_STATUS]=0x70;
    tick(46000); sample(47000,0x50,0x76);
    /* An MCU-only restart must reset a sensor left in normal mode. */
    assert(registers[0][BMP3_REG_PWR_CTRL]==0x33);
    present[0]=false; tick(48000);
    present[0]=true; tick(58000); sample(59000,0x50,0x76);
    assert(resets==4 && calibration_reads==4);
    /* Warning/clamped temperature must never publish a fresh pressure. */
    memset(registers[0]+BMP3_REG_DATA+3,0,3);
    unsigned samples=m1_state.barometer.samples;
    tick(60000); assert(!m1_state.barometer.ready && m1_state.barometer.samples==samples);
    present[0]=present[1]=false; tick(70000);
    assert(!m1_state.barometer.chip_id && !m1_state.barometer.address);
    fail_initialize=true; tick(80000);
    assert(!m1_state.barometer.ready && m1_state.barometer.last_error==kTimeoutErr);
    fail_initialize=false;
    uint32_t at=90000;
    for(unsigned address=0x76;address<=0x77;address++)for(unsigned id=0x50;id<=0x60;id+=0x10) {
        present[0]=present[1]=false;
        attach(address,id);tick(at);sample(at+1000,id,address);
        present[address-0x76]=false;tick(at+2000);at+=12000;
    }
    attach(0x76,0x50);registers[0][BMP3_REG_ODR]=29;
    tick(at);assert(!m1_state.barometer.ready); /* Invalid ODR cannot index outside vendor table. */
    attach(0x76,0x50);tick(at+10000);sample(at+11000,0x50,0x76);
    registers[0][BMP3_REG_ERR]=1;samples=m1_state.barometer.samples;
    tick(at+12000);assert(!m1_state.barometer.ready && m1_state.barometer.samples==samples);
    barometer_state_t wrap={.ready=true,.received=true,.updated_ms=UINT32_MAX-100};
    assert(m1_barometer_fresh(&wrap,50));
    assert(!m1_barometer_fresh(&wrap,10000));
    scd_driver_tests();
    puts("BMP388/BMP390: IDs/addresses, no-device/unknown, fallback, trim/ADC compensation, loss/recovery, stale/wrap and warnings PASS");
}
