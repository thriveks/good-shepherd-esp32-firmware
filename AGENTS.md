# Good Shepherd ESP32 firmware engineering workflow

## Scoped diagnosis and repair
1. Identify the exact device family/node, firmware version, symptom and actual source-to-binary provenance. Use existing telemetry, build receipts and past verified findings; avoid repetitive fleet audits or assuming workbench source is the deployed OTA binary.
2. Trace the smallest failure using known diagnostics. Separate LD2410 GPIO21 coarse occupancy, UART frames, high-resolution evidence, server storage and customer-facing learning. Add a small reusable diagnostic only if needed.
3. Make routine, authorized changes **only** in the established development/testing tree. Preserve Mac production \`CURRENT\` pointers, manufacturing artifacts, rollback packages and unrelated functionality, including watchdog/reconnection.
4. Compile and run relevant checks, compare exact candidate and approved release digests, and report the identified physical one-device test still required. Never claim a successful command acknowledgement proves samples were captured.
5. For publication or OTA selection use the authoritative **private** firmware-control runbook \`thriveks/good-shepherd-firmware-control/docs/PUSH-TO-OTA.md\`; the public repository is distribution, not the release-selection controller.

## Approval boundary
- Routine investigation, isolated fixes and tests follow a user's fix instruction without micro-approvals. No speculative flash/reset, automatic fleet updates or release promotion.
- Physical USB test, release publication/activation and sensor-specific OTA each have distinct validation requirements; honor the exact approved workflow and retain immutable prior binaries for rollback.
- Ask only for inaccessible physical tests, destructive operations or genuine tradeoffs. Do not invent provenance, test evidence or live status.
- Email requires explicit approval of the exact final recipients and content before sending.
