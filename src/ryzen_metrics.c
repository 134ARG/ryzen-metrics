#include "asm/paravirt.h"
#include "asm/tsc.h"
#include "linux/device/bus.h"
#include <asm/msr.h>
#include <asm/processor.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#define MSR_MPERF 0xE7
#define MSR_APERF 0xE8

static void freq_callback(void *freq) {
  uint64_t *effective_freq = (uint64_t *)freq;
  uint64_t mperf_end = 0;
  uint64_t aperf_end = 0;

  wrmsrl(MSR_MPERF, 0);
  wrmsrl(MSR_APERF, 0);

  mdelay(10); // Wait for 10 milliseconds

  rdmsrl(MSR_MPERF, mperf_end); // Read the end value of MPERF
  rdmsrl(MSR_APERF, aperf_end); // Read the end value of APERF
  pr_info("mperf end value: %llu, aperf end value: %llu\n", mperf_end,
          aperf_end);

  *effective_freq = aperf_end * (tsc_khz / 1000) /
                    mperf_end; // Calculate effective frequency in MHz
}

// Function to calculate effective frequency for a given CPU
static int calculate_effective_freq(int cpu, uint64_t *effective_freq) {
  if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu)) {
    pr_err("ryzen_metrics: the cpu id is invalid: %d\n", cpu);
    return -EINVAL;
  }
  int ret = 0;
  if ((ret = smp_call_function_single(cpu, freq_callback, effective_freq,
                                      true))) {
    pr_err("ryzen_metrics: the cpu id is invalid: %d\n", cpu);
    return ret;
  }

  return 0;
}

// Sysfs show function for effective frequency
static ssize_t effective_freq_show(struct kobject *kobj,
                                   struct kobj_attribute *attr, char *buf) {
  int cpu, ret;
  uint64_t effective_freq;

  // Extract the CPU ID from the kobject's name
  ret = kstrtoint(kobj->name + 3, 10, &cpu); // Extract "cpuX" -> X
  if (ret) {
    pr_err("ryzen_metrics: failed to extract cpu id from: %s\n",
           kobj->parent->name);
    return ret;
  }

  ret = calculate_effective_freq(cpu, &effective_freq);
  if (ret) {
    return scnprintf(buf, PAGE_SIZE, "Error: %d\n", ret);
  }

  return scnprintf(buf, PAGE_SIZE, "%llu\n", effective_freq);
}

// Per-CPU sysfs attribute
static struct kobj_attribute effective_freq_attr = __ATTR_RO(effective_freq);

struct kobject *metrics_kobj = NULL;
struct kobject **cpu_kobjs = NULL;

// Module initialization
static int __init effective_freq_init(void) {
  int cpu, ret;
  struct device *cpu_dev;

  struct device *dev_root;
  dev_root = bus_get_dev_root(&cpu_subsys);
  if (dev_root) {
    metrics_kobj = kobject_create_and_add("ryzen_metrics", &dev_root->kobj);
    put_device(dev_root);
    if (metrics_kobj == NULL) {
      pr_err("ryzen_metrics: failed to create ryzen_metrics kobject\n");
      return -ENOMEM;
    }
  } else {
    pr_err("ryzen_metrics: failed to get root device!\n");
    return -ENOMEM;
  }

  cpu_kobjs = kmalloc(num_online_cpus() * sizeof(struct kobject *), GFP_KERNEL);
  if (cpu_kobjs == NULL) {
    pr_err("ryzen_metrics: failed to allocate memory for cpu_kobjs\n");
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

    // Allocate memory for a new kobject and initialize it with cpu_dev's name
    cpu_kobjs[cpu] = kobject_create_and_add(cpu_dev->kobj.name, metrics_kobj);
    if (!cpu_kobjs[cpu]) {
      pr_err("ryzen_metrics: failed to create directory for CPU %d under "
             "'ryzen_metrics'\n",
             cpu);
      continue;
    }

    // Create the effective_freq file under
    // /sys/devices/system/cpu/ryzen_metrics/
    ret = sysfs_create_file(cpu_kobjs[cpu], &effective_freq_attr.attr);
    if (ret) {
      pr_err("Failed to create effective_freq file for CPU %d\n", cpu);
      continue;
    }
  }

  pr_info("Effective Frequency module loaded\n");
  return 0;
}

// Module exit
static void __exit effective_freq_exit(void) {
  int cpu;
  struct device *cpu_dev;
  // struct kobject *metrics_kobj;

  pr_info("Unloading Effective Frequency module...\n");

  for_each_online_cpu(cpu) {
    // Get the device for the current CPU
    cpu_dev = get_cpu_device(cpu);
    if (!cpu_dev) {
      pr_err("Failed to get device for CPU %d during cleanup\n", cpu);
      continue;
    }

    // Remove the effective_freq file
    sysfs_remove_file(cpu_kobjs[cpu], &effective_freq_attr.attr);

    // Decrement the reference count
    kobject_put(cpu_kobjs[cpu]);
  }
  kfree(cpu_kobjs); // Free the allocated memory
  kobject_put(metrics_kobj);
  pr_info("Effective Frequency module successfully unloaded\n");
}

module_init(effective_freq_init);
module_exit(effective_freq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("134ARG");
MODULE_DESCRIPTION(
    "A kernel module to expose AMD Ryzen's per-CPU metrics via sysfs");
