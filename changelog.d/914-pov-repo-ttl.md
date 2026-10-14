- **An expired POV's repo goes when the POV does** ([#914](https://github.com/orlyatomics/orly/issues/914)).
  After the layer cleaner began discarding cached repos at their deadline, a POV's repo still
  outlived the POV by one TTL: it closed only when the expired POV let go of it, then entered the
  repo cache with a fresh deadline of its own. So about two TTLs' worth of POVs kept their repos,
  each holding a Data Layer block, a Repo Mapping block and a WAL pin. An expired POV's repo is now
  destroyed when its last pointer goes, instead of cached.
