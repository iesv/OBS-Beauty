"""Expose CTest evidence in the Actions summary and check annotations."""
import os
import sys
from pathlib import Path

path = Path("build/Testing/Temporary/LastTest.log")
if path.is_file():
    log = path.read_text(errors="replace")
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
        summary.write("## Rendering tests\n```text\n" + log + "\n```\n")
    failed = sys.argv[1] == "failure"
    report = log[-24000:] if failed else "\n".join(
        line for line in log.splitlines()
        if line.startswith(("RENDERER:", "PASS:", "BENCH", "STRESS", "LIFECYCLE", "ALL "))
    )
    if report:
        escaped = report.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(("::error" if failed else "::notice") + " title=Rendering test log::" + escaped)
