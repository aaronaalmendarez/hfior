# Versioning

HFIOR is in the `0.x` research stage. The current shared-memory ABI reports
version `2`; source APIs, control messages, feature bits, and integration XML
may change incompatibly.

Consumers must check magic, ABI version, header size, record size, capacity,
features, and generation. Unknown required features fail closed. Stable ABI/API
claims require independent implementations, documented extension rules, and a
security/lifecycle review.
