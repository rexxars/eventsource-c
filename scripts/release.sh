#!/usr/bin/env bash
# Publishes the current version to the ESP Component Registry and pushes the
# v<version> tag. Invoked by the changesets action after the release PR
# merges. Must be idempotent: the action runs this on every push to main
# that has no pending changesets.
set -euo pipefail

VERSION=$(node -p 'JSON.parse(require("node:fs").readFileSync("package.json","utf8")).version')
MANIFEST_VERSION=$(sed -n 's/^version: "\(.*\)"$/\1/p' idf_component.yml)

if [ "$VERSION" = "0.0.0" ]; then
  echo "version 0.0.0 is the pre-release placeholder; nothing to publish"
  exit 0
fi
if [ "$VERSION" != "$MANIFEST_VERSION" ]; then
  echo "package.json ($VERSION) and idf_component.yml ($MANIFEST_VERSION) disagree" >&2
  exit 1
fi
if git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null; then
  echo "v$VERSION already tagged; nothing to publish"
  exit 0
fi

compote component upload --namespace rexxars --name eventsource --project-dir .

# `changeset tag` prints the "New tag:" line the changesets action parses to
# push the tag and create the GitHub Release from the CHANGELOG entry.
npx changeset tag
echo "published v$VERSION"
