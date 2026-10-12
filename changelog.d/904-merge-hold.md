- **Merges no longer pin flushed memory layers** ([#904](https://github.com/orlyatomics/orly/issues/904)).
  A memory or disk merge used to hold the repo's whole mapping until it finished, which kept every
  memory layer flushed in the meantime in the Update Entry pool, and under sustained large writes
  turned into admission refusals. A merge now references only the layers it reads and releases the
  mapping as soon as it has chosen them.
