# Changelog

## 0.2.0

- Vector layer: FLAT, IVF (k-means), and SLSH (bidirectional scan) ANN index types
- Format/Runtime config split (format is immutable after create; runtime is
  mutable via `reconfigure`)
- Sync + async API (`syncOnly` flag)
- Subtree mode (shared database via `VectorLayer.open` + `Subtree`)
- Iterator frame-confusion fix for tombstoned prefix entries
- `deleteSync`, `train`, `rebuild`, `count`, `reconfigure` methods