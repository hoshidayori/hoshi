"""The SAS emoji table (the spec's sas-emoji.json) that Element Web bundles. Run directly, prints number,
emoji, description and Russian description, one per line. sas_interop.py checks core/sas.cpp against it.
Usage: sas_emoji_from_element.py PATH_TO_ELEMENT_INIT_JS"""

import json
import sys


def load(bundle_path):
    bundle = open(bundle_path, encoding="utf-8").read()
    start = bundle.index('[{"number":0,')
    # The table sits in a single-quoted JavaScript string, so its apostrophes are escaped
    table, _ = json.JSONDecoder().raw_decode(bundle[start:].replace("\\'", "'"))
    return table


if __name__ == "__main__":
    for entry in load(sys.argv[1]):
        ru = entry["translated_descriptions"].get("ru") or ""
        print(f'{entry["number"]}\t{entry["emoji"]}\t{entry["description"]}\t{ru}')
