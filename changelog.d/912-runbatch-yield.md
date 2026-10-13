- **A read that parks on a runner no longer waits out a whole batch there** ([#912](https://github.com/orlyatomics/orly/issues/912)).
  A read that misses the page cache resumes on the runner it started on. If a batch started there
  in the meantime, the read waited until all of the batch's calls had run. A batch now yields every
  32 calls, so the read resumes between them. Under back-to-back 4,096-call Edits on 16 CPUs, at 16
  sessions, point-read p99.9 fell from about 70 ms to about 42 ms and the worst read from about
  440 ms to 190–300 ms; expand moved the same way. Range reads barely changed, since their slow
  reads weren't waiting on a batch. With no readers, the Edit rate is unchanged (3.24/s before,
  3.23/s after).
