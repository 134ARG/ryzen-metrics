## Ryzen Metrics

Collect AMD Ryzen CPU metrics (power, freq, etc.) under Linux by reading MSR.

Target CPU Family 1Ah Model 44h.

Still WIP.

### Usage

Build and load the kernel module:

```shell
$ cd src && make && sudo insmod ryzen_metrics.ko
```

You will find the metrics for each CPU under `/sys/devices/system/cpu/ryzen_metrics`.

#### `/sys/devices/system/cpu/ryzen_metrics/cpuX/effective_freq`

Provides a (relatively) precise effective frequency (MHz) of the Ryzen processor. Frequent access to this file may cause performance degradation.

#### `/sys/devices/system/cpu/ryzen_metrics/cpuX/core_power`

Provides the power consumption of each core in milliwatts. Note that the data for SMT threads is mirrored with their sibling threads, so to calculate the total power, read from only one thread per core.
