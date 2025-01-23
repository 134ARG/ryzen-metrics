## Ryzen Metrics

Collect AMD Ryzen CPU metrics (power, freq, etc.) under Linux by reading MSR.

Target CPU Family 1Ah Model 44h.

Still WIP.

### Usage

Build and load the kernel module:

```shell
❯ cd src && make && sudo insmod ryzen_metrics.ko
```

You will find the metrics for each CPU under `/sys/devices/system/cpu/ryzen_metrics`.

E.g.,
```shell
❯ ls /sys/devices/system/cpu/ryzen_metrics
cpu0  cpu10  cpu12  cpu14  cpu16  cpu18  cpu2   cpu21  cpu23  cpu25  cpu27  cpu29  cpu30  cpu4  cpu6  cpu8
cpu1  cpu11  cpu13  cpu15  cpu17  cpu19  cpu20  cpu22  cpu24  cpu26  cpu28  cpu3   cpu31  cpu5  cpu7  cpu9
```

For each cpu, we currently have these sysfs files:

```shell
❯ ls /sys/devices/system/cpu/ryzen_metrics/cpu0
core_power  effective_freq
```

#### /sys/devices/system/cpu/ryzen_metrics/cpuX/effective_freq

Provides a (relatively) precise effective frequency (MHz) of the Ryzen processor. Frequent access to this file may cause performance degradation.

E.g.,

```shell
❯ sudo cat /sys/devices/system/cpu/ryzen_metrics/cpu0/effective_freq
4395
```

#### /sys/devices/system/cpu/ryzen_metrics/cpuX/core_power

Provides the power consumption of each core in milliwatts. Note that the data for SMT threads is mirrored with their sibling threads since they share the same MSR, so read from only one thread per core when calculate the total power.

E.g.,

```shell
❯ sudo cat /sys/devices/system/cpu/ryzen_metrics/cpu0/core_power
291
```
