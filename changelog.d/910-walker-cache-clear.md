- **A removed disk file no longer leaves pages referenced** ([#910](https://github.com/orlyatomics/orly/issues/910)).
  Each runner pools present walkers per file and index, and a pooled walker keeps the file's pages
  referenced. Before a disk layer was removed, its walkers were cleared only under the indexes the
  file itself lists. A walker asked for an index the file has no data for still reads the file, so it
  stayed pooled after the file's blocks were freed. The next file written into those blocks failed
  `AssertNoRefCount`, which showed up as a rare abort in the TSan smoke. A walker still checked out
  when its cache was cleared also came back to a freed loader and was never deleted. Clearing now
  drops every walker for the file, and a walker returned after a clear, or on another runner, is
  deleted instead of pooled. In a 50-minute TSan loop of the smoke gate pass (172 passes per arm),
  the abort went from 35 passes to none, and blocks freed while still referenced went from about
  280 per pass to none.
