- **Key-only range counts:** Recognise `(keys ... reduce (start 0) + 1)` without
  decoding native key results or folding stored values. Counts preserve POV
  visibility, deletions, and per-read budgets; other reductions are unchanged.
  Document when to use a range count versus a maintained `+=` counter. Whole-file
  count shortcuts remain a follow-up; there is no language or disk-format change
  ([#749](https://github.com/orlyatomics/orly/issues/749)).
