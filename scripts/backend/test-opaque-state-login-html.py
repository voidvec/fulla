#!/usr/bin/env python3
"""Check real login HTML with an independent HTML/form/query parser."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from html.parser import HTMLParser
from urllib.parse import parse_qsl, urlencode, urlsplit

CASE = "Integration_P1_OpaqueState_LoginHtmlRoundTrip"
FIELDS = {
    "client_id", "redirect_uri", "scope", "response_type", "state",
    "code_challenge", "code_challenge_method", "nonce",
}


class LoginPage(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.fields = []
        self.links = []
        self.forms = []
        self.in_form = False
        self.script_count = 0
        self.input_count = 0
        self.injection = False

    def handle_starttag(self, tag, attrs):
        names = [name for name, _ in attrs]
        if len(names) != len(set(names)):
            raise ValueError("duplicate HTML attribute")
        attributes = dict(attrs)
        if any(name.startswith("on") or name == "data-opaque-injected" for name in names):
            self.injection = True
        if tag == "script":
            self.script_count += 1
        if tag == "form":
            self.forms.append(attributes)
            self.in_form = True
        if tag == "input" and self.in_form:
            self.input_count += 1
            name = attributes.get("name")
            if name in FIELDS:
                if set(attributes) != {"type", "name", "value"}:
                    raise ValueError("OAuth hidden input gained attributes")
                if attributes["type"] != "hidden":
                    raise ValueError("OAuth field is not hidden")
                self.fields.append((name, attributes["value"]))
            elif name not in {"username", "password"}:
                raise ValueError("unexpected input in login form")
        if tag == "a":
            self.links.append(attributes.get("href"))

    def handle_endtag(self, tag):
        if tag == "form":
            self.in_form = False


def verify(directory):
    fixtures = json.loads((directory / "fixtures.json").read_text(encoding="utf-8"))
    if not isinstance(fixtures, list) or len(fixtures) != 17:
        raise ValueError("incomplete actual HTTP fixture set")
    for fixture in fixtures:
        path = directory / fixture["file"]
        if path.parent != directory or not path.is_file():
            raise ValueError("invalid or missing captured HTML")
        page = LoginPage()
        page.feed(path.read_text(encoding="utf-8"))
        page.close()
        if page.injection or page.script_count:
            raise ValueError("login HTML gained injected markup")
        if len(page.forms) != 1 or page.forms[0].get("action") != "/oauth2/login":
            raise ValueError("login form action changed")
        if page.forms[0].get("method", "").lower() != "post":
            raise ValueError("login form method changed")
        expected = fixture["fields"]
        if set(expected) != FIELDS or len(page.fields) != len(FIELDS) or page.input_count != 10:
            raise ValueError("OAuth hidden fields missing or duplicated")
        if sorted(page.fields) != sorted(expected.items()):
            raise ValueError("actual HTML form values differ from original OAuth inputs")
        if parse_qsl(urlencode(page.fields), keep_blank_values=True) != page.fields:
            raise ValueError("HTML form serialization changed opaque values")
        if page.links != [fixture["register_url"]]:
            raise ValueError("registration href changed")
        url = urlsplit(fixture["query_url"])
        pairs = parse_qsl(url.query, keep_blank_values=True)
        if url.fragment or sorted(pairs) != sorted([
            ("tenant", "alpha+beta"), ("mode", "opaque"),
            ("code", "opaque-test-code"), ("state", expected["state"]),
        ]):
            raise ValueError("query component oracle differs from urllib.parse")
    return len(fixtures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-exe", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    args = parser.parse_args()
    executable = args.test_exe.resolve(strict=True)
    config = args.config.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="fulla-opaque-state-") as temporary:
        directory = Path(temporary)
        shutil.copyfile(config, directory / "config.json")
        environment = os.environ.copy()
        environment["FULLA_OPAQUE_STATE_EVIDENCE_DIR"] = str(directory)
        try:
            # run() kills and reaps this child on timeout. The existing test
            # main owns its server lifecycle; no second server is started.
            result = subprocess.run(
                [str(executable), "-r", CASE], cwd=directory, env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=100,
            )
        except subprocess.TimeoutExpired:
            print("[opaque-state-html] test child timed out", file=sys.stderr)
            return 1
        if result.returncode:
            print("[opaque-state-html] test child failed: " + str(result.returncode), file=sys.stderr)
            return 1
        count = verify(directory)
        print(f"[opaque-state-html] {count} real HTML/form/query round trips passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError):
        print("[opaque-state-html] actual response/fixture validation failed", file=sys.stderr)
        raise SystemExit(1)
