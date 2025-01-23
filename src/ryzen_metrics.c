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

    if (mperf_start > mperf_end || aperf_start > aperf_end) {
      pr_info("ryzen_metrics: overflow encountered. retrying...");
      retry_count--;
      continue;
    }
    uint64_t mperf_diff = mperf_end - mperf_start;
    uint64_t aperf_diff = aperf_end - aperf_start;
    pr_info("mperf diff: %llu, aperf diff: %llu\n", mperf_diff, aperf_diff);
    break;
  }

  *effective_freq =
      (perf_values_end.aperf - perf_values_start.aperf) * (tsc_khz / 1000) /
      (perf_values_end.mperf -
       perf_values_start.mperf); // Calculate effective frequency in MHz

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

// Module initialization
static int __init ryzen_metrics_init(void) {
  int cpu;
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

  for_each_online_cpu(cpu) {
    // Get the device for the current CPU
    cpu_dev = get_cpu_device(cpu);
    if (!cpu_dev) {
      pr_err("Failed to get device for CPU %d\n", cpu);
      continue;
    }

    struct cpu_kobject *cpu_obj = NULL;
    int ret = add_cpu_kobject(cpu, &cpu_obj);
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
  }

  pr_info("Effective Frequency module loaded\n");
  return 0;
}

// Module exit
static void __exit ryzen_metrics_exit(void) {
  struct cpu_kobject *cpu_obj, *tmp;
  list_for_each_entry_safe(cpu_obj, tmp, &cpu_kobj_list, list) {
    sysfs_remove_file(&cpu_obj->kobj, &effective_freq_attr.attr);
  }

  cleanup_all_cpu_kobjects();
  kobject_put(metrics_kobj);
  pr_info("Effective Frequency module successfully unloaded\n");
}

module_init(ryzen_metrics_init);
module_exit(ryzen_metrics_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("134ARG");
MODULE_DESCRIPTION(
    "A kernel module to expose AMD Ryzen's per-CPU metrics via sysfs");
