#include "asm-generic/errno.h"
#include "asm/paravirt.h"
#include "asm/tsc.h"
#include "linux/device/bus.h"
#include "linux/printk.h"
#include "linux/types.h"
#include <asm/msr.h>
#include <asm/processor.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#define MSR_MPERF 0xE7
#define MSR_APERF 0xE8

struct cpu_kobject {
  struct kobject kobj;
  int cpuid;
  struct list_head list;
};

struct cpu_kobject_attribute {
  struct attribute attr;
  ssize_t (*show)(struct cpu_kobject *, struct cpu_kobject_attribute *, char *);
  ssize_t (*store)(struct cpu_kobject *, struct cpu_kobject_attribute *,
                   const char *, size_t);
};

static void cpu_kobj_release(struct kobject *kobj) {
  struct cpu_kobject *cpu = container_of(kobj, struct cpu_kobject, kobj);
  list_del(&cpu->list);
  kfree(cpu);
}

#define to_cpu_kobj_attr(x) container_of(x, struct cpu_kobject_attribute, attr)
#define to_cpu_kobj(x) container_of(x, struct cpu_kobject, kobj)

static struct kobject *metrics_kobj = NULL;
static LIST_HEAD(cpu_kobj_list);

static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf) {
  struct cpu_kobject *cpu_kobj;
  struct cpu_kobject_attribute *attribute;

  cpu_kobj = to_cpu_kobj(kobj);
  attribute = to_cpu_kobj_attr(attr);

  if (!attribute->show)
    return -EIO;

  return attribute->show(cpu_kobj, attribute, buf);
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
                     const char *buf, size_t len) {
  struct cpu_kobject *cpu_kobj;
  struct cpu_kobject_attribute *attribute;

  cpu_kobj = to_cpu_kobj(kobj);
  attribute = to_cpu_kobj_attr(attr);

  if (!attribute->store)
    return -EIO;

  return attribute->store(cpu_kobj, attribute, buf, len);
}

static const struct sysfs_ops sysfs_ops = {
    .show = show,
    .store = store,
};

static struct kobj_type cpu_ktype = {
    .sysfs_ops = &sysfs_ops,
    .release = cpu_kobj_release,
};

static int add_cpu_kobject(int cpuid, struct cpu_kobject **ptr) {
  if (!metrics_kobj) {
    pr_err("ryzen_metrics: parent kobject not initialized\n");
    return -ENAVAIL;
  }

  struct cpu_kobject *cpu_obj = kzalloc(sizeof(struct cpu_kobject), GFP_KERNEL);
  if (!cpu_obj) {
    return -ENOMEM;
  }
  cpu_obj->cpuid = cpuid;
  if (kobject_init_and_add(&cpu_obj->kobj, &cpu_ktype, metrics_kobj, "cpu%d",
                           cpuid)) {
    kobject_put(&cpu_obj->kobj); // Cleanup on failure
    return -EINVAL;
  }

  // Add to the linked list
  list_add_tail(&cpu_obj->list, &cpu_kobj_list);
  *ptr = cpu_obj;

  pr_info("CPU kobject for CPU %d added\n", cpuid);
  return 0;
}

static void cleanup_all_cpu_kobjects(void) {
  struct cpu_kobject *cpu, *tmp;
  list_for_each_entry_safe(cpu, tmp, &cpu_kobj_list, list) {
    kobject_put(&cpu->kobj);
  }
}

struct truncated_perf_value {
  uint64_t mperf;
  uint64_t aperf;
};

static void freq_callback(void *freq) {
  struct truncated_perf_value *perf_values =
      (struct truncated_perf_value *)freq;
  uint64_t mperf = 0;
  uint64_t aperf = 0;

  rdmsrl(MSR_MPERF, mperf); // Read the end value of MPERF
  rdmsrl(MSR_APERF, aperf); // Read the end value of APERF
  perf_values->mperf = mperf;
  perf_values->aperf = aperf;
}

// Function to calculate effective frequency for a given CPU
static int calculate_effective_freq(int cpu, uint64_t *effective_freq) {
  if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu)) {
    pr_err("ryzen_metrics: the cpu id is invalid: %d\n", cpu);
    return -EINVAL;
  }
  struct truncated_perf_value perf_values_start = {0}, perf_values_end = {0};
  int ret = 0;
  int retry_count = 3;

  uint64_t mperf_diff = 1;
  uint64_t aperf_diff = 0;

  while (retry_count > 0) {
    if ((ret = smp_call_function_single(cpu, freq_callback, &perf_values_start,
                                        true))) {
      pr_err("ryzen_metrics: the cpu id is invalid: %d\n", cpu);
      return ret;
    }

    msleep(5);

    if ((ret = smp_call_function_single(cpu, freq_callback, &perf_values_end,
                                        true))) {
      pr_err("ryzen_metrics: the cpu id is invalid: %d\n", cpu);
      return ret;
    }

    uint64_t mperf_start = perf_values_start.mperf;
    uint64_t mperf_end = perf_values_end.mperf;
    uint64_t aperf_start = perf_values_start.aperf;
    uint64_t aperf_end = perf_values_end.aperf;
    pr_info("mperf start value: %llu, mperf end value: %llu, aperf start "
            "value: %llu, aperf end value: %llu\n",
            mperf_start, mperf_end, aperf_start, aperf_end);

    mperf_diff = mperf_end - mperf_start;
    aperf_diff = aperf_end - aperf_start;

    if (mperf_start >= mperf_end || aperf_start >= aperf_end) {
      pr_info("ryzen_metrics: overflow encountered. retrying...");
      retry_count--;
      continue;
    }

    pr_info("mperf diff: %llu, aperf diff: %llu\n", mperf_diff, aperf_diff);
    break;
  }

  *effective_freq = (aperf_diff) * (tsc_khz / 1000) /
                    (mperf_diff); // Calculate effective frequency in MHz

  return 0;
}

// Sysfs show function for effective frequency
static ssize_t effective_freq_show(struct cpu_kobject *kobj,
                                   struct cpu_kobject_attribute *attr,
                                   char *buf) {
  int cpu, ret;
  uint64_t effective_freq;

  // Extract the CPU ID from the kobject
  cpu = kobj->cpuid;

  ret = calculate_effective_freq(cpu, &effective_freq);
  if (ret) {
    return scnprintf(buf, PAGE_SIZE, "Error: %d\n", ret);
  }

  return scnprintf(buf, PAGE_SIZE, "%llu\n", effective_freq);
}

// Per-CPU sysfs attribute
static struct cpu_kobject_attribute effective_freq_attr =
    __ATTR_RO(effective_freq);

#define MSR_PWR_UNIT 0xC0010299
#define MSR_CORE_ENERGY 0xC001029A
#define MSR_PACKAGE_ENERGY 0xC001029B

#define POOLING_INTERVAL_MS 10
#define ENERGY_MUTLIPLIER (1000 * 1000 / POOLING_INTERVAL_MS)

struct rapl_power_unit {
  uint8_t power_unit : 8;
  uint8_t energy_unit : 8;
  uint8_t time_unit : 8;
  uint64_t _ : 40;
};

static int calculate_power(int cpuid, uint64_t source_reg,
                           uint64_t *power_in_mw) {
  struct rapl_power_unit power_unit;
  int ret;
  if ((ret =
           rdmsrl_safe_on_cpu(cpuid, MSR_PWR_UNIT, (uint64_t *)&power_unit))) {
    pr_alert("ryzen_metrics: Failed to read power unit");
    return ret;
  }

  uint64_t inverse_energy_unit_d = (1llu << power_unit.energy_unit);
  uint64_t raw_core_energy_begin = 0, raw_core_energy_end = 0;

  if ((ret = rdmsrl_safe_on_cpu(cpuid, source_reg, &raw_core_energy_begin))) {
    pr_alert("ryzen_metrics: Failed to read core energy");
    return ret;
  }

  msleep(POOLING_INTERVAL_MS);

  if ((ret = rdmsrl_safe_on_cpu(cpuid, source_reg, &raw_core_energy_end))) {
    pr_alert("ryzen_metrics: Failed to read core energy");
    return ret;
  }

  *power_in_mw = (raw_core_energy_end - raw_core_energy_begin) *
                 ENERGY_MUTLIPLIER / inverse_energy_unit_d;

  return 0;
}

static ssize_t core_power_show(struct cpu_kobject *kobj,
                               struct cpu_kobject_attribute *attr, char *buf) {
  int cpuid = kobj->cpuid;
  int ret;

  uint64_t core_power_in_mw = 0;

  if ((ret = calculate_power(cpuid, MSR_CORE_ENERGY, &core_power_in_mw))) {
    pr_alert("ryzen_metrics: Failed to calculate the power");
    return scnprintf(buf, PAGE_SIZE, "Error: %d\n", ret);
  }

  return scnprintf(buf, PAGE_SIZE, "%llu\n", core_power_in_mw);
}

static struct cpu_kobject_attribute core_power_attr = __ATTR_RO(core_power);

static ssize_t package_power_show(struct cpu_kobject *kobj,
                                  struct cpu_kobject_attribute *attr,
                                  char *buf) {
  int ret;

  uint64_t core_power_in_mw = 0;

  if ((ret = calculate_power(0, MSR_PACKAGE_ENERGY, &core_power_in_mw))) {
    pr_alert("ryzen_metrics: Failed to calculate the power");
    return scnprintf(buf, PAGE_SIZE, "Error: %d\n", ret);
  }

  return scnprintf(buf, PAGE_SIZE, "%llu\n", core_power_in_mw);
}

static struct cpu_kobject_attribute package_power_attr =
    __ATTR_RO(package_power);

// Module initialization
static int __init ryzen_metrics_init(void) {
  int cpu, ret;
  struct device *cpu_dev;

  struct device *dev_root;
  dev_root = bus_get_dev_root(&cpu_subsys);

  if (!dev_root) {
    pr_err("ryzen_metrics: failed to get root device!\n");
    return -EINVAL;
  }

  metrics_kobj = kobject_create_and_add("ryzen_metrics", &dev_root->kobj);
  put_device(dev_root);
  if (metrics_kobj == NULL) {
    pr_err("ryzen_metrics: failed to create ryzen_metrics kobject\n");
    cleanup_all_cpu_kobjects();
    kobject_put(metrics_kobj);
    return -ENOMEM;
  }

  ret = sysfs_create_file(metrics_kobj, &package_power_attr.attr);
  if (ret) {
    pr_err("Failed to create package_power file for CPU\n");
    cleanup_all_cpu_kobjects();
    kobject_put(metrics_kobj);
    return ret;
  }

  for_each_online_cpu(cpu) {
    // Get the device for the current CPU
    cpu_dev = get_cpu_device(cpu);
    if (!cpu_dev) {
      pr_err("Failed to get device for CPU %d\n", cpu);
      continue;
    }

    struct cpu_kobject *cpu_obj = NULL;
    ret = add_cpu_kobject(cpu, &cpu_obj);
    if (ret) {
      pr_err("ryzen_metrics: failed to create CPU %d kobject\n", cpu);
      cleanup_all_cpu_kobjects();
      kobject_put(metrics_kobj);
      return ret;
    }

    // Create the effective_freq file under
    // /sys/devices/system/cpu/ryzen_metrics/cpuX/
    ret = sysfs_create_file(&cpu_obj->kobj, &effective_freq_attr.attr);
    if (ret) {
      pr_err("Failed to create effective_freq file for CPU %d\n", cpu);
      cleanup_all_cpu_kobjects();
      kobject_put(metrics_kobj);
      return ret;
    }

    ret = sysfs_create_file(&cpu_obj->kobj, &core_power_attr.attr);
    if (ret) {
      pr_err("Failed to create core_power file for CPU %d\n", cpu);
      cleanup_all_cpu_kobjects();
      kobject_put(metrics_kobj);
      return ret;
    }
  }

  pr_info("Effective Frequency module loaded\n");
  return 0;
}

// Module exit
static void __exit ryzen_metrics_exit(void) {
  struct cpu_kobject *cpu_obj, *tmp;
  list_for_each_entry_safe(cpu_obj, tmp, &cpu_kobj_list, list) {
    sysfs_remove_file(&cpu_obj->kobj, &effective_freq_attr.attr);
    sysfs_remove_file(&cpu_obj->kobj, &core_power_attr.attr);
  }
  cleanup_all_cpu_kobjects();

  sysfs_remove_file(metrics_kobj, &package_power_attr.attr);
  kobject_put(metrics_kobj);

  pr_info("Effective Frequency module successfully unloaded\n");
}

module_init(ryzen_metrics_init);
module_exit(ryzen_metrics_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("134ARG");
MODULE_DESCRIPTION(
    "A kernel module to expose AMD Ryzen's per-CPU metrics via sysfs");
