// SPDX-License-Identifier: GPL-2.0-only
/*
 * HP OMEN / Victus WMI GPU power unlock (GC21/GC22 + performance mode).
 * Mirrors hp-wmi paths for boards missing from the kernel DMI allowlist.
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define DRV_NAME			"omen_wmi_boost"
#define HPWMI_BIOS_GUID			"5FB7F034-2C63-45E9-BE91-3D44E2C707E4"
#define HPWMI_SIGNATURE			0x55434553
#define HPWMI_GM			0x20008

#define HPWMI_GET_GPU_THERMAL_MODES	0x21
#define HPWMI_SET_GPU_THERMAL_MODES	0x22
#define HPWMI_SET_PERFORMANCE_MODE	0x1a
#define HPWMI_FAN_COUNT_GET_QUERY	0x10
#define HPWMI_FAN_SPEED_GET_QUERY	0x11
#define HPWMI_FAN_SPEED_MAX_GET_QUERY	0x26
#define HPWMI_FAN_SPEED_MAX_SET_QUERY	0x27
#define HPWMI_GET_SYSTEM_DESIGN_DATA	0x28
#define HPWMI_VICTUS_S_FAN_SPEED_GET_QUERY	0x2d
#define HPWMI_VICTUS_S_FAN_SPEED_SET_QUERY	0x2e
#define HPWMI_VICTUS_S_GET_FAN_TABLE_QUERY	0x2f

#define HP_THERMAL_VICTUS_S_PERFORMANCE	0x01
#define HP_THERMAL_OMEN_V1_PERFORMANCE	0x31
#define HP_FAN_SPEED_AUTOMATIC		0x00

#define HP_FAN_CPU			0
#define HP_FAN_GPU			1
#define HP_FAN_KEEPALIVE_SECS		90
#define HP_FAN_DEFAULT_MAX_SPEED	60

#define HPWMI_RET_UNKNOWN_COMMAND	0x03
#define HPWMI_RET_UNKNOWN_CMDTYPE	0x04

/* --- hp-wmi packet layout ------------------------------------------------ */

struct bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[];
};

struct bios_return {
	u32 sigpass;
	u32 return_code;
};

struct gpu_power_modes {
	u8 ctgp;
	u8 ppab;
	u8 dstate;
	u8 slowdown_temp;
};

struct victus_s_fan_table_header {
	u8 num_fans;
	u8 unknown;
} __packed;

struct victus_s_fan_table_entry {
	u8 cpu_speed;
	u8 gpu_speed;
	u8 noise_db;
} __packed;

struct victus_s_fan_table {
	struct victus_s_fan_table_header header;
	struct victus_s_fan_table_entry entries[];
} __packed;

enum fan_control_mode {
	FAN_MODE_MAX = 0,
	FAN_MODE_MANUAL = 1,
	FAN_MODE_AUTO = 2,
};

struct fan_control_state {
	enum fan_control_mode mode;
	u8 manual_speed;
	u8 min_speed;
	u8 max_speed;
	int gpu_delta;
	bool manual_supported;
	bool table_valid;
	bool max_supported;
	int last_error;
	struct delayed_work keepalive;
};

/* --- module parameters --------------------------------------------------- */

static bool persist = true;
module_param(persist, bool, 0444);
MODULE_PARM_DESC(persist, "Stay loaded and expose sysfs (default: Y)");

static bool auto_boost;
module_param(auto_boost, bool, 0444);
MODULE_PARM_DESC(auto_boost, "On load, run full performance path (persist mode)");

static bool minimal_packet;
module_param(minimal_packet, bool, 0644);
MODULE_PARM_DESC(minimal_packet, "Prefer DASI=0 for GC21 reads");

static u8 thermal_profile = HP_THERMAL_VICTUS_S_PERFORMANCE;
module_param(thermal_profile, byte, 0644);
MODULE_PARM_DESC(thermal_profile, "SET_PERFORMANCE_MODE byte (1 or 0x31)");

static char boot_mode[16];
module_param_string(boot_mode, boot_mode, sizeof(boot_mode), 0444);
MODULE_PARM_DESC(boot_mode,
		 "One-shot at load: read|enable|disable|performance|trace (empty=none)");

/* Legacy alias used in early testing */
static char mode[16];
module_param_string(mode, mode, sizeof(mode), 0444);
MODULE_PARM_DESC(mode, "Deprecated: use boot_mode= instead");

/* --- driver state -------------------------------------------------------- */

static struct {
	struct kobject *kobj;
	struct mutex lock;
	bool wmi_ready;
	char last_error[128];
	struct fan_control_state fan;
} omen_drv;

/* --- WMI core (mirrors hp_wmi_perform_query) ----------------------------- */

static int encode_method_id(int outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

static int hp_gm_query(u32 cmtp, void *buf, int insize, int outsize)
{
	struct acpi_buffer input, output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct bios_args *args = NULL;
	union acpi_object *obj = NULL;
	struct bios_return *rsp;
	int mid, padded_in, out_len, ret = 0;
	size_t args_sz;
	acpi_status ast;

	mid = encode_method_id(outsize);
	if (mid < 0)
		return mid;

	padded_in = max(insize, 128);
	args_sz = struct_size(args, data, padded_in);
	args = kzalloc(args_sz, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	args->signature = HPWMI_SIGNATURE;
	args->command = HPWMI_GM;
	args->commandtype = cmtp;
	args->datasize = insize;
	if (insize > 0)
		memcpy(args->data, buf, insize);

	input.length = args_sz;
	input.pointer = args;

	ast = wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	if (ACPI_FAILURE(ast)) {
		ret = -EIO;
		pr_err("WMI cmtp=0x%x failed: %s\n", cmtp, acpi_format_exception(ast));
		goto out;
	}

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER ||
	    obj->buffer.length < sizeof(*rsp)) {
		ret = -EINVAL;
		pr_err("WMI cmtp=0x%x: bad response\n", cmtp);
		goto out;
	}

	rsp = (struct bios_return *)obj->buffer.pointer;
	if (rsp->return_code) {
		if (rsp->return_code == HPWMI_RET_UNKNOWN_COMMAND)
			pr_err("firmware: unknown command (cmtp=0x%x)\n", cmtp);
		else if (rsp->return_code == HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_err("firmware: unknown command type (cmtp=0x%x)\n", cmtp);
		else
			pr_warn("firmware error 0x%x (cmtp=0x%x)\n",
				rsp->return_code, cmtp);
		ret = -EIO;
		goto out;
	}

	if (outsize) {
		out_len = min(outsize, (int)obj->buffer.length - (int)sizeof(*rsp));
		memcpy(buf, (u8 *)rsp + sizeof(*rsp), out_len);
		if (out_len < outsize)
			memset((u8 *)buf + out_len, 0, outsize - out_len);
	}

out:
	kfree(obj);
	kfree(args);
	return ret;
}

/* --- high-level operations ----------------------------------------------- */

static void gpu_modes_log(const char *tag, const struct gpu_power_modes *m)
{
	pr_info("%s: CTGP=%u DTGP=%u DSTA=%u slowdown_temp=%u\n",
		tag, m->ctgp, m->ppab, m->dstate, m->slowdown_temp);
}

static int gpu_modes_read(struct gpu_power_modes *modes)
{
	int insize = minimal_packet ? 0 : sizeof(*modes);
	int ret;

	ret = hp_gm_query(HPWMI_GET_GPU_THERMAL_MODES, modes, insize,
			  sizeof(*modes));
	if (ret && !minimal_packet) {
		pr_debug("GC21 retry with DASI=0\n");
		ret = hp_gm_query(HPWMI_GET_GPU_THERMAL_MODES, modes, 0,
				  sizeof(*modes));
	}
	return ret;
}

static int gpu_modes_write(bool enable, const struct gpu_power_modes *cur)
{
	struct gpu_power_modes set = {
		.ctgp = enable ? 1 : 0,
		.ppab = enable ? 1 : 0,
		.dstate = cur->dstate,
		.slowdown_temp = cur->slowdown_temp,
	};

	return hp_gm_query(HPWMI_SET_GPU_THERMAL_MODES, &set, sizeof(set), 0);
}

static int fan_trigger(void)
{
	u8 data[4] = { 0 };

	return hp_gm_query(HPWMI_FAN_COUNT_GET_QUERY, data, 1, sizeof(data));
}

static int fan_count_read(int *count)
{
	u8 data[4] = { 0 };
	int ret;

	ret = hp_gm_query(HPWMI_FAN_COUNT_GET_QUERY, data, 1, sizeof(data));
	if (ret)
		return ret;

	*count = data[0];
	return 0;
}

static int fan_rpm_read_legacy(int fan, int *rpm)
{
	u8 data[4] = { fan, 0, 0, 0 };
	int ret;

	ret = hp_gm_query(HPWMI_FAN_SPEED_GET_QUERY, data, 1, sizeof(data));
	if (ret)
		return ret;

	*rpm = (data[2] << 8) | data[3];
	return 0;
}

static int fan_rpm_read_victus_s(int fan, int *rpm)
{
	u8 data[128] = { 0 };
	int ret;

	if (fan < 0 || fan >= sizeof(data))
		return -EINVAL;

	ret = hp_gm_query(HPWMI_VICTUS_S_FAN_SPEED_GET_QUERY, data, 1,
			  sizeof(data));
	if (ret)
		return ret;

	*rpm = data[fan] * 100;
	return 0;
}

static int fan_rpm_read(int fan, int *rpm)
{
	int ret;

	ret = fan_rpm_read_legacy(fan, rpm);
	if (!ret)
		return 0;

	return fan_rpm_read_victus_s(fan, rpm);
}

static int fan_max_get(int *raw)
{
	int val = 0;
	int ret;

	ret = hp_gm_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, &val, sizeof(val),
			  sizeof(val));
	if (ret)
		return ret;

	*raw = val;
	return 0;
}

static int fan_max_set(bool enable)
{
	int val = enable ? 1 : 0;

	return hp_gm_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, &val, sizeof(val), 0);
}

static int fan_speed_set(u8 speed)
{
	u8 fan_speed[2];
	int gpu_speed, ret;

	fan_speed[HP_FAN_CPU] = speed;
	fan_speed[HP_FAN_GPU] = speed;

	if (speed != HP_FAN_SPEED_AUTOMATIC) {
		gpu_speed = speed + omen_drv.fan.gpu_delta;
		fan_speed[HP_FAN_GPU] = clamp_val(gpu_speed, 0, U8_MAX);
	}

	ret = fan_trigger();
	if (ret)
		return ret;

	ret = fan_max_set(false);
	if (ret)
		return ret;

	return hp_gm_query(HPWMI_VICTUS_S_FAN_SPEED_SET_QUERY, fan_speed,
			   sizeof(fan_speed), 0);
}

static int fan_speed_reset(void)
{
	return fan_speed_set(HP_FAN_SPEED_AUTOMATIC);
}

static void fan_set_error(int err)
{
	omen_drv.fan.last_error = err;
}

static void fan_clear_error(void)
{
	omen_drv.fan.last_error = 0;
}

static const char *fan_mode_name(enum fan_control_mode mode)
{
	switch (mode) {
	case FAN_MODE_MAX:
		return "max";
	case FAN_MODE_MANUAL:
		return "manual";
	case FAN_MODE_AUTO:
		return "auto";
	default:
		return "unknown";
	}
}

static int fan_table_probe(bool log)
{
	u8 data[128] = { 0 };
	struct victus_s_fan_table *table = (struct victus_s_fan_table *)data;
	u8 min_speed = U8_MAX, max_speed = 0;
	int first_gpu_delta = 0;
	int entries, i, ret;

	ret = hp_gm_query(HPWMI_VICTUS_S_GET_FAN_TABLE_QUERY, data, 4,
			  sizeof(data));
	if (ret) {
		omen_drv.fan.manual_supported = false;
		omen_drv.fan.table_valid = false;
		if (log)
			pr_warn("fan table read failed: %d\n", ret);
		return ret;
	}

	entries = (sizeof(data) - sizeof(*table)) / sizeof(table->entries[0]);
	for (i = 0; i < entries; i++) {
		u8 cpu = table->entries[i].cpu_speed;
		u8 gpu = table->entries[i].gpu_speed;
		u8 noise = table->entries[i].noise_db;

		if (!cpu && !gpu && !noise)
			break;

		min_speed = min(min_speed, cpu);
		max_speed = max(max_speed, cpu);
		if (!first_gpu_delta)
			first_gpu_delta = (int)gpu - (int)cpu;

		if (log)
			pr_info("fan table[%d]: cpu=%u gpu=%u noise_db=%u\n",
				i, cpu, gpu, noise);
	}

	if (min_speed == U8_MAX || !max_speed) {
		omen_drv.fan.manual_supported = false;
		omen_drv.fan.table_valid = false;
		if (log)
			pr_warn("fan table did not contain usable entries\n");
		return -EINVAL;
	}

	omen_drv.fan.min_speed = min_speed;
	omen_drv.fan.max_speed = max_speed;
	omen_drv.fan.gpu_delta = first_gpu_delta;
	omen_drv.fan.manual_supported = true;
	omen_drv.fan.table_valid = true;
	omen_drv.fan.manual_speed = clamp_val(omen_drv.fan.manual_speed,
					      min_speed, max_speed);

	if (log)
		pr_info("fan table: fans=%u min=%u max=%u gpu_delta=%d\n",
			table->header.num_fans, min_speed, max_speed,
			first_gpu_delta);

	return 0;
}

static unsigned long fan_keepalive_delay(void)
{
	return msecs_to_jiffies(HP_FAN_KEEPALIVE_SECS * MSEC_PER_SEC);
}

static int fan_apply_locked(void)
{
	int ret;

	switch (omen_drv.fan.mode) {
	case FAN_MODE_MAX:
		ret = fan_trigger();
		if (ret)
			goto err;

		ret = fan_max_set(true);
		if (ret)
			goto err;

		omen_drv.fan.max_supported = true;
		mod_delayed_work(system_wq, &omen_drv.fan.keepalive,
				 fan_keepalive_delay());
		return 0;
	case FAN_MODE_MANUAL:
		if (!omen_drv.fan.manual_supported) {
			ret = fan_table_probe(false);
			if (ret)
				goto err;
		}

		ret = fan_speed_set(omen_drv.fan.manual_speed);
		if (ret)
			goto err;

		mod_delayed_work(system_wq, &omen_drv.fan.keepalive,
				 fan_keepalive_delay());
		return 0;
	case FAN_MODE_AUTO:
		ret = fan_max_set(false);
		if (ret)
			goto err;

		if (omen_drv.fan.manual_supported) {
			ret = fan_speed_reset();
			if (ret)
				goto err;
		}

		cancel_delayed_work(&omen_drv.fan.keepalive);
		return 0;
	default:
		ret = -EINVAL;
		goto err;
	}

err:
	fan_set_error(ret);
	return ret;
}

static void fan_keepalive_work(struct work_struct *work)
{
	int ret;

	guard(mutex)(&omen_drv.lock);
	if (omen_drv.fan.mode == FAN_MODE_AUTO)
		return;

	ret = fan_apply_locked();
	if (ret)
		pr_warn_ratelimited("fan keepalive failed: %d\n", ret);
}

static void fan_control_init(void)
{
	omen_drv.fan.mode = FAN_MODE_AUTO;
	omen_drv.fan.min_speed = 0;
	omen_drv.fan.max_speed = HP_FAN_DEFAULT_MAX_SPEED;
	omen_drv.fan.manual_speed = HP_FAN_DEFAULT_MAX_SPEED / 2;
	omen_drv.fan.gpu_delta = 0;
	omen_drv.fan.manual_supported = false;
	omen_drv.fan.table_valid = false;
	omen_drv.fan.max_supported = false;
	omen_drv.fan.last_error = 0;
	INIT_DELAYED_WORK(&omen_drv.fan.keepalive, fan_keepalive_work);

	/*
	 * This is read-only discovery. It deliberately avoids applying any fan
	 * mode so the firmware's default automatic policy stays in charge.
	 */
	fan_table_probe(false);
}

static void fan_control_exit(void)
{
	cancel_delayed_work_sync(&omen_drv.fan.keepalive);
}

static int thermal_profile_set(u8 profile)
{
	char buf[2] = { -1, profile };

	return hp_gm_query(HPWMI_SET_PERFORMANCE_MODE, buf, sizeof(buf), 0);
}

static int gpu_boost_set(bool enable)
{
	struct gpu_power_modes before, after;
	int ret;

	ret = gpu_modes_read(&before);
	if (ret)
		return ret;

	gpu_modes_log(enable ? "enabling" : "disabling", &before);

	ret = gpu_modes_write(enable, &before);
	if (ret)
		return ret;

	ret = gpu_modes_read(&after);
	if (ret)
		return ret;

	gpu_modes_log("result", &after);

	if (enable && (!after.ctgp || !after.ppab))
		pr_warn("CTGP/DTGP did not stick — check OGHP / thermal profile\n");

	return 0;
}

static int performance_apply(void)
{
	int ret;

	ret = fan_trigger();
	if (ret)
		pr_warn("fan trigger failed (%d), continuing\n", ret);

	ret = thermal_profile_set(thermal_profile);
	if (ret) {
		pr_err("SET_PERFORMANCE_MODE 0x%02x failed: %d\n",
		       thermal_profile, ret);
		return ret;
	}

	pr_info("thermal profile 0x%02x set\n", thermal_profile);
	return gpu_boost_set(true);
}

static int trace_dump(void)
{
	struct gpu_power_modes m;
	u8 design[8] = { 0 };
	int ret;

	pr_info("trace: board=%s product=%s\n",
		dmi_get_system_info(DMI_BOARD_NAME) ?: "?",
		dmi_get_system_info(DMI_PRODUCT_NAME) ?: "?");

	ret = hp_gm_query(HPWMI_GET_SYSTEM_DESIGN_DATA, design, sizeof(design),
			  sizeof(design));
	if (!ret)
		pr_info("system_design: %*ph\n", 8, design);
	else
		pr_warn("system_design read failed: %d\n", ret);

	ret = gpu_modes_read(&m);
	if (!ret)
		gpu_modes_log("GC21", &m);
	return ret;
}

static int run_boot_mode(const char *name)
{
	if (!name || !name[0])
		return 0;

	pr_info("boot_mode=%s\n", name);

	if (!strcmp(name, "read")) {
		struct gpu_power_modes m;
		int ret = gpu_modes_read(&m);

		if (!ret)
			gpu_modes_log("GC21", &m);
		return ret;
	}
	if (!strcmp(name, "enable"))
		return gpu_boost_set(true);
	if (!strcmp(name, "disable"))
		return gpu_boost_set(false);
	if (!strcmp(name, "performance"))
		return performance_apply();
	if (!strcmp(name, "trace"))
		return trace_dump();

	pr_err("unknown boot_mode '%s'\n", name);
	return -EINVAL;
}

static void omen_set_last_error(int err)
{
	scnprintf(omen_drv.last_error, sizeof(omen_drv.last_error),
		  "auto_boost failed: %d", err);
}

static void omen_clear_last_error(void)
{
	omen_drv.last_error[0] = '\0';
}

/* --- sysfs ---------------------------------------------------------------- */

static ssize_t last_error_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	return sysfs_emit(buf, "%s\n", omen_drv.last_error);
}

static ssize_t gpu_state_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct gpu_power_modes m;
	int ret;

	scoped_guard(mutex, &omen_drv.lock) {
		ret = gpu_modes_read(&m);
	}
	if (ret)
		return ret;

	return sysfs_emit(buf, "ctgp=%u ppab=%u dstate=%u slowdown_temp=%u\n",
			  m.ctgp, m.ppab, m.dstate, m.slowdown_temp);
}

static ssize_t boost_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	bool enable;
	int ret;

	if (kstrtobool(buf, &enable))
		return -EINVAL;

	guard(mutex)(&omen_drv.lock);
	ret = gpu_boost_set(enable);

	return ret ? ret : count;
}

static ssize_t performance_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;

	if (count > 0 && buf[0] == '0')
		return -EINVAL;

	guard(mutex)(&omen_drv.lock);
	ret = performance_apply();

	return ret ? ret : count;
}

static ssize_t thermal_profile_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0x%02x\n", thermal_profile);
}

static ssize_t thermal_profile_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 0, &val) || val > 0xff)
		return -EINVAL;

	thermal_profile = (u8)val;
	return count;
}

static ssize_t fan_state_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	int fan_count = -1, fan_count_ret, fan1_ret, fan2_ret, max_ret;
	int fan1_rpm = -1, fan2_rpm = -1, max_raw = -1;
	ssize_t len = 0;

	guard(mutex)(&omen_drv.lock);

	fan_count_ret = fan_count_read(&fan_count);
	fan1_ret = fan_rpm_read(0, &fan1_rpm);
	fan2_ret = fan_rpm_read(1, &fan2_rpm);
	max_ret = fan_max_get(&max_raw);
	if (!max_ret)
		omen_drv.fan.max_supported = true;

	len += sysfs_emit_at(buf, len, "mode=%s\n",
			     fan_mode_name(omen_drv.fan.mode));
	len += sysfs_emit_at(buf, len, "manual_supported=%u\n",
			     omen_drv.fan.manual_supported);
	len += sysfs_emit_at(buf, len, "table_valid=%u\n",
			     omen_drv.fan.table_valid);
	len += sysfs_emit_at(buf, len, "speed_min=%u\n",
			     omen_drv.fan.min_speed);
	len += sysfs_emit_at(buf, len, "speed_max=%u\n",
			     omen_drv.fan.max_speed);
	len += sysfs_emit_at(buf, len, "manual_speed=%u\n",
			     omen_drv.fan.manual_speed);
	len += sysfs_emit_at(buf, len, "gpu_delta=%d\n",
			     omen_drv.fan.gpu_delta);
	len += sysfs_emit_at(buf, len, "max_supported=%u\n",
			     omen_drv.fan.max_supported);
	if (!max_ret)
		len += sysfs_emit_at(buf, len, "max_raw=%d\n", max_raw);
	else
		len += sysfs_emit_at(buf, len, "max_error=%d\n", max_ret);

	if (!fan_count_ret)
		len += sysfs_emit_at(buf, len, "fan_count=%d\n", fan_count);
	else
		len += sysfs_emit_at(buf, len, "fan_count_error=%d\n",
				     fan_count_ret);

	if (!fan1_ret)
		len += sysfs_emit_at(buf, len, "fan1_rpm=%d\n", fan1_rpm);
	else
		len += sysfs_emit_at(buf, len, "fan1_error=%d\n", fan1_ret);

	if (!fan2_ret)
		len += sysfs_emit_at(buf, len, "fan2_rpm=%d\n", fan2_rpm);
	else
		len += sysfs_emit_at(buf, len, "fan2_error=%d\n", fan2_ret);

	len += sysfs_emit_at(buf, len, "last_fan_error=%d\n",
			     omen_drv.fan.last_error);

	return len;
}

static ssize_t fan_mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	guard(mutex)(&omen_drv.lock);
	return sysfs_emit(buf, "%s\n", fan_mode_name(omen_drv.fan.mode));
}

static ssize_t fan_mode_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	enum fan_control_mode old_mode, new_mode;
	int ret;

	if (sysfs_streq(buf, "auto") || sysfs_streq(buf, "2"))
		new_mode = FAN_MODE_AUTO;
	else if (sysfs_streq(buf, "manual") || sysfs_streq(buf, "1"))
		new_mode = FAN_MODE_MANUAL;
	else if (sysfs_streq(buf, "max") || sysfs_streq(buf, "0"))
		new_mode = FAN_MODE_MAX;
	else
		return -EINVAL;

	guard(mutex)(&omen_drv.lock);
	old_mode = omen_drv.fan.mode;
	omen_drv.fan.mode = new_mode;
	fan_clear_error();

	ret = fan_apply_locked();
	if (ret) {
		omen_drv.fan.mode = old_mode;
		return ret;
	}

	return count;
}

static ssize_t fan_speed_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	guard(mutex)(&omen_drv.lock);
	return sysfs_emit(buf, "%u\n", omen_drv.fan.manual_speed);
}

static ssize_t fan_speed_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int val;
	u8 old_speed;
	int ret;

	if (kstrtouint(buf, 0, &val) || val > U8_MAX)
		return -EINVAL;

	guard(mutex)(&omen_drv.lock);
	if (omen_drv.fan.mode != FAN_MODE_MANUAL)
		return -EINVAL;
	if (!omen_drv.fan.manual_supported) {
		ret = fan_table_probe(false);
		if (ret) {
			fan_set_error(ret);
			return ret;
		}
	}

	old_speed = omen_drv.fan.manual_speed;
	omen_drv.fan.manual_speed = clamp_val(val, omen_drv.fan.min_speed,
					      omen_drv.fan.max_speed);
	fan_clear_error();

	ret = fan_apply_locked();
	if (ret) {
		omen_drv.fan.manual_speed = old_speed;
		return ret;
	}

	return count;
}

static ssize_t fan_probe_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	int fan_count = -1, fan1_rpm = -1, fan2_rpm = -1, max_raw = -1;
	int table_ret, count_ret, fan1_ret, fan2_ret, max_ret;

	if (count > 0 && buf[0] == '0')
		return -EINVAL;

	guard(mutex)(&omen_drv.lock);
	table_ret = fan_table_probe(true);
	count_ret = fan_count_read(&fan_count);
	fan1_ret = fan_rpm_read(0, &fan1_rpm);
	fan2_ret = fan_rpm_read(1, &fan2_rpm);
	max_ret = fan_max_get(&max_raw);
	if (!max_ret)
		omen_drv.fan.max_supported = true;

	pr_info("fan probe: table_ret=%d count_ret=%d fan_count=%d\n",
		table_ret, count_ret, fan_count);
	pr_info("fan probe: fan1_ret=%d fan1_rpm=%d fan2_ret=%d fan2_rpm=%d\n",
		fan1_ret, fan1_rpm, fan2_ret, fan2_rpm);
	pr_info("fan probe: max_ret=%d max_raw=%d manual_supported=%u\n",
		max_ret, max_raw, omen_drv.fan.manual_supported);

	if (table_ret)
		fan_set_error(table_ret);
	else
		fan_clear_error();

	return count;
}

static struct kobj_attribute omen_attr_last_error =
	__ATTR_RO(last_error);
static struct kobj_attribute omen_attr_gpu_state =
	__ATTR_RO(gpu_state);
static struct kobj_attribute omen_attr_boost =
	__ATTR_WO(boost);
static struct kobj_attribute omen_attr_performance =
	__ATTR_WO(performance);
static struct kobj_attribute omen_attr_thermal_profile =
	__ATTR(thermal_profile, 0644, thermal_profile_show, thermal_profile_store);
static struct kobj_attribute omen_attr_fan_state =
	__ATTR_RO(fan_state);
static struct kobj_attribute omen_attr_fan_mode =
	__ATTR(fan_mode, 0644, fan_mode_show, fan_mode_store);
static struct kobj_attribute omen_attr_fan_speed =
	__ATTR(fan_speed, 0644, fan_speed_show, fan_speed_store);
static struct kobj_attribute omen_attr_fan_probe =
	__ATTR_WO(fan_probe);

static struct attribute *omen_attrs[] = {
	&omen_attr_last_error.attr,
	&omen_attr_gpu_state.attr,
	&omen_attr_boost.attr,
	&omen_attr_performance.attr,
	&omen_attr_thermal_profile.attr,
	&omen_attr_fan_state.attr,
	&omen_attr_fan_mode.attr,
	&omen_attr_fan_speed.attr,
	&omen_attr_fan_probe.attr,
	NULL,
};

static const struct attribute_group omen_attr_group = {
	.attrs = omen_attrs,
};

static void omen_sysfs_remove(void)
{
	if (omen_drv.kobj) {
		sysfs_remove_group(omen_drv.kobj, &omen_attr_group);
		kobject_put(omen_drv.kobj);
		omen_drv.kobj = NULL;
	}
}

static int omen_sysfs_init(void)
{
	omen_drv.kobj = kobject_create_and_add(DRV_NAME, kernel_kobj);
	if (!omen_drv.kobj)
		return -ENOMEM;

	if (sysfs_create_group(omen_drv.kobj, &omen_attr_group)) {
		omen_sysfs_remove();
		return -ENOMEM;
	}

	return 0;
}

/* --- module lifecycle ---------------------------------------------------- */

static int __init omen_wmi_boost_init(void)
{
	const char *once;
	int ret = 0;

	if (!wmi_has_guid(HPWMI_BIOS_GUID)) {
		pr_err("HP BIOS WMI GUID not present\n");
		return -ENODEV;
	}

	omen_drv.wmi_ready = true;
	mutex_init(&omen_drv.lock);
	fan_control_init();

	pr_info("board %s — %s\n",
		dmi_get_system_info(DMI_BOARD_NAME) ?: "unknown",
		dmi_get_system_info(DMI_PRODUCT_NAME) ?: "unknown");

	once = boot_mode[0] ? boot_mode : mode;

	if (persist) {
		ret = omen_sysfs_init();
		if (ret)
			return ret;

		pr_info("sysfs: /sys/kernel/%s/{gpu_state,last_error,boost,performance,thermal_profile,fan_state,fan_mode,fan_speed,fan_probe}\n",
			DRV_NAME);

		if (auto_boost) {
			guard(mutex)(&omen_drv.lock);
			omen_clear_last_error();
			ret = performance_apply();
			if (ret) {
				omen_set_last_error(ret);
				pr_err("auto_boost failed: %d\n", ret);
			}
		} else if (once[0]) {
			guard(mutex)(&omen_drv.lock);
			ret = run_boot_mode(once);
			if (ret)
				pr_err("boot_mode failed: %d\n", ret);
		}
		return 0;
	}

	/* One-shot: fail insmod if the requested operation fails */
	if (!once[0]) {
		pr_err("persist=0 requires boot_mode= or mode=\n");
		return -EINVAL;
	}

	guard(mutex)(&omen_drv.lock);
	return run_boot_mode(once);
}

static void __exit omen_wmi_boost_exit(void)
{
	fan_control_exit();
	omen_sysfs_remove();
}

module_init(omen_wmi_boost_init);
module_exit(omen_wmi_boost_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HP OMEN WMI GPU power unlock");
MODULE_AUTHOR("5080_Unlock");
MODULE_VERSION("1.2");
MODULE_SOFTDEP("pre: wmi");
