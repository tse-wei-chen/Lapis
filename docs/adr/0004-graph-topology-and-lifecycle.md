# ADR 0004: Graph topology and runtime lifecycle

Status: Accepted for foundation.

- Graph plans are immutable directed acyclic graphs (DAGs). A cycle is rejected before any runtime resources are allocated.
- Duplicate directed edges are rejected. Node identifiers are ordinal and unique.
- A successful build checks required capabilities, then validates graph topology.
- Media processing feedback, if needed later, must be modeled as an explicit bounded control channel, not a cycle in the media data graph.
- The playback command mailbox has one reader. Cancellation of a caller's wait does not imply that an already executing native operation was rolled back.
- Future runtime adapters must define handle ownership, frame lifetime, buffer reclamation, and shutdown semantics before exposing decoded frames.
