// Mirrors the package.json version (managed by changesets) into the
// idf_component.yml manifest the ESP Component Registry publishes from.
import { readFileSync, writeFileSync } from "node:fs";

const { version } = JSON.parse(readFileSync("package.json", "utf8"));
const manifestPath = "idf_component.yml";
const manifest = readFileSync(manifestPath, "utf8");
const updated = manifest.replace(/^version: ".*"$/m, `version: "${version}"`);
if (updated === manifest && !manifest.includes(`version: "${version}"`)) {
  console.error(`could not find a version line to update in ${manifestPath}`);
  process.exit(1);
}
writeFileSync(manifestPath, updated);
console.log(`${manifestPath} version -> ${version}`);
