// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * bc250_metrics_fix - SMU telemetry fix for the BC-250 (Cyan Skillfish),
 * loaded on top of the running amdgpu.
 *
 * SMU firmware with the eight-core metrics patch (metrics-8core.s from
 * https://github.com/rw-r-r-0644/bc250-smu-unlock) widens every per-core
 * array of the metrics table to eight entries; stock amdgpu decodes it with
 * the six-core layout. This module replaces five Cyan Skillfish pptable
 * callbacks with ones using the right layout, widens the sclk range of
 * pp_od_clk_voltage, and exports the table under
 * /sys/module/bc250_metrics_fix/telemetry (with hwmon=1 also as a "bc250"
 * hwmon device).
 *
 * Nothing is hooked unless the callbacks are the stock ones and amdgpu's
 * structure layout matches the build headers. Semantics follow the
 * bc250-smu-port adapter of bc250-smu-metrics (from v6-alpha3.4 on).
 */

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/kallsyms.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/topology.h>
#include <linux/uaccess.h>

#include "amdgpu.h"
#include "amdgpu_smu.h"
#include "smu11_driver_if_cyan_skillfish.h"
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"

#define BC250_VERSION "1.0"

/* --- Parameters --- */

static int layout;
module_param(layout, int, 0444);
MODULE_PARM_DESC(layout, "SMU metrics layout: 0 = auto (from the table, else the core count), 6, 8");

static bool hook = true;
module_param(hook, bool, 0444);
MODULE_PARM_DESC(hook, "Fix amdgpu's own hwmon/gpu_metrics/pp_dpm/gpu_busy_percent (0 = only export the table)");

static bool hwmon_enable;
module_param_named(hwmon, hwmon_enable, bool, 0444);
MODULE_PARM_DESC(hwmon, "Register the bc250 hwmon device with every reading as a sensor (default 0: telemetry directory only)");

static uint sclk_min = 350;
module_param(sclk_min, uint, 0444);
MODULE_PARM_DESC(sclk_min, "Lowest GFX clock accepted through pp_od_clk_voltage, MHz (stock 1000)");

static uint sclk_max = 2230;
module_param(sclk_max, uint, 0444);
MODULE_PARM_DESC(sclk_max, "Highest GFX clock accepted through pp_od_clk_voltage, MHz (stock 2000)");

static uint sclk_default;
module_param(sclk_default, uint, 0444);
MODULE_PARM_DESC(sclk_default, "GFX clock restored through pp_od_clk_voltage 'r', MHz (0 = the clock at load; reads back the value in use)");


/* Stock Cyan Skillfish voltage limits, unit mV */
#define CYAN_SKILLFISH_VDDC_MIN		700
#define CYAN_SKILLFISH_VDDC_MAX		1129
#define CYAN_SKILLFISH_VDDC_MAGIC	5118	/* "unforce" */

/* --- Firmware layout --- */

/*
 * Eight-core layout: metrics-8core.s widens every per-core array to eight,
 * which moves the tail of the table:
 *
 *   field                 six-core   eight-core   firmware store
 *   CorePower             0x0c       0x10         s32i.n a12, a13, 0x10
 *   CoreTemperature       0x24       0x30         s16i   a11, a10, 0x30
 *   C0Residency           0x38       0x48         s16i   a8,  a10, 0x48
 *   GfxclkFrequency       0x44       0x58         s16i   a14, a2,  0x58
 *   CurrentSocketPower    0x68       0x7c         s32i   a8,  a2,  0x7c
 */
typedef struct {
	uint16_t CoreFrequency[8];		/* 0x00 */
	uint32_t CorePower[8];			/* 0x10 */
	uint16_t CoreTemperature[8];		/* 0x30 */
	uint16_t L3Frequency[2];		/* 0x40 */
	uint16_t L3Temperature[2];		/* 0x44 */
	uint16_t C0Residency[8];		/* 0x48 */
	uint16_t GfxclkFrequency;		/* 0x58 */
	uint16_t GfxTemperature;		/* 0x5a */
	uint16_t SocclkFrequency;		/* 0x5c */
	uint16_t VclkFrequency;			/* 0x5e */
	uint16_t DclkFrequency;			/* 0x60 */
	uint16_t MemclkFrequency;		/* 0x62 */
	uint32_t Voltage[2];			/* 0x64 */
	uint32_t Current[2];			/* 0x6c */
	uint32_t Power[2];			/* 0x74 */
	uint32_t CurrentSocketPower;		/* 0x7c */
	uint16_t SocTemperature;		/* 0x80 */
	uint16_t EdgeTemperature;		/* 0x82 */
	uint16_t ThrottlerStatus;		/* 0x84 */
	uint16_t Spare;				/* 0x86 */
} bc250_table8_t;

static_assert(offsetof(bc250_table8_t, C0Residency) == 0x48);
static_assert(offsetof(bc250_table8_t, GfxclkFrequency) == 0x58);
static_assert(offsetof(bc250_table8_t, CurrentSocketPower) == 0x7c);
static_assert(sizeof(bc250_table8_t) == 0x88);
/* Current fits the stock 244-byte transfer; Average is served from it. */
static_assert(sizeof(bc250_table8_t) <= sizeof(SmuMetrics_t));

/* Both layouts decoded into one view. */
struct bc250_view {
	uint16_t core_freq[8];
	uint32_t core_power[8];
	uint16_t core_temp[8];
	uint16_t c0[8];
	uint16_t l3_freq[2];
	uint16_t l3_temp[2];
	uint16_t gfxclk, gfx_temp, socclk, vclk, dclk, memclk;
	uint32_t volt[2], curr[2], power[2], socket;
	uint16_t soc_temp, edge_temp, throttle;
	uint8_t ncores;
};

#define BC250_COPY_VIEW(v, t, n)					\
	do {								\
		int _i;							\
		for (_i = 0; _i < (n); _i++) {				\
			(v)->core_freq[_i] = (t)->CoreFrequency[_i];	\
			(v)->core_power[_i] = (t)->CorePower[_i];	\
			(v)->core_temp[_i] = (t)->CoreTemperature[_i];	\
			(v)->c0[_i] = (t)->C0Residency[_i];		\
		}							\
		for (_i = 0; _i < 2; _i++) {				\
			(v)->l3_freq[_i] = (t)->L3Frequency[_i];	\
			(v)->l3_temp[_i] = (t)->L3Temperature[_i];	\
			(v)->volt[_i] = (t)->Voltage[_i];		\
			(v)->curr[_i] = (t)->Current[_i];		\
			(v)->power[_i] = (t)->Power[_i];		\
		}							\
		(v)->gfxclk = (t)->GfxclkFrequency;			\
		(v)->gfx_temp = (t)->GfxTemperature;			\
		(v)->socclk = (t)->SocclkFrequency;			\
		(v)->vclk = (t)->VclkFrequency;				\
		(v)->dclk = (t)->DclkFrequency;				\
		(v)->memclk = (t)->MemclkFrequency;			\
		(v)->socket = (t)->CurrentSocketPower;			\
		(v)->soc_temp = (t)->SocTemperature;			\
		(v)->edge_temp = (t)->EdgeTemperature;			\
		(v)->throttle = (t)->ThrottlerStatus;			\
		(v)->ncores = (n);					\
	} while (0)

/* --- State --- */

static struct {
	struct pci_dev *pdev;
	struct amdgpu_device *adev;
	struct smu_context *smu;
	bool eight;
	uint8_t ncores;			/* shown: the table's width, at most the cores present */

	const struct pptable_funcs *orig;
	struct pptable_funcs *ours;	/* see bc250_unhook() */
	bool hooked;
	bool driver_fixed;		/* amdgpu already answers GPU_LOAD */

	uint32_t user_sclk, user_vddc;	/* pending pp_od_clk_voltage edit */

	struct device *hwmon;
	struct kobject *telemetry;	/* /sys/module/<mod>/telemetry */

	/* Table snapshot; protected by adev->pm.mutex. */
	unsigned long snap_time;	/* jiffies */
	bool snap_valid;
	union {
		SmuMetrics_t six;
		bc250_table8_t eight;	/* the Current table */
		u8 bytes[sizeof(SmuMetrics_t)];
	} raw;

	void __iomem *grbm_status;	/* NULL if outside the MMIO range */
} bc;

/* --- Table access (all callers hold adev->pm.mutex) --- */

static bool bc250_smu_usable(void)
{
	return bc.smu->pm_enabled && bc.adev->pm.dpm_enabled &&
	       !bc.adev->in_suspend;
}

/* The table amdgpu copied out of the SMU on its last export. */
static void bc250_capture(void)
{
	memcpy(bc.raw.bytes, bc.smu->smu_table.metrics_table, sizeof(SmuMetrics_t));
	bc.snap_time = jiffies;
	bc.snap_valid = true;
}

static int bc250_refresh(void)
{
	uint32_t val, size = sizeof(val);
	int ret;

	if (!bc250_smu_usable())
		return -EBUSY;

	/* GFX_SCLK makes amdgpu export the table from the SMU (1 ms debounce). */
	ret = bc.orig->read_sensor(bc.smu, AMDGPU_PP_SENSOR_GFX_SCLK, &val, &size);
	if (ret)
		return ret;

	bc250_capture();
	return 0;
}

/*
 * The module's own readers share one snapshot: a `sensors` run would
 * otherwise transfer the table once per attribute. amdgpu's own paths
 * refresh on every call, as in stock.
 */
#define BC250_SNAPSHOT_MS	100

static int bc250_refresh_shared(void)
{
	if (!bc250_smu_usable())
		return -EBUSY;
	if (bc.snap_valid &&
	    time_before(jiffies, bc.snap_time + msecs_to_jiffies(BC250_SNAPSHOT_MS)))
		return 0;
	return bc250_refresh();
}

static void bc250_decode(bool average, struct bc250_view *v)
{
	memset(v, 0, sizeof(*v));

	/* Eight-core: the Average table lies past the stock transfer. */
	if (bc.eight)
		BC250_COPY_VIEW(v, &bc.raw.eight, 8);
	else if (average)
		BC250_COPY_VIEW(v, &bc.raw.six.Average, 6);
	else
		BC250_COPY_VIEW(v, &bc.raw.six.Current, 6);
	v->ncores = min(v->ncores, bc.ncores);
}

static int bc250_view(bool average, struct bc250_view *v)
{
	int ret = bc250_refresh();

	if (ret)
		return ret;
	bc250_decode(average, v);
	return 0;
}

/*
 * GFX activity. The metrics table has no activity field, so a soft hrtimer
 * samples GRBM_STATUS.GUI_ACTIVE every CS_ACT_PERIOD_US into a window that
 * reads average. The timer runs while read and stops CS_ACT_KEEPALIVE_MS
 * after the last read; the next read is answered from a short burst.
 * All-ones reads (block powered down) are skipped.
 */
#define CS_ACT_PERIOD_US	2000
#define CS_ACT_SLACK_US		500
#define CS_ACT_WINDOW		128	/* samples, a power of two */
#define CS_ACT_KEEPALIVE_MS	5000
#define CS_ACT_SEED_SAMPLES	16
#define CS_ACT_SEED_DELAY_US	10

enum { ACT_IDLE, ACT_BUSY, ACT_UNREADABLE };

static struct {
	spinlock_t lock;
	struct hrtimer timer;
	bool running;
	unsigned long deadline;		/* jiffies */
	u8 sample[CS_ACT_WINDOW];
	unsigned int pos, filled, busy, valid;
	uint16_t seed;			/* answer until the window fills */
} act;

static int bc250_grbm_sample(void)
{
	uint32_t reg;

	/* The timer can run into a suspend. */
	if (READ_ONCE(bc.adev->in_suspend) || READ_ONCE(bc.adev->no_hw_access))
		return ACT_UNREADABLE;
	reg = readl(bc.grbm_status);
	if (reg == 0xffffffff)
		return ACT_UNREADABLE;
	return reg & GRBM_STATUS__GUI_ACTIVE_MASK ? ACT_BUSY : ACT_IDLE;
}

/* Activity in hundredths of a percent, as gpu_metrics carries it. */
static uint16_t bc250_act_ratio(unsigned int busy, unsigned int valid)
{
	return valid ? DIV_ROUND_CLOSEST(busy * 10000, valid) : 0;
}

static enum hrtimer_restart bc250_act_tick(struct hrtimer *t)
{
	int s = bc250_grbm_sample();
	u8 old;

	spin_lock(&act.lock);
	if (act.filled == CS_ACT_WINDOW) {
		old = act.sample[act.pos];
		act.valid -= old != ACT_UNREADABLE;
		act.busy -= old == ACT_BUSY;
	} else {
		act.filled++;
	}
	act.sample[act.pos] = s;
	act.valid += s != ACT_UNREADABLE;
	act.busy += s == ACT_BUSY;
	act.pos = (act.pos + 1) & (CS_ACT_WINDOW - 1);
	if (time_after(jiffies, act.deadline)) {
		act.running = false;
		spin_unlock(&act.lock);
		return HRTIMER_NORESTART;
	}
	spin_unlock(&act.lock);

	hrtimer_forward_now(t, us_to_ktime(CS_ACT_PERIOD_US));
	hrtimer_set_expires_range_ns(t, hrtimer_get_softexpires(t),
				     CS_ACT_SLACK_US * NSEC_PER_USEC);
	return HRTIMER_RESTART;
}

/* Burst for the first read after the timer stopped. */
static uint16_t bc250_act_seed(void)
{
	unsigned int busy = 0, valid = 0;
	int i, s;

	for (i = 0; i < CS_ACT_SEED_SAMPLES; i++) {
		s = bc250_grbm_sample();
		valid += s != ACT_UNREADABLE;
		busy += s == ACT_BUSY;
		if (i != CS_ACT_SEED_SAMPLES - 1)
			udelay(CS_ACT_SEED_DELAY_US);
	}
	return bc250_act_ratio(busy, valid);
}

static uint16_t bc250_activity(void)
{
	unsigned int busy, valid, filled;
	uint16_t seed;
	bool start;

	if (!bc.grbm_status)
		return 0;

	spin_lock_bh(&act.lock);
	act.deadline = jiffies + msecs_to_jiffies(CS_ACT_KEEPALIVE_MS);
	start = !act.running;
	if (start) {
		act.running = true;
		act.pos = act.filled = act.busy = act.valid = 0;
	}
	busy = act.busy;
	valid = act.valid;
	filled = act.filled;
	seed = act.seed;
	spin_unlock_bh(&act.lock);

	if (start) {
		seed = bc250_act_seed();
		spin_lock_bh(&act.lock);
		act.seed = seed;
		spin_unlock_bh(&act.lock);
		hrtimer_start_range_ns(&act.timer, us_to_ktime(CS_ACT_PERIOD_US),
				       CS_ACT_SLACK_US * NSEC_PER_USEC, HRTIMER_MODE_REL_SOFT);
		return seed;
	}
	if (filled < CS_ACT_SEED_SAMPLES)
		return seed;
	return bc250_act_ratio(busy, valid);
}

static void bc250_act_init(void)
{
	struct amdgpu_device *adev = bc.adev;
	uint32_t off;

	spin_lock_init(&act.lock);
	hrtimer_setup(&act.timer, bc250_act_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);

	/* RREG32_SOC15(GC, 0, mmGRBM_STATUS), as a plain MMIO address. */
	off = (adev->reg_offset[GC_HWIP][0][mmGRBM_STATUS_BASE_IDX] + mmGRBM_STATUS) << 2;
	bc.grbm_status = off + 4 <= adev->rmmio_size ? adev->rmmio + off : NULL;
}

/* --- Human-readable table (telemetry and hwmon "summary") --- */

static int bc250_emit_summary(const struct bc250_view *m, char *buf, int size)
{
	int i;

	size += sysfs_emit_at(buf, size, "layout: %s\n\n", bc.eight ? "8-core" : "6-core");
	size += sysfs_emit_at(buf, size, "core     clk     power    temp    C0%%\n");
	for (i = 0; i < m->ncores; i++)
		size += sysfs_emit_at(buf, size,
				      "core %d:  %4uMHz  %3u.%02uW  %3u.%02uC  %3u\n",
				      i, m->core_freq[i],
				      m->core_power[i] / 1000, (m->core_power[i] % 1000) / 10,
				      m->core_temp[i] / 100, m->core_temp[i] % 100,
				      m->c0[i]);
	size += sysfs_emit_at(buf, size, "\n");
	for (i = 0; i < 2; i++)
		size += sysfs_emit_at(buf, size, "L3 #%d:   %4uMHz          %3u.%02uC\n",
				      i, m->l3_freq[i], m->l3_temp[i] / 100, m->l3_temp[i] % 100);
	size += sysfs_emit_at(buf, size, "\n");
	size += sysfs_emit_at(buf, size, "gfx:     %4uMHz          %3u.%02uC\n",
			      m->gfxclk, m->gfx_temp / 100, m->gfx_temp % 100);
	size += sysfs_emit_at(buf, size, "soc:     %4uMHz          %3u.%02uC\n",
			      m->socclk, m->soc_temp / 100, m->soc_temp % 100);
	size += sysfs_emit_at(buf, size, "vclk:    %4uMHz\n", m->vclk);
	size += sysfs_emit_at(buf, size, "dclk:    %4uMHz\n", m->dclk);
	size += sysfs_emit_at(buf, size, "mem:     %4uMHz\n", m->memclk);
	size += sysfs_emit_at(buf, size, "edge:                    %3u.%02uC\n",
			      m->edge_temp / 100, m->edge_temp % 100);
	size += sysfs_emit_at(buf, size, "\n");
	for (i = 0; i < 2; i++)
		size += sysfs_emit_at(buf, size, "pd %s:  %4umV  %3u.%02uA  %3u.%02uW\n",
				      i ? "gpu" : "cpu", m->volt[i],
				      m->curr[i] / 1000, (m->curr[i] % 1000) / 10,
				      m->power[i] / 1000, (m->power[i] % 1000) / 10);
	size += sysfs_emit_at(buf, size, "pd socket:         %3u.%02uW\n",
			      m->socket / 1000, (m->socket % 1000) / 10);
	size += sysfs_emit_at(buf, size, "\nthrottle: 0x%x\n", m->throttle);
	return size;
}

/* --- Replacement amdgpu callbacks (called with adev->pm.mutex held) --- */

static int bc250_read_sensor(struct smu_context *smu, enum amd_pp_sensors sensor,
			     void *data, uint32_t *size)
{
	uint32_t *out = data;
	struct bc250_view v;
	int ret;

	if (smu != bc.smu)
		return bc.orig->read_sensor(smu, sensor, data, size);
	if (!data || !size)
		return -EINVAL;

	switch (sensor) {
	case AMDGPU_PP_SENSOR_GPU_LOAD:
		*out = bc250_activity() / 100;
		*size = 4;
		return 0;
	case AMDGPU_PP_SENSOR_GFX_SCLK:
	case AMDGPU_PP_SENSOR_GFX_MCLK:
	case AMDGPU_PP_SENSOR_GPU_AVG_POWER:
	case AMDGPU_PP_SENSOR_GPU_INPUT_POWER:
	case AMDGPU_PP_SENSOR_HOTSPOT_TEMP:
	case AMDGPU_PP_SENSOR_EDGE_TEMP:
	case AMDGPU_PP_SENSOR_VDDNB:
	case AMDGPU_PP_SENSOR_VDDGFX:
		break;
	default:
		return bc.orig->read_sensor(smu, sensor, data, size);
	}

	ret = bc250_view(sensor == AMDGPU_PP_SENSOR_GPU_AVG_POWER, &v);
	if (ret)
		return ret;

	switch (sensor) {
	case AMDGPU_PP_SENSOR_GFX_SCLK:
		*out = v.gfxclk * 100;
		break;
	case AMDGPU_PP_SENSOR_GFX_MCLK:
		*out = v.memclk * 100;
		break;
	case AMDGPU_PP_SENSOR_GPU_AVG_POWER:
	case AMDGPU_PP_SENSOR_GPU_INPUT_POWER:
		*out = v.socket;
		break;
	case AMDGPU_PP_SENSOR_HOTSPOT_TEMP:
		*out = v.soc_temp / 100 * SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case AMDGPU_PP_SENSOR_EDGE_TEMP:
		*out = v.gfx_temp / 100 * SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case AMDGPU_PP_SENSOR_VDDNB:
		*out = v.volt[0];
		break;
	default: /* AMDGPU_PP_SENSOR_VDDGFX */
		*out = v.volt[1];
		break;
	}
	*size = 4;
	return 0;
}

/* gpu_metrics power fields are 16-bit with 0xffff meaning "unsupported". */
static uint16_t bc250_u16_power(uint32_t mw)
{
	return min_t(uint32_t, mw, U16_MAX - 1);
}

static ssize_t bc250_get_gpu_metrics(struct smu_context *smu, void **table)
{
	struct gpu_metrics_v2_2 *gm;
	struct bc250_view cur, avg;
	ssize_t ret;
	int i;

	if (smu != bc.smu)
		return bc.orig->get_gpu_metrics(smu, table);

	/* Stock fetches a fresh table and fills the header; redo the values. */
	ret = bc.orig->get_gpu_metrics(smu, table);
	if (ret < (ssize_t)sizeof(*gm))
		return ret;
	gm = *table;

	bc250_capture();
	bc250_decode(false, &cur);
	bc250_decode(true, &avg);

	gm->temperature_gfx = cur.gfx_temp;
	gm->temperature_soc = cur.soc_temp;
	gm->average_gfx_activity = bc250_activity();

	/* Power[0] is VDDCR_VDD (CPU), Power[1] VDDCR_GFX; no separate SoC rail. */
	gm->average_socket_power = bc250_u16_power(cur.socket);
	gm->average_cpu_power = bc250_u16_power(cur.power[0]);
	gm->average_soc_power = U16_MAX;
	gm->average_gfx_power = bc250_u16_power(cur.power[1]);

	gm->average_gfxclk_frequency = avg.gfxclk;
	gm->average_socclk_frequency = avg.socclk;
	gm->average_uclk_frequency = avg.memclk;
	gm->average_fclk_frequency = avg.memclk;
	gm->average_vclk_frequency = avg.vclk;
	gm->average_dclk_frequency = avg.dclk;

	gm->current_gfxclk = cur.gfxclk;
	gm->current_socclk = cur.socclk;
	gm->current_uclk = cur.memclk;
	gm->current_fclk = cur.memclk;
	gm->current_vclk = cur.vclk;
	gm->current_dclk = cur.dclk;

	/* Per-core power: Average on six-core, Current on eight-core. */
	for (i = 0; i < 8; i++) {
		bool present = i < cur.ncores;

		gm->current_coreclk[i] = present ? cur.core_freq[i] : U16_MAX;
		gm->temperature_core[i] = present ? cur.core_temp[i] : U16_MAX;
		gm->average_core_power[i] = present ? bc250_u16_power(avg.core_power[i]) : U16_MAX;
	}
	for (i = 0; i < 2; i++) {
		gm->temperature_l3[i] = cur.l3_temp[i];
		gm->current_l3clk[i] = cur.l3_freq[i];
	}
	gm->throttle_status = cur.throttle;

	return ret;
}

static int bc250_emit_clk_levels(struct smu_context *smu, enum smu_clk_type type,
				 char *buf, int *offset)
{
	int size = *offset, start = *offset, lvl;
	struct bc250_view v;
	uint32_t cur;
	int ret;

	if (smu != bc.smu)
		return bc.orig->emit_clk_levels(smu, type, buf, offset);

	switch (type) {
	case SMU_SOCCLK:
	case SMU_FCLK:
	case SMU_MCLK:
	case SMU_VCLK:
	case SMU_DCLK:
	case SMU_SCLK:
	case SMU_GFXCLK:
	case SMU_OD_SCLK:
	case SMU_OD_VDDC_CURVE:
		break;
	case SMU_OD_RANGE:
		size += sysfs_emit_at(buf, size, "OD_RANGE:\n");
		size += sysfs_emit_at(buf, size, "SCLK: %7uMhz %10uMhz\n", sclk_min, sclk_max);
		size += sysfs_emit_at(buf, size, "VDDC: %7umV  %10umV\n",
				      CYAN_SKILLFISH_VDDC_MIN, CYAN_SKILLFISH_VDDC_MAX);
		*offset += size - start;
		return 0;
	default:
		return bc.orig->emit_clk_levels(smu, type, buf, offset);
	}

	ret = bc250_view(false, &v);
	if (ret)
		return ret;

	switch (type) {
	case SMU_SOCCLK:
		size += sysfs_emit_at(buf, size, "0: %uMhz *\n", v.socclk);
		break;
	case SMU_FCLK:
	case SMU_MCLK:
		size += sysfs_emit_at(buf, size, "0: %uMhz *\n", v.memclk);
		break;
	case SMU_VCLK:
		size += sysfs_emit_at(buf, size, "0: %uMhz *\n", v.vclk);
		break;
	case SMU_DCLK:
		size += sysfs_emit_at(buf, size, "0: %uMhz *\n", v.dclk);
		break;
	case SMU_OD_SCLK:
		size += sysfs_emit_at(buf, size, "OD_SCLK:\n0: %uMhz *\n", v.gfxclk);
		break;
	case SMU_OD_VDDC_CURVE:
		size += sysfs_emit_at(buf, size, "OD_VDDC:\n0: %umV *\n", v.volt[1]);
		break;
	default: /* SMU_SCLK, SMU_GFXCLK: same three levels as stock */
		cur = v.gfxclk;
		lvl = cur == sclk_max ? 2 : cur == sclk_min ? 0 : 1;
		size += sysfs_emit_at(buf, size, "0: %uMhz %s\n", sclk_min, lvl == 0 ? "*" : "");
		size += sysfs_emit_at(buf, size, "1: %uMhz %s\n",
				      lvl == 1 ? cur : sclk_default, lvl == 1 ? "*" : "");
		size += sysfs_emit_at(buf, size, "2: %uMhz %s\n", sclk_max, lvl == 2 ? "*" : "");
		break;
	}

	*offset += size - start;
	return 0;
}

/* The path smu_cmn_send_smc_msg_with_param() takes, without importing it. */
static int bc250_smu_msg(enum smu_message_type msg, uint32_t param)
{
	struct smu_msg_ctl *ctl = &bc.smu->msg_ctl;
	struct smu_msg_args args = {
		.msg = msg,
		.num_args = 1,
		.args = { param },
	};

	return ctl->ops->send_msg(ctl, &args);
}

/* Stock od_edit_dpm_table with the sclk_min..sclk_max range. */
static int bc250_od_edit_dpm_table(struct smu_context *smu,
				   enum PP_OD_DPM_TABLE_COMMAND type,
				   long input[], uint32_t size)
{
	struct device *dev = smu->adev->dev;
	int ret;

	if (smu != bc.smu)
		return bc.orig->od_edit_dpm_table(smu, type, input, size);

	switch (type) {
	case PP_OD_EDIT_VDDC_CURVE:
		if (size != 3 || input[0] != 0) {
			dev_err(dev, "Invalid parameter!\n");
			return -EINVAL;
		}
		if (input[1] < sclk_min || input[1] > sclk_max) {
			dev_err(dev, "Invalid sclk! Valid sclk range: %uMHz - %uMhz\n",
				sclk_min, sclk_max);
			return -EINVAL;
		}
		if (input[2] < CYAN_SKILLFISH_VDDC_MIN || input[2] > CYAN_SKILLFISH_VDDC_MAX) {
			dev_err(dev, "Invalid vddc! Valid vddc range: %umV - %umV\n",
				CYAN_SKILLFISH_VDDC_MIN, CYAN_SKILLFISH_VDDC_MAX);
			return -EINVAL;
		}
		bc.user_sclk = input[1];
		bc.user_vddc = input[2];
		return 0;

	case PP_OD_RESTORE_DEFAULT_TABLE:
		if (size != 0) {
			dev_err(dev, "Invalid parameter!\n");
			return -EINVAL;
		}
		bc.user_sclk = sclk_default;
		bc.user_vddc = CYAN_SKILLFISH_VDDC_MAGIC;
		return 0;

	case PP_OD_COMMIT_DPM_TABLE:
		if (size != 0) {
			dev_err(dev, "Invalid parameter!\n");
			return -EINVAL;
		}
		if (bc.user_sclk < sclk_min || bc.user_sclk > sclk_max) {
			dev_err(dev, "Invalid sclk! Valid sclk range: %uMHz - %uMhz\n",
				sclk_min, sclk_max);
			return -EINVAL;
		}
		if (bc.user_vddc != CYAN_SKILLFISH_VDDC_MAGIC &&
		    (bc.user_vddc < CYAN_SKILLFISH_VDDC_MIN ||
		     bc.user_vddc > CYAN_SKILLFISH_VDDC_MAX)) {
			dev_err(dev, "Invalid vddc! Valid vddc range: %umV - %umV\n",
				CYAN_SKILLFISH_VDDC_MIN, CYAN_SKILLFISH_VDDC_MAX);
			return -EINVAL;
		}

		ret = bc250_smu_msg(SMU_MSG_RequestGfxclk, bc.user_sclk);
		if (ret) {
			dev_err(dev, "Set sclk failed!\n");
			return ret;
		}
		if (bc.user_vddc == CYAN_SKILLFISH_VDDC_MAGIC) {
			ret = bc250_smu_msg(SMU_MSG_UnforceGfxVid, 0);
			if (ret)
				dev_err(dev, "Unforce vddc failed!\n");
		} else {
			/* SVI2 VID: vid = (1.55 V - voltage) * 160 */
			ret = bc250_smu_msg(SMU_MSG_ForceGfxVid,
					    (1550 - bc.user_vddc) * 160 / 1000);
			if (ret)
				dev_err(dev, "Force vddc failed!\n");
		}
		return ret;

	default:
		return -EOPNOTSUPP;
	}
}

static int bc250_get_dpm_ultimate_freq(struct smu_context *smu, enum smu_clk_type type,
				       uint32_t *min, uint32_t *max)
{
	struct bc250_view v;
	uint32_t low, high;
	int ret;

	if (smu != bc.smu)
		return bc.orig->get_dpm_ultimate_freq(smu, type, min, max);

	switch (type) {
	case SMU_GFXCLK:
	case SMU_SCLK:
		low = sclk_min;
		high = sclk_max;
		break;
	case SMU_FCLK:
	case SMU_MCLK:
	case SMU_UCLK:
	case SMU_SOCCLK:
	case SMU_VCLK:
	case SMU_DCLK:
		/* Stock reports the current clock as both ends. */
		ret = bc250_view(false, &v);
		if (ret)
			return ret;
		low = high = type == SMU_SOCCLK ? v.socclk :
			     type == SMU_VCLK ? v.vclk :
			     type == SMU_DCLK ? v.dclk : v.memclk;
		break;
	default:
		return bc.orig->get_dpm_ultimate_freq(smu, type, min, max);
	}

	if (min)
		*min = low;
	if (max)
		*max = high;
	return 0;
}

/* --- bc250 hwmon device --- */

enum {
	T_CORE0 = 0, T_L3_0 = 8, T_GFX = 10, T_EDGE, T_SOC, T_COUNT
};
enum {
	P_SOCKET = 0, P_CPU, P_CORE0, P_GFX = P_CORE0 + 8, P_COUNT
};
enum {
	F_CORE0 = 0, F_L3_0 = 8, F_GFX = 10, F_SOC, F_VCLK, F_DCLK, F_MEM, F_COUNT
};

/*
 * Unique labels: GUI monitors list sensors by label alone, without unit.
 * Within each type: total, CPU (rail, cores, L3), GPU (gfx, edge), SoC.
 */
static const char *const bc250_temp_label[T_COUNT] = {
	"core0 temp", "core1 temp", "core2 temp", "core3 temp",
	"core4 temp", "core5 temp", "core6 temp", "core7 temp",
	"L3 0 temp", "L3 1 temp", "gfx temp", "edge temp", "soc temp",
};
static const char *const bc250_power_label[P_COUNT] = {
	"socket power", "cpu rail power",
	"core0 power", "core1 power", "core2 power", "core3 power",
	"core4 power", "core5 power", "core6 power", "core7 power",
	"gfx rail power",
};
static const char *const bc250_volt_label[2] = { "cpu rail voltage", "gfx rail voltage" };
static const char *const bc250_curr_label[2] = { "cpu rail current", "gfx rail current" };
static const char *const bc250_freq_label[F_COUNT] = {
	"core0 clock", "core1 clock", "core2 clock", "core3 clock",
	"core4 clock", "core5 clock", "core6 clock", "core7 clock",
	"L3 0 clock", "L3 1 clock", "gfx clock", "soc clock",
	"vclk", "dclk", "mem clock",
};

static bool bc250_core_channel_present(int core)
{
	return core < bc.ncores;
}

static umode_t bc250_hwmon_visible(const void *data, enum hwmon_sensor_types type,
				   u32 attr, int ch)
{
	if (type == hwmon_temp && ch < T_L3_0 && !bc250_core_channel_present(ch))
		return 0;
	if (type == hwmon_power && ch >= P_CORE0 && ch < P_GFX &&
	    !bc250_core_channel_present(ch - P_CORE0))
		return 0;
	return 0444;
}

static int bc250_locked_view(struct bc250_view *v)
{
	int ret;

	mutex_lock(&bc.adev->pm.mutex);
	ret = bc250_refresh_shared();
	if (!ret)
		bc250_decode(false, v);
	mutex_unlock(&bc.adev->pm.mutex);
	return ret;
}

static int bc250_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int ch, long *val)
{
	struct bc250_view v;
	int ret = bc250_locked_view(&v);

	if (ret)
		return ret;

	switch (type) {
	case hwmon_temp: {	/* centi-degC -> milli-degC */
		uint16_t t = ch < T_L3_0 ? v.core_temp[ch] :
			     ch < T_GFX ? v.l3_temp[ch - T_L3_0] :
			     ch == T_GFX ? v.gfx_temp :
			     ch == T_SOC ? v.soc_temp : v.edge_temp;
		*val = t * 10L;
		return 0;
	}
	case hwmon_in:		/* mV */
		*val = v.volt[ch];
		return 0;
	case hwmon_curr:	/* mA */
		*val = v.curr[ch];
		return 0;
	case hwmon_power: {	/* mW -> uW */
		uint32_t p = ch == P_SOCKET ? v.socket :
			     ch == P_CPU ? v.power[0] :
			     ch == P_GFX ? v.power[1] : v.core_power[ch - P_CORE0];
		*val = p * 1000L;
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int bc250_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				   u32 attr, int ch, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = bc250_temp_label[ch];
		return 0;
	case hwmon_in:
		*str = bc250_volt_label[ch];
		return 0;
	case hwmon_curr:
		*str = bc250_curr_label[ch];
		return 0;
	case hwmon_power:
		*str = bc250_power_label[ch];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops bc250_hwmon_ops = {
	.is_visible = bc250_hwmon_visible,
	.read = bc250_hwmon_read,
	.read_string = bc250_hwmon_read_string,
};

#define T_CH (HWMON_T_INPUT | HWMON_T_LABEL)
#define P_CH (HWMON_P_INPUT | HWMON_P_LABEL)
static const struct hwmon_channel_info *const bc250_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, T_CH, T_CH, T_CH, T_CH, T_CH, T_CH, T_CH, T_CH,
			   T_CH, T_CH, T_CH, T_CH, T_CH),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power, P_CH, P_CH, P_CH, P_CH, P_CH, P_CH, P_CH, P_CH,
			   P_CH, P_CH, P_CH),
	NULL
};

static const struct hwmon_chip_info bc250_hwmon_chip = {
	.ops = &bc250_hwmon_ops,
	.info = bc250_hwmon_info,
};

/* Clocks and residency have no hwmon channel type: plain attributes. */

static ssize_t freq_input_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i = to_sensor_dev_attr(attr)->index;
	struct bc250_view v;
	uint16_t mhz;
	int ret = bc250_locked_view(&v);

	if (ret)
		return ret;
	mhz = i < F_L3_0 ? v.core_freq[i] :
	      i < F_GFX ? v.l3_freq[i - F_L3_0] :
	      i == F_GFX ? v.gfxclk : i == F_SOC ? v.socclk :
	      i == F_VCLK ? v.vclk : i == F_DCLK ? v.dclk : v.memclk;
	return sysfs_emit(buf, "%lu\n", mhz * 1000000UL);
}

static ssize_t freq_label_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", bc250_freq_label[to_sensor_dev_attr(attr)->index]);
}

static ssize_t c0_residency_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bc250_view v;
	int ret = bc250_locked_view(&v);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", v.c0[to_sensor_dev_attr(attr)->index]);
}

static ssize_t throttle_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bc250_view v;
	int ret = bc250_locked_view(&v);

	if (ret)
		return ret;
	return sysfs_emit(buf, "0x%x\n", v.throttle);
}

/* Shared by the hwmon device and the telemetry directory. */

static ssize_t bc250_show_gpu_busy(char *buf)
{
	return sysfs_emit(buf, "%u\n", bc250_activity() / 100);
}

static ssize_t bc250_show_layout(char *buf)
{
	return sysfs_emit(buf, "%s\n", bc.eight ? "8-core" : "6-core");
}

static ssize_t bc250_show_state(char *buf)
{
	return sysfs_emit(buf, "%s\n", bc.hooked ? "hooked" :
			  bc.driver_fixed ? "amdgpu-already-fixed" : "export-only");
}

static ssize_t bc250_show_summary(char *buf)
{
	struct bc250_view v;
	int ret = bc250_locked_view(&v);

	if (ret)
		return ret;
	return bc250_emit_summary(&v, buf, 0);
}

static ssize_t gpu_busy_percent_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return bc250_show_gpu_busy(buf);
}

static ssize_t layout_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return bc250_show_layout(buf);
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return bc250_show_state(buf);
}

static ssize_t summary_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return bc250_show_summary(buf);
}

#define BC250_FREQ(n)								\
	static SENSOR_DEVICE_ATTR_RO(freq##n##_input, freq_input, (n) - 1);	\
	static SENSOR_DEVICE_ATTR_RO(freq##n##_label, freq_label, (n) - 1)
BC250_FREQ(1); BC250_FREQ(2); BC250_FREQ(3); BC250_FREQ(4); BC250_FREQ(5);
BC250_FREQ(6); BC250_FREQ(7); BC250_FREQ(8); BC250_FREQ(9); BC250_FREQ(10);
BC250_FREQ(11); BC250_FREQ(12); BC250_FREQ(13); BC250_FREQ(14); BC250_FREQ(15);

#define BC250_C0(n) static SENSOR_DEVICE_ATTR_RO(c0_residency_core##n, c0_residency, n)
BC250_C0(0); BC250_C0(1); BC250_C0(2); BC250_C0(3);
BC250_C0(4); BC250_C0(5); BC250_C0(6); BC250_C0(7);

static DEVICE_ATTR_RO(throttle_status);
static DEVICE_ATTR_RO(gpu_busy_percent);
static DEVICE_ATTR_RO(layout);
static DEVICE_ATTR_RO(state);
static DEVICE_ATTR_RO(summary);

#define F_ATTRS(n) &sensor_dev_attr_freq##n##_input.dev_attr.attr, \
		   &sensor_dev_attr_freq##n##_label.dev_attr.attr
static struct attribute *bc250_extra_attrs[] = {
	F_ATTRS(1), F_ATTRS(2), F_ATTRS(3), F_ATTRS(4), F_ATTRS(5),
	F_ATTRS(6), F_ATTRS(7), F_ATTRS(8), F_ATTRS(9), F_ATTRS(10),
	F_ATTRS(11), F_ATTRS(12), F_ATTRS(13), F_ATTRS(14), F_ATTRS(15),
	&sensor_dev_attr_c0_residency_core0.dev_attr.attr,
	&sensor_dev_attr_c0_residency_core1.dev_attr.attr,
	&sensor_dev_attr_c0_residency_core2.dev_attr.attr,
	&sensor_dev_attr_c0_residency_core3.dev_attr.attr,
	&sensor_dev_attr_c0_residency_core4.dev_attr.attr,
	&sensor_dev_attr_c0_residency_core5.dev_attr.attr,
	&sensor_dev_attr_c0_residency_core6.dev_attr.attr,
	&sensor_dev_attr_c0_residency_core7.dev_attr.attr,
	&dev_attr_throttle_status.attr,
	&dev_attr_gpu_busy_percent.attr,
	&dev_attr_layout.attr,
	&dev_attr_state.attr,
	&dev_attr_summary.attr,
	NULL
};

static umode_t bc250_extra_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct sensor_device_attribute *sa;

	/* Only the freq* and c0_* attributes are sensor attributes. */
	if (strncmp(attr->name, "freq", 4) && strncmp(attr->name, "c0_", 3))
		return attr->mode;
	sa = container_of(attr, struct sensor_device_attribute, dev_attr.attr);
	/* Indices 0-7 are cores in both (F_CORE0 == 0). */
	if (sa->index < 8 && !bc250_core_channel_present(sa->index))
		return 0;
	return attr->mode;
}

static const struct attribute_group bc250_extra_group = {
	.attrs = bc250_extra_attrs,
	.is_visible = bc250_extra_visible,
};
static const struct attribute_group *bc250_extra_groups[] = { &bc250_extra_group, NULL };

/* --- Telemetry directory, /sys/module/bc250_metrics_fix/telemetry --- */

static ssize_t tm_summary_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return bc250_show_summary(buf);
}

static ssize_t tm_gpu_busy_percent_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buf)
{
	return bc250_show_gpu_busy(buf);
}

static ssize_t tm_layout_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return bc250_show_layout(buf);
}

static ssize_t tm_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return bc250_show_state(buf);
}

static ssize_t tm_hwmon_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", bc.hwmon ? dev_name(bc.hwmon) : "none");
}

static struct kobj_attribute tm_summary = __ATTR(summary, 0444, tm_summary_show, NULL);
static struct kobj_attribute tm_gpu_busy_percent =
	__ATTR(gpu_busy_percent, 0444, tm_gpu_busy_percent_show, NULL);
static struct kobj_attribute tm_layout = __ATTR(layout, 0444, tm_layout_show, NULL);
static struct kobj_attribute tm_state = __ATTR(state, 0444, tm_state_show, NULL);
static struct kobj_attribute tm_hwmon = __ATTR(hwmon, 0444, tm_hwmon_show, NULL);

static struct attribute *bc250_tm_attrs[] = {
	&tm_summary.attr,
	&tm_gpu_busy_percent.attr,
	&tm_layout.attr,
	&tm_state.attr,
	&tm_hwmon.attr,
	NULL
};

static const struct attribute_group bc250_tm_group = { .attrs = bc250_tm_attrs };

static int bc250_telemetry_add(void)
{
	int ret;

	bc.telemetry = kobject_create_and_add("telemetry", &THIS_MODULE->mkobj.kobj);
	if (!bc.telemetry)
		return -ENOMEM;
	ret = sysfs_create_group(bc.telemetry, &bc250_tm_group);
	if (ret) {
		kobject_put(bc.telemetry);
		bc.telemetry = NULL;
	}
	return ret;
}

/* --- Discovery and validation --- */

/* Physical cores, so SMT being off does not matter. */
static unsigned int bc250_core_count(void)
{
	unsigned int cpu, cores = 0;

	for_each_present_cpu(cpu)
		if (cpumask_first(topology_sibling_cpumask(cpu)) == cpu)
			cores++;
	return cores;
}

static bool bc250_rail_mv(uint32_t mv)
{
	return mv >= 200 && mv <= 2000;
}

/*
 * The rail voltages sit at 0x50 in the six-core layout and at 0x64 in the
 * eight-core one; what the other layout keeps there (power in mW, C0
 * residency pairs) is never a plausible voltage. So the table itself tells
 * the layout, also with patched firmware on locked cores. The core count
 * decides only if the table does not. Called with adev->pm.mutex held.
 */
static bool bc250_detect_eight(const char **how)
{
	bool six, eight;

	if (layout == 6 || layout == 8) {
		*how = "forced";
		return layout == 8;
	}
	if (!bc250_refresh()) {
		six = bc250_rail_mv(bc.raw.six.Current.Voltage[0]) &&
		      bc250_rail_mv(bc.raw.six.Current.Voltage[1]);
		eight = bc250_rail_mv(bc.raw.eight.Voltage[0]) &&
			bc250_rail_mv(bc.raw.eight.Voltage[1]);
		if (six != eight) {
			*how = "from the metrics table";
			return eight;
		}
	}
	*how = "from the core count";
	return bc250_core_count() >= 8;
}

static bool bc250_symbol_is(const void *fn, const char *name)
{
	char sym[KSYM_SYMBOL_LEN];
	size_t len = strlen(name);

	sprint_symbol(sym, (unsigned long)fn);
	/* "name+0x0/0x... [amdgpu]" */
	return !strncmp(sym, name, len) && !strncmp(sym + len, "+0x0/", 5) &&
	       strstr(sym, " [amdgpu]");
}

#define BC250_READ_NOFAULT(dst, src) \
	copy_from_kernel_nofault(&(dst), &(src), sizeof(dst))

/*
 * Every pointer is read with copy_from_kernel_nofault() and checked against
 * a back-pointer, so a differently laid-out amdgpu is rejected, not
 * dereferenced.
 */
static int bc250_find(struct pci_dev *pdev)
{
	struct amdgpu_device *adev;
	struct drm_device *ddev;
	struct smu_context *smu;
	const struct pptable_funcs *funcs;
	struct amdgpu_device *adev_back;
	struct pci_dev *pdev_back;
	enum amd_asic_type asic;
	uint32_t metrics_size;
	void *metrics_table;
	struct smu_context *ctl_back;
	const struct smu_msg_ops *msg_ops;

	if (!pdev->driver || strcmp(pdev->driver->name, "amdgpu"))
		return -ENODEV;

	ddev = pci_get_drvdata(pdev);
	if (!ddev)
		return -ENODEV;
	adev = drm_to_adev(ddev);

	if (BC250_READ_NOFAULT(pdev_back, adev->pdev) || pdev_back != pdev ||
	    BC250_READ_NOFAULT(asic, adev->asic_type) || asic != CHIP_CYAN_SKILLFISH ||
	    BC250_READ_NOFAULT(smu, adev->powerplay.pp_handle) || !smu ||
	    BC250_READ_NOFAULT(adev_back, smu->adev) || adev_back != adev) {
		pr_err("bc250_metrics_fix: amdgpu structure layout does not match this build; rebuild for the running kernel\n");
		return -EINVAL;
	}

	if (BC250_READ_NOFAULT(funcs, smu->ppt_funcs) || !funcs ||
	    BC250_READ_NOFAULT(metrics_size, smu->smu_table.tables[SMU_TABLE_SMU_METRICS].size) ||
	    BC250_READ_NOFAULT(metrics_table, smu->smu_table.metrics_table) || !metrics_table) {
		pr_err("bc250_metrics_fix: amdgpu SMU state is not readable\n");
		return -EINVAL;
	}
	if (BC250_READ_NOFAULT(ctl_back, smu->msg_ctl.smu) || ctl_back != smu ||
	    BC250_READ_NOFAULT(msg_ops, smu->msg_ctl.ops) || !msg_ops) {
		pr_err("bc250_metrics_fix: amdgpu SMU message control does not match this build\n");
		return -EINVAL;
	}
	if (metrics_size != sizeof(SmuMetrics_t)) {
		pr_err("bc250_metrics_fix: unexpected SMU metrics table size %u (expected %zu)\n",
		       metrics_size, sizeof(SmuMetrics_t));
		return -EINVAL;
	}

	bc.pdev = pdev;
	bc.adev = adev;
	bc.smu = smu;
	bc.orig = funcs;
	return 0;
}

static bool bc250_stock_callbacks(void)
{
	return bc250_symbol_is(bc.orig->read_sensor, "cyan_skillfish_read_sensor") &&
	       bc250_symbol_is(bc.orig->get_gpu_metrics, "cyan_skillfish_get_gpu_metrics") &&
	       bc250_symbol_is(bc.orig->emit_clk_levels, "cyan_skillfish_emit_clk_levels") &&
	       bc250_symbol_is(bc.orig->od_edit_dpm_table, "cyan_skillfish_od_edit_dpm_table") &&
	       bc250_symbol_is(bc.orig->get_dpm_ultimate_freq, "cyan_skillfish_get_dpm_ultimate_freq");
}

static int bc250_hook(void)
{
	uint32_t val, size = sizeof(val);
	struct bc250_view v;

	if (!bc250_stock_callbacks()) {
		pr_warn("bc250_metrics_fix: amdgpu's Cyan Skillfish callbacks are not the stock ones (another patch or module owns them); not hooking\n");
		return 0;
	}

	/* A driver that already fixes telemetry implements GPU_LOAD. */
	if (bc.orig->read_sensor(bc.smu, AMDGPU_PP_SENSOR_GPU_LOAD, &val, &size) != -EOPNOTSUPP) {
		bc.driver_fixed = true;
		pr_info("bc250_metrics_fix: amdgpu already provides BC-250 telemetry; not hooking\n");
		return 0;
	}

	bc.ours = kmemdup(bc.orig, sizeof(*bc.orig), GFP_KERNEL);
	if (!bc.ours)
		return -ENOMEM;
	bc.ours->read_sensor = bc250_read_sensor;
	bc.ours->get_gpu_metrics = bc250_get_gpu_metrics;
	bc.ours->emit_clk_levels = bc250_emit_clk_levels;
	bc.ours->od_edit_dpm_table = bc250_od_edit_dpm_table;
	bc.ours->get_dpm_ultimate_freq = bc250_get_dpm_ultimate_freq;

	/*
	 * The firmware's clock when first loaded at boot. The installer passes it
	 * back on reloads, when a governor may have moved the clock.
	 */
	if (!sclk_default && !bc250_view(false, &v))
		sclk_default = v.gfxclk;
	sclk_default = clamp(sclk_default ?: sclk_max, sclk_min, sclk_max);

	WRITE_ONCE(bc.smu->ppt_funcs, bc.ours);
	bc.hooked = true;
	return 0;
}

/*
 * Replaced callbacks run only under adev->pm.mutex, so none is running once
 * the original table is back. Our copy is never freed: the other callbacks
 * may still be called through a pointer read earlier.
 */
static void bc250_unhook(void)
{
	if (!bc.hooked)
		return;
	WRITE_ONCE(bc.smu->ppt_funcs, bc.orig);
	bc.hooked = false;
}

static void bc250_teardown(void)
{
	if (!bc.adev)
		return;
	/* Readers of both are drained before this returns. */
	if (bc.telemetry) {
		kobject_put(bc.telemetry);
		bc.telemetry = NULL;
	}
	if (bc.hwmon) {
		hwmon_device_unregister(bc.hwmon);
		bc.hwmon = NULL;
	}
	mutex_lock(&bc.adev->pm.mutex);
	bc250_unhook();
	mutex_unlock(&bc.adev->pm.mutex);
	/* Nothing can start the sampler any more. */
	hrtimer_cancel(&act.timer);
	bc.adev = NULL;
	bc.smu = NULL;
}

/* amdgpu may be unbound while we are loaded: let go first. */
static int bc250_bus_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	if (action == BUS_NOTIFY_UNBIND_DRIVER && to_pci_dev(data) == bc.pdev) {
		pr_info("bc250_metrics_fix: amdgpu is unbinding, restoring its callbacks\n");
		bc250_teardown();
	}
	return NOTIFY_DONE;
}

static struct notifier_block bc250_nb = { .notifier_call = bc250_bus_notify };

static int __init bc250_init(void)
{
	struct pci_dev *pdev;
	const char *how;
	int ret;

	if (!sclk_min || sclk_min > sclk_max) {
		pr_err("bc250_metrics_fix: invalid sclk_min=%u sclk_max=%u\n", sclk_min, sclk_max);
		return -EINVAL;
	}

	pdev = pci_get_device(PCI_VENDOR_ID_ATI, 0x13fe, NULL);
	if (!pdev)
		return -ENODEV;

	ret = bc250_find(pdev);
	if (ret)
		goto err_put;

	bc250_act_init();

	mutex_lock(&bc.adev->pm.mutex);
	if (!bc250_smu_usable()) {
		mutex_unlock(&bc.adev->pm.mutex);
		pr_err("bc250_metrics_fix: amdgpu power management is not up yet\n");
		ret = -EAGAIN;
		goto err_put;
	}
	bc.eight = bc250_detect_eight(&how);
	/* Patched firmware keeps eight slots with cores locked: hide the empty ones. */
	bc.ncores = min_t(unsigned int, bc.eight ? 8 : 6, bc250_core_count() ?: 8);
	if (!bc.eight && bc250_core_count() >= 8)
		dev_warn(&pdev->dev, "bc250_metrics_fix: 8 cores but a six-core metrics table: the SMU firmware lacks the 8-core metrics patch, so per-core readings and the GFX clock are wrong; enable the patch in the BIOS\n");
	ret = hook ? bc250_hook() : 0;
	mutex_unlock(&bc.adev->pm.mutex);
	if (ret)
		goto err_put;

	if (hwmon_enable) {
		bc.hwmon = hwmon_device_register_with_info(&pdev->dev, "bc250", NULL,
							   &bc250_hwmon_chip, bc250_extra_groups);
		if (IS_ERR(bc.hwmon)) {
			ret = PTR_ERR(bc.hwmon);
			bc.hwmon = NULL;
			bc250_teardown();
			goto err_put;
		}
	}

	ret = bc250_telemetry_add();
	if (ret) {
		bc250_teardown();
		goto err_put;
	}

	ret = bus_register_notifier(&pci_bus_type, &bc250_nb);
	if (ret) {
		bc250_teardown();
		goto err_put;
	}

	dev_info(&pdev->dev, "bc250_metrics_fix %s: %s SMU metrics layout (%s, %u cores), sclk %u-%u MHz, %s, %s\n",
		 BC250_VERSION, bc.eight ? "eight-core" : "six-core", how, bc250_core_count(),
		 sclk_min, sclk_max,
		 bc.hooked ? "amdgpu telemetry callbacks replaced" :
		 bc.driver_fixed ? "amdgpu already fixed, export only" : "export only",
		 bc.hwmon ? dev_name(bc.hwmon) : "no hwmon device");
	return 0;

err_put:
	pci_dev_put(pdev);
	return ret;
}

static void __exit bc250_exit(void)
{
	bus_unregister_notifier(&pci_bus_type, &bc250_nb);
	bc250_teardown();
	pci_dev_put(bc.pdev);
}

module_init(bc250_init);
module_exit(bc250_exit);

MODULE_DESCRIPTION("BC-250 (Cyan Skillfish) SMU telemetry fix for amdgpu");
MODULE_VERSION(BC250_VERSION);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_SOFTDEP("pre: amdgpu");
