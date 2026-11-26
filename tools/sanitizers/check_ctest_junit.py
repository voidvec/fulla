#!/usr/bin/env python3
"""Reject empty, skipped, or not-run tests in CTest JUnit reports."""

import argparse
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


SKIPPED_STATUSES = {"notrun", "skipped", "disabled"}


def find_skipped_tests(report_path):
    root = ET.parse(report_path).getroot()
    testcases = list(root.iter("testcase"))
    if not testcases:
        raise ValueError(f"empty JUnit report: {report_path}")
    skipped = []
    for testcase in testcases:
        has_skipped_element = any(element.tag == "skipped" for element in testcase.iter())
        status = (testcase.get("status") or "").strip().lower()
        if has_skipped_element or status in SKIPPED_STATUSES:
            name = testcase.get("name") or "<unnamed>"
            classname = testcase.get("classname")
            skipped.append(f"{classname}.{name}" if classname else name)
    return skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("junit", type=Path, nargs="+")
    args = parser.parse_args()
    try:
        for report in args.junit:
            skipped = find_skipped_tests(report)
            if skipped:
                print(f"CTest JUnit contains skipped or not-run tests: {report}", file=sys.stderr)
                for name in skipped:
                    print(f"  {name}", file=sys.stderr)
                return 1
    except (OSError, ET.ParseError, ValueError) as error:
        print(f"CTest JUnit verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
