#!/usr/bin/env node
// Updates the single source of truth for the project version - the root
// CMakeLists.txt's project(shaderfmt VERSION x.y.z ...) line - to the
// version semantic-release just computed. Invoked by @semantic-release/exec
// (see .releaserc.json's "prepareCmd"), never run by hand.
//
// A small Node script rather than sed: this repo's CI matrix includes
// Windows, where a portable `sed -i` isn't guaranteed available, but
// Node.js already is (semantic-release itself needs it).

const fs = require("fs");
const path = require("path");

const version = process.argv[2];
if (!version || !/^\d+\.\d+\.\d+$/.test(version)) {
    console.error(`bump-cmake-version.js: expected a plain X.Y.Z version argument, got: ${version}`);
    process.exit(1);
}

const cmakeListsPath = path.join(__dirname, "..", "CMakeLists.txt");
const original = fs.readFileSync(cmakeListsPath, "utf8");

const pattern = /project\(shaderfmt VERSION \d+\.\d+\.\d+ LANGUAGES CXX\)/;
if (!pattern.test(original)) {
    console.error("bump-cmake-version.js: could not find the expected "
        + "'project(shaderfmt VERSION X.Y.Z LANGUAGES CXX)' line in CMakeLists.txt "
        + "- has its wording changed?");
    process.exit(1);
}

const updated = original.replace(pattern, `project(shaderfmt VERSION ${version} LANGUAGES CXX)`);
fs.writeFileSync(cmakeListsPath, updated);
console.log(`bump-cmake-version.js: CMakeLists.txt project version set to ${version}`);
