#include "version.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/sched.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
#include <asm/system.h>
#endif
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>

#include "pt3_com.h"
#include "pt3_pci.h"
#include "pt3_mx.h"

typedef struct _SHF_TYPE {
	__u32	freq;		// Channel center frequency
	__u32	freq_th;	// Offset frequency threshold
	__u8	shf_val;	// Spur shift value
	__u8	shf_dir;	// Spur shift direction
} SHF_TYPE;

static SHF_TYPE SHF_DVBT_TAB[] = {
	// { Freq(kHz),Offset(kHz), Val,	Dir,	type},
	{  64500, 500, 0x92, 0x07 },
	{ 191500, 300, 0xE2, 0x07 },
	{ 205500, 500, 0x2C, 0x04 },
	{ 212500, 500, 0x1E, 0x04 },
	{ 226500, 500, 0xD4, 0x07 },
	{  99143, 500, 0x9C, 0x07 },
	{ 173143, 500, 0xD4, 0x07 },
	{ 191143, 300, 0xD4, 0x07 },
	{ 207143, 500, 0xCE, 0x07 },
	{ 225143, 500, 0xCE, 0x07 },
	{ 243143, 500, 0xD4, 0x07 },
	{ 261143, 500, 0xD4, 0x07 },
	{ 291143, 500, 0xD4, 0x07 },
	{ 339143, 500, 0x2C, 0x04 },
	{ 117143, 500, 0x7A, 0x07 },
	{ 135143, 300, 0x7A, 0x07 },
	{ 153143, 500, 0x01, 0x07 }
};

static __u8 mx_address[MAX_TUNER] = { 0x62, 0x61 };

static void
mx_write(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx, __u8 *data, size_t size)
{
	pt3_tc_write_tuner_without_addr(bus, tc, data, size);
}

static void
mx_rftune(__u8 *data, __u32 *size, __u32 freq)
{
	__u32 dig_rf_freq ,temp ,frac_divider, khz, mhz, i;
	__u8 rf_data[] = {
		0x13, 0x00,		// abort tune
		0x3B, 0xC0,
		0x3B, 0x80,
		0x10, 0x95,		// BW
		0x1A, 0x05,
		0x61, 0x00,
		0x62, 0xA0,
		0x11, 0x40,		// 2 bytes to store RF frequency
		0x12, 0x0E,		// 2 bytes to store RF frequency
		0x13, 0x01		// start tune
	};

	dig_rf_freq = 0;
	temp = 0;
	frac_divider = 1000000;
	khz = 1000;
	mhz = 1000000;

	dig_rf_freq = freq / mhz;
	temp = freq % mhz;

	for (i = 0; i < 6; i++) {
		dig_rf_freq <<= 1;
		frac_divider /= 2;
		if (temp > frac_divider) {
			temp -= frac_divider;
			dig_rf_freq++;
		}
	}

	if (temp > 7812)
		dig_rf_freq++;

	rf_data[2 * (7) + 1] = (__u8)(dig_rf_freq);
	rf_data[2 * (8) + 1] = (__u8)(dig_rf_freq >> 8);

	for (i = 0; i < sizeof(SHF_DVBT_TAB)/sizeof(*SHF_DVBT_TAB); i++) {
		if ( (freq >= (SHF_DVBT_TAB[i].freq - SHF_DVBT_TAB[i].freq_th) * khz) &&
				(freq <= (SHF_DVBT_TAB[i].freq + SHF_DVBT_TAB[i].freq_th) * khz) ) {
			rf_data[2 * (5) + 1] = SHF_DVBT_TAB[i].shf_val;
			rf_data[2 * (6) + 1] = 0xa0 | SHF_DVBT_TAB[i].shf_dir;
			break;
		}
	}

	memcpy(data, rf_data, sizeof(rf_data));

	*size = sizeof(rf_data);
}

static void
mx_set_register(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx, __u8 addr, __u8 value)
{
	__u8 data[2];

	data[0] = addr;
	data[1] = value;
	
	mx_write(bus, tc, mx, data, sizeof(data));
}

static void
mx_idac_setting(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx)
{
	__u8 data[] = {
		0x0D, 0x00,
		0x0C, 0x67,
		0x6F, 0x89,
		0x70, 0x0C,
		0x6F, 0x8A,
		0x70, 0x0E,
		0x6F, 0x8B,
		0x70, 0x10+12,
	};

	mx_write(bus, tc, mx, data, sizeof(data));
}

static void
mx_tuner_rftune(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx, __u32 freq)
{
	__u8 data[100];
	__u32 size;

	size = 0;
	mx->freq = freq;

	mx_rftune(data, &size, freq);

	mx_write(bus, tc, mx, data, 14);

	schedule_timeout_interruptible(msecs_to_jiffies(1));	

	mx_write(bus, tc, mx, data + 14, 6);

	schedule_timeout_interruptible(msecs_to_jiffies(1));	
	schedule_timeout_interruptible(msecs_to_jiffies(30));	

	mx_set_register(bus, tc, mx, 0x1a, 0x0d);

	mx_idac_setting(bus, tc, mx);
}

static void
mx_standby(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx)
{
	__u8 data[4];

	data[0] = 0x01;
	data[1] = 0x00;
	data[2] = 0x13;
	data[3] = 0x00;

	mx_write(bus, tc, mx, data, sizeof(data));
}

static void
mx_wakeup(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx)
{
	__u8 data[2];

	data[0] = 0x01;
	data[1] = 0x01;

	mx_write(bus, tc, mx, data, sizeof(data));

	mx_tuner_rftune(bus, tc, mx, mx->freq);
}

static STATUS
mx_set_sleep_mode(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx, int sleep)
{
	STATUS status;
	status = 0;

	if (sleep) {
		mx_standby(bus, tc, mx);
	} else {
		mx_wakeup(bus, tc, mx);
	}

	return status;
}

__u8
pt3_mx_address(__u32 index)
{
	return mx_address[index];
}

STATUS
pt3_mx_set_sleep(PT3_I2C_BUS *bus, PT3_TC *tc, PT3_MX *mx, int sleep)
{
	STATUS status;
	PT3_TS_PIN_MODE mode;
	PT3_TS_PINS_MODE pins;

	mode = sleep ? PT3_TS_PIN_MODE_LOW : PT3_TS_PIN_MODE_NORMAL;
	pins.clock_data = mode;
	pins.byte = mode;
	pins.valid = mode;

	if (sleep) {
		status = pt3_tc_set_agc_t(bus, tc, PT3_TC_AGC_MANUAL);
		if (status)
			return status;
		mx_set_sleep_mode(bus, tc, mx, sleep);
		pt3_tc_write_slptim(bus, tc, sleep);
		pt3_tc_set_ts_pins_mode_t(bus, tc, &pins);
	} else {
		pt3_tc_set_ts_pins_mode_t(bus, tc, &pins);
		pt3_tc_write_slptim(bus, tc, sleep);
		mx_set_sleep_mode(bus, tc, mx, sleep);
	}
	
	mx->sleep = sleep;

	return STATUS_OK;
}

PT3_MX *
create_pt3_mx()
{
	PT3_MX *mx;

	mx = NULL;

	mx = vzalloc(sizeof(PT3_MX));
	if (mx == NULL)
		goto fail;

	mx->sleep = 1;
	
	return mx;
fail:
	if (mx != NULL)
		vfree(mx);
	return NULL;
}

void
free_pt3_mx(PT3_MX *mx)
{
	vfree(mx);
}
