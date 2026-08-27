# Security policy

HFIOR is experimental and has no stable production ABI. Do not deploy the
reference bridge as an ambient, multi-user raw-input service.

Report vulnerabilities privately through GitHub's private vulnerability
reporting feature. Include affected revision, threat model, reproduction, and
impact. Do not attach raw input traces containing sensitive user activity to a
public issue.

Security-sensitive areas include authorization, descriptor transfer, mapping
permissions, focus/seat lifetime, revocation, ACK validation, integer bounds,
overflow, generation changes, and sandbox escape surfaces. See
[`docs/security-model.md`](docs/security-model.md).
