## Ryzen Metrics

Collect AMD Ryzen CPU metrics (power, freq, etc.) under Linux by reading MSR. 

Target CPU Family 1Ah Model 44h.

Still WIP.

### Usage

Build and load the kernel module:

```shell
$ cd src && make && sudo insmod ryzen_metrics.ko
```

You will find the metrics in `/sys/devices/system/cpu/ryzen-metrics` and `/sys/devices/system/cpu/cpu*/ryzen-metrics`.
