- **Memory merges get their own cores by default** ([#904](https://github.com/orlyatomics/orly/issues/904)).
  The default core layout used to pin the 3 memory-merge threads and the 8 disk-merge threads to
  the same cores (on 16 CPUs, just cores 3 and 11). A root flush, which is what frees the Update
  pools, then waited behind long disk merges. Memory layers piled up, and sustained large writes were
  refused. The flush took 13–16 s, and Edits that were refused took up to 4.5 s. Memory merges now
  run on cores of their own in every CPU tier. From 16 CPUs up, those cores come out of the slow
  set: for example, on 16 CPUs slow is 0,1,8,9, memory merge is 2,10 and disk merge is 3,11. On
  32–63 CPUs, the slow set no longer overlaps the merge cores either. `--mem_merge_cores`,
  `--disk_merge_cores` and `--slow_cores` still override the defaults, and the resolved layout is
  logged at startup.
