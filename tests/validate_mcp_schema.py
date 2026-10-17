"""Validate native production fixtures against the unmodified MCP 2.0 schema.

Requires jsonschema >= 4.18. The schema is deliberately supplied as a file so
validation never quietly downloads a different protocol version.

  python validate_mcp_schema.py --schema path/to/schema.json \
      --fixtures work/mcp-native-tests/mcp-schema-fixtures.json

The native harness emits actual dot_mcp_dispatch input/output for core MCP RPCs.
MCP Events remains an extension: its lifecycle/security checks run in test_mcp.c,
and its shapes are checked against the OpenAI integration contract there.
"""
import argparse
import json
from pathlib import Path

from jsonschema import Draft202012Validator


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()
    schema = json.loads(args.schema.read_text(encoding="utf-8-sig"))
    fixtures = json.loads(args.fixtures.read_text(encoding="utf-8-sig"))
    if not fixtures:
        raise SystemExit("No production fixtures were generated")
    checked = set()
    for fixture in fixtures:
        definition = fixture["schema"]
        if definition not in schema["$defs"]:
            raise SystemExit(f"Unknown MCP schema definition: {definition}")
        validation_schema = {
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "$defs": schema["$defs"],
            "$ref": f"#/$defs/{definition}",
        }
        validator = Draft202012Validator(validation_schema)
        errors = list(validator.iter_errors(fixture["value"]))
        if errors:
            details = "\n".join(f"{error.json_path}: {error.message}" for error in errors)
            raise SystemExit(f"MCP {definition} failed schema validation:\n{details}")
        checked.add(definition)
    print(f"Validated {len(fixtures)} production fixtures against MCP 2.0 schema: "
          + ", ".join(sorted(checked)))


if __name__ == "__main__":
    main()
