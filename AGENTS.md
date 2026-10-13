# Good Shepherd firmware engineering workflow

The user expects root-cause investigation and a tested repair, not repeated confirmation requests or open-ended commentary.

1. Verify device family, node identity, actual firmware release label, exact source/binary relationship, and reproducible symptom before modifying code.
2. Confirm historical vs current production behavior from real telemetry; distinguish LD2410 GPIO occupancy from UART frames, high-resolution evidence and server learning.
3. Reuse established diagnostic commands where possible. Provide a small, safe diagnostic tool when one is missing rather than requiring the user to infer hardware state from the dashboard.
4. Develop only in the established **development/testing** tree. Preserve protected production release artifacts and the Mac `CURRENT` pointers. Do not treat Git workbench source as verified source for an OTA production binary unless hash/build provenance is established.
5. Do not flash, reflash, promote to production, repoint `CURRENT`, or publish OTA solely because a plausible change was made. Require a build, comparison, test evidence, rollback plan and approved production release workflow.
6. Protect coarse GPIO21 presence detection and watchdog/reconnection functionality while investigating UART or high-res capture failures. Do not change unrelated behavior.
7. Provide precise device-specific test instructions, results, firmware/binary identity and any remaining physical steps. Never claim a command acknowledgement proves samples were captured or delivered.

Routine read-only diagnostics and authorized focused changes should proceed without micro-approval. Reserve questions for physical access, irreversible actions and real tradeoffs. Email requires explicit approval of final content and recipient before sending.
