#!/usr/bin/env bash
# Publishes one build log as the sole file on an orphan branch, force-pushed so
# the branch always holds just the latest result. The commit message records the
# job status and the source commit, so a reader can tell which push it belongs to.
#
# Usage: publish-log.sh <log file>
# Env:   GH_TOKEN, LOG_BRANCH, JOB_STATUS, plus the standard GITHUB_* variables.
set -u

log="${1:?log file}"
if [ ! -f "$log" ]; then
    echo "(no build log: the job failed before the build step ran)" > "$log"
fi

tmp="$(mktemp -d)"
cp "$log" "$tmp/build.log"
cd "$tmp"

git init -q
git checkout -q -b publish
git add build.log
git -c user.name="ci" -c user.email="ci@users.noreply.github.com" \
    commit -q -m "${GITHUB_JOB} ${JOB_STATUS} ${GITHUB_SHA}"

# GITHUB_TOKEN pushes never trigger workflows, so this cannot loop.
git push -q -f "https://x-access-token:${GH_TOKEN}@github.com/${GITHUB_REPOSITORY}.git" \
    "publish:${LOG_BRANCH}"
