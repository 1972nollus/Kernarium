# Kernel families

Kernel inputs are separated by source family and version. The repository
currently supports only the existing mainline Linux 6.18 family, sourced from
the pinned upstream-stable branch:

- [`mainline-6.18/`](mainline-6.18/README.md) owns its source identity,
  generated patch series, complete configuration, build tools, and replay
  commands;
- [`common/`](common/README.md) owns contracts that are deliberately shared
  across kernel families; and
- [`CMakeLists.txt`](CMakeLists.txt) preserves the current Apple product graph
  by composing the mainline family explicitly.

No Android kernel line, AOSP source input, Android product, or Android build
workflow is present yet. A future line must be added as a separate versioned
owner and must not reuse the mainline family's generated patches, source
configuration, cache, or complete product policy.

The move from the former singleton layout does not change the current Linux
source, patch bytes, final tree, page profiles, kernel target names, or Apple
products. The old ignored `kernel/source-location.conf` remains untouched as
developer-owned data. Copy its value manually into the new ignored
`kernel/mainline-6.18/source-location.conf` when adopting this layout.

All maintained changes remain local to this project. Source repositories and
branch names are provenance only; the kernel tooling has no submission or
push workflow.
