#!/usr/bin/env bash
# BoxedVN - fetch one pinned fex64 component and make any failure readable
# without a token.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage: .github/fex64-fetch.sh <fex|wine|dxmt>
#
# Actions logs require authentication to download. Annotations do not, so on
# failure the tail of the output is re-emitted as ::error:: lines. That is the
# difference between a red run anyone can diagnose and one only a signed-in
# maintainer can.

set -uo pipefail

component="${1:?usage: fex64-fetch.sh <fex|wine|dxmt>}"
output="${RUNNER_TEMP:-/tmp}/fex64-fetch-${component}.log"

set +e
scripts/fetch-fex64-dependencies.sh --component "${component}" 2>&1 | tee "${output}"
status="${PIPESTATUS[0]}"
set -e

if [[ "${status}" -eq 0 ]]; then
    exit 0
fi

echo "::group::${component} fetch output"
cat "${output}"
echo "::endgroup::"

# Annotations are capped, so spend the budget on the lines that say what went
# wrong rather than on the last thing that happened to be printed. git reports
# progress right up to the failure, so a plain tail is mostly noise.
echo "::error title=fex64 ${component} fetch failed::exit status ${status}"
df -h . | tail -n 1 | while IFS= read -r line; do
    echo "::error title=fex64 ${component} disk::${line}"
done
grep -Ei 'error|fatal|denied|not found|no space|cannot|failed' "${output}" \
    | grep -v '^[[:space:]]*$' | tail -n 20 | while IFS= read -r line; do
    echo "::error title=fex64 ${component}::${line}"
done
grep -v '^[[:space:]]*$' "${output}" | tail -n 5 | while IFS= read -r line; do
    echo "::error title=fex64 ${component} tail::${line}"
done

exit "${status}"
