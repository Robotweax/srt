# Choose scheduler shards from measured topology

For v0.3.5, `ROBOTWEAX_SRT_SCHEDULER_SHARDS` is a process-level decimal integer from 1 to 64. Unset selects 2. Set it at process launch before the first scheduler acquisition, which may occur after `srt_startup()`. An acquired scheduler retains its configuration until final cleanup. An invalid initial setting fails acquisition. Reverify these rules in the pinned release. Each UDP channel has fixed affinity to one worker. The setting controls local scheduling independently of wire negotiation, connection groups or network bonding. [v0.3.5 scheduler guide and scorecard][shards]

| Actual target topology | Starting decision |
| --- | --- |
| Single desktop/mobile/embedded stream | Default 2. Compare 1 and 2 on the target only if CPU quota, thread count, memory or power warrants it. |
| One shared listener with many accepted peers | Default 2. Identify UDP channel ownership and the hottest worker. Adding workers leaves the busy channel on its existing affinity shard. If the channel limits service, evaluate separate channels/listeners as a topology change and test whether peers can use the changed endpoints. |
| Many independent busy UDP channels or listeners | Use 2 as the control. Compare 1/2/4/8 as CPU quota, affinity, traffic distribution and memory budget justify. Check for uneven assignment. |

Pin the Robotweax artifact, peer builds, security, addresses and roles. Keep offered load, bitrate distribution, impairment, warmup and measurement duration constant. Repeat runs serially. Record CPU quota and affinity, offered rate, delivered goodput, p99 delivery latency, application drops and SRT retransmits, per-shard CPU, RSS and thread count. Separate idle, steady state and overload. Measure on the actual target and include simultaneous Live and File traffic if deployed together. Record requested shard count and confirmation separately. Confirm using attributable scheduler instrumentation or a debugger when available. Total thread count and an environment variable alone cannot confirm worker count. State when observation is unavailable.

Choose the smallest measured count that meets service targets and headroom. Without measurements report **default, benchmark required**. The pinned scalability scorecard is a functional starting point with unencrypted integrity-checked traffic. Its completion percentiles measure transfer completion. Add actual workload and per-packet latency instrumentation for sizing claims. The published extrapolation is outside measured saturation evidence. [Performance recipes and boundaries][performance] · [Scorecard source][scorecard] · [Scheduler guide][shards]

[shards]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/scheduler-shards.md
[performance]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/performance.md
[scorecard]: https://github.com/Robotweax/srt/blob/v0.3.5/benchmarks/scalability_scorecard.py
