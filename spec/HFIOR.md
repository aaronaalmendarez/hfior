# HFIOR experimental specification

Status: **Experimental / unstable**. This document defines architecture-level
requirements; the C ABI is version 2 and is not promised stable.

## Required roles

An implementation has a Producer, Authorization Authority, Shared Observation
Stream, Consumer, and Urgent Event Path. One component may implement multiple
roles, but authorization and ownership semantics remain explicit.

## Required properties

1. Every accepted observation has an ordered sequence and timestamp.
2. Published records are immutable.
3. The consumer can recover all unread accepted records in order.
4. Loss, overflow, disconnect, source reset, and generation change are visible.
5. Motion consumption does not require a consumer wake or expensive callback
   for every record.
6. Urgent discrete transitions have a separately specified immediate path.
7. Stream access is granted and revocable by an appropriate authority.
8. Requested rate, observed rate, wake rate, consumer action rate, and frame
   rate are reported as distinct measurements.

## Non-requirements

HFIOR does not require Linux, Wayland, shared memory, a kernel producer, a
specific ring capacity, or frame-based consumers. A conforming implementation
may use another transport if it preserves these semantics.

Read the [producer](producer-model.md), [consumer](consumer-model.md),
[record](record-format.md), [overflow](overflow-semantics.md), and
[authorization](authorization.md) documents together.
