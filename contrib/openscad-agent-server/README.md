# OpenSCAD agent HTTP access

Enable **Window > Agent Access**, copy connection info and follow [SKILL.md](SKILL.md).
Connect with any HTTP tool; no dedicated client or Python installation is needed.

GET /v1/info links to the live operation catalog (/v1/help), record schemas (/v1/schema) and complete
contract (/v1/help/contract). These are embedded in the executable and work outside a source checkout.
Operation receipts supply polling routes; annotation and capture metadata link directly to PNG downloads.

The service remains numeric-loopback only with explicit server/target identities. It does not provide source
writes, forced evaluation, saving, document opening or arbitrary execution. [HTTP.md](HTTP.md) is the offline contract.
