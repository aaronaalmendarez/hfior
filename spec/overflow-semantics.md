# Overflow semantics

Preservation claims apply only while unread depth is at most ring capacity.

The reference producer uses **drop newest**:

- it leaves unread records intact;
- it increments an atomic drop counter;
- the missing sequence becomes detectable downstream; and
- the run fails ordinary integrity validation.

An implementation must not silently overwrite unread history or report a
lossless result after `SYN_DROPPED`, overflow, a sequence gap, duplication, or
reordering. Capacity planning and prompt post-use reclamation reduce overflow
risk but do not replace explicit accounting.
