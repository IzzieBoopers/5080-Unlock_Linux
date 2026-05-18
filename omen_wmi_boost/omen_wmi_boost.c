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

#define DRV_NAME			"omen_wmi_boost"
#define HPWMI_BIOS_GUID			"5FB7F034-2C63-45E9-BE91-3D44E2C707E4"
#define HPWMI_SIGNATURE			0x55434553
#define HPWMI_GM			0x20008

#define HPWMI_GET_GPU_THERMAL_MODES	0x21
#define HPWMI_SET_GPU_THERMAL_MODES	0x22
#define HPWMI_SET_PERFORMANCE_MODE	0x1a
#define HPWMI_FAN_COUNT_GET_QUERY	0x10
#define HPWMI_GET_SYSTEM_DESIGN_DATA	0x28

#define HP_THERMAL_VICTUS_S_PERFORMANCE	0x01
#define HP_THERMAL_OMEN_V1_PERFORMANCE	0x31

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

static struct attribute *omen_attrs[] = {
	&omen_attr_last_error.attr,
	&omen_attr_gpu_state.attr,
	&omen_attr_boost.attr,
	&omen_attr_performance.attr,
	&omen_attr_thermal_profile.attr,
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

	pr_info("board %s — %s\n",
		dmi_get_system_info(DMI_BOARD_NAME) ?: "unknown",
		dmi_get_system_info(DMI_PRODUCT_NAME) ?: "unknown");

	once = boot_mode[0] ? boot_mode : mode;

	if (persist) {
		ret = omen_sysfs_init();
		if (ret)
			return ret;

		pr_info("sysfs: /sys/kernel/%s/{gpu_state,last_error,boost,performance,thermal_profile}\n",
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
	omen_sysfs_remove();
}

module_init(omen_wmi_boost_init);
module_exit(omen_wmi_boost_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HP OMEN WMI GPU power unlock");
MODULE_AUTHOR("5080_Unlock");
MODULE_VERSION("1.1");
MODULE_SOFTDEP("pre: wmi");
