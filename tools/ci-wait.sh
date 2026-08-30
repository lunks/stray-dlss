#!/bin/bash
# Wait for the CI run belonging to the CURRENT HEAD commit, and report per-job results.
#
# `gh run list --limit 1` returns whatever run is newest at that instant, which right after a
# push is often the PREVIOUS commit's run. Watching that and reporting success is how a broken
# build gets called clean. Match on the SHA instead.
set -uo pipefail

SHA=$(git rev-parse HEAD)
REPO=${REPO:-lunks/stray-dlss}
DEADLINE=$(( $(date +%s) + ${TIMEOUT:-1200} ))

echo "Waiting for CI on $SHA"

RUN_ID=""
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    RUN_ID=$(gh run list --repo "$REPO" --limit 20 \
        --json databaseId,headSha,workflowName \
        --jq "[.[] | select(.headSha == \"$SHA\" and .workflowName == \"CI\")][0].databaseId" 2>/dev/null)
    [ -n "$RUN_ID" ] && [ "$RUN_ID" != "null" ] && break
    sleep 5
done

if [ -z "$RUN_ID" ] || [ "$RUN_ID" = "null" ]; then
    echo "No CI run appeared for $SHA"
    exit 1
fi

echo "Run $RUN_ID"
gh run watch "$RUN_ID" --repo "$REPO" --exit-status >/dev/null 2>&1
STATUS=$?

gh run view "$RUN_ID" --repo "$REPO" --json jobs \
    --jq '.jobs[] | "\(.name): \(.conclusion)"'

if [ $STATUS -ne 0 ]; then
    echo "--- errors ---"
    gh run view "$RUN_ID" --repo "$REPO" --log-failed 2>/dev/null \
        | grep -iE "error" | sed 's/.*Z //' | sort -u | head -20
fi

echo "RUN_ID=$RUN_ID"
exit $STATUS
