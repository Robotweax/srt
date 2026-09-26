# Bounded payload storage

Send and receive buffers reserve their complete configured packet capacity at
construction. Packet metadata lives separately from payload bytes. Payload slots
are assigned from a bounded pool and recycled independently of the sequence ring.
This avoids initializing or eventually touching every payload page simply because
a long-running stream traverses the ring with low occupancy.

The implementation allocates the full payload arena, length metadata and free-index
storage up front. Acquiring, replacing and releasing a payload slot require no
heap allocation. A slot always has room for the maximum wire payload, including
an authentication tag; the application payload-size option does not shrink this
storage. Buffer sizes, flow windows, packetization, wire format and the public C
ABI are unchanged. Native C++ implementation layouts require matching headers and
libraries, as with other internal implementation changes.

## Ownership and recovery

- A send packet owns its payload until acknowledged or discarded. Retransmissions
  refer to the retained protected bytes, including any authentication tag.
- Expired send messages can retain sequence/drop metadata without retaining a
  payload slot. A later acknowledgement of that marker cannot free a new owner.
- Receive fragments and out-of-order packets retain their slots until delivery or
  discard. A partial stream read keeps ownership until the final byte is consumed.
- Copies own independent payload storage and copy only initialized payload bytes.
  Copy assignment leaves the destination unchanged if allocation fails. Moves
  transfer ownership; existing borrowed-view invalidation rules remain in force.

## Memory bounds and measurement

This changes the resident working set, not the configured packet capacity or the
full payload reservation. The separate indices and length metadata add a small
bounded overhead when compared with fully populated inline buffers. Operating
systems and allocators differ in how reservations affect commitment and RSS.

Touched payload pages are reused. This version does not explicitly decommit pages
after a traffic spike, so RSS can follow historical peak occupancy. First use of
previously untouched pages may incur page faults. Measure idle, sustained occupancy,
multiple ring rotations and full occupancy separately. An empty-buffer RSS result
is not a loaded-connection limit or a portable throughput claim.

Focused buffer/session regressions cover sequence wrap, full capacity, partial
stream reads, drops, retransmission, protected payloads and independent copies.
Capacity and runtime allocation measurements should use each revision's matching
private headers and library. Network-level performance and interoperability still
need their own platform-specific evidence.
