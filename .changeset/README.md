# Releases via changesets

This C library versions and publishes through [changesets](https://github.com/changesets/changesets), even though it ships to the ESP Component Registry rather than npm. The root `package.json` exists only as the version anchor; `scripts/sync-component-version.mjs` mirrors its version into `idf_component.yml`.

The flow:

1. Land changes on `main` together with a changeset file. For same-repo PRs with a conventional-commit title touching published paths, the `Generate changeset from PR` workflow writes one automatically (`.changeset/pr-<n>.md`, marked `<!-- auto-generated -->`); a manual changeset (`npx changeset`, or markdown by hand) always takes precedence, and editing the auto-generated file (removing the marker) stops the bot from touching it. Fork PRs need a manual changeset.
2. The `Release` workflow keeps a "chore: release" PR up to date: it runs `changeset version`, syncs `idf_component.yml`, and updates `CHANGELOG.md`.
3. Merging that PR is the explicit publish action: the workflow uploads the component to the ESP Component Registry (`compote`, using the `IDF_COMPONENT_API_TOKEN` secret), pushes a `v<version>` tag, and creates the GitHub Release with the changelog entry as its body.

`scripts/release.sh` is idempotent (it exits early when the current version's tag already exists), because the changesets action runs the publish command on every push to `main` that has no pending changesets. The `Publish to ESP Component Registry` workflow remains as a manual (`workflow_dispatch`) fallback for re-publishing; tag pushes no longer trigger it, since tags created with the workflow token would not fire it anyway.
