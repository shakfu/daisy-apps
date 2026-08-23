#!/usr/bin/env bash
# Fail if a source comment or README cites a docs/*.md that does not exist and is not a known,
# catalogued absence.
#
# Engine sources are ported from sk-engines verbatim and cite that project's design notes, which were
# not ported with them. Rewriting those citations would mean editing ported sources, which is exactly
# what docs/dev/porting-sk-engines.md says not to do - so the inherited dead links are listed in
# docs/dev/README.md and tolerated here. The point of this check is that a NEW one cannot be added
# quietly.
#
#   scripts/check_docs.sh
set -euo pipefail

cd "$(dirname "$0")/.."

INDEX=docs/dev/README.md
[ -f "$INDEX" ] || { echo "check_docs: $INDEX is missing - it is the catalogue of known absences"; exit 1; }

fail=0
missing=$(grep -rhoE 'docs/(dev/)?[a-z0-9._-]+\.md' \
              --include='*.h' --include='*.cpp' --include='*.cc' --include='*.md' \
              src app pod scripts README.md 2>/dev/null \
          | sort -u \
          | while read -r doc; do [ -f "$doc" ] || echo "$doc"; done)

for doc in $missing; do
    # Catalogued in the index? (the table cites each one in backticks)
    if ! grep -qF "\`$doc\`" "$INDEX"; then
        echo "check_docs: '$doc' is referenced but does not exist, and is not listed in $INDEX"
        fail=1
    fi
done

# The reverse direction: an entry that has since been written should be removed from the catalogue,
# so the list does not rot into a list of things that are actually fine.
while read -r doc; do
    if [ -f "$doc" ]; then
        echo "check_docs: '$doc' now EXISTS but is still listed as absent in $INDEX - remove that row"
        fail=1
    fi
done < <(grep -oE '`docs/(dev/)?[a-z0-9._-]+\.md`' "$INDEX" | tr -d '`' | sort -u)

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "check_docs: ok ($(echo "$missing" | grep -c . || true) catalogued absences, no new dead links)"
