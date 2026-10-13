- **Reads no longer wait behind a concurrent batch** ([#908](https://github.com/orlyatomics/orly/issues/908)).
  Statements went to the fast runners in blind rotation, and a fiber doesn't yield inside a batch,
  so under ingest each reading session landed behind a running Edit about once per Edit and waited
  for the rest of it. Each statement now goes to the fast runner with the least work in flight,
  and a batch counts as many units of work as it has calls, so reads steer around it while it
  runs. Under back-to-back 4,096-call Edits on 16 CPUs, point-read p99 fell from 271 ms to 8.5 ms
  at 1 session and from 319 ms to 17 ms at 16 sessions, and the server answered 7 to 9 times as
  many reads in the same time. With no readers, the Edit rate is unchanged (3.31/s before, 3.33/s
  after); serving the extra reads costs the Edits about 5% (3.15/s to 3.00/s).
