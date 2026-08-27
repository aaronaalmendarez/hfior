# Reference backend: Aquamarine

Status: **design and exploratory prototype work**.

Aquamarine is the first backend target for preserving source timestamps and
ordered observations on their way to the compositor. A focused integration
should not embed application authorization policy in the backend. It should
provide faithful source metadata and lifecycle signals, leaving the compositor
to grant and revoke client access.

The first patch should establish timestamp fidelity independently. Ring
publication and instrumentation should follow in separate changes so reviewers
can validate behavior and performance without a monolithic compositor patch.
