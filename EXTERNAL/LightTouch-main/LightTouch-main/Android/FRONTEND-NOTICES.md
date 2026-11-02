# Selected frontend runtime notice assets

`scripts/package-frontend-notices.py` consumes the private schema-1 report from
`:app:recordApkRuntimeArtifacts`. Use the configuration and source/build properties
for the actual main APK. The verification metadata file, download cache and
androidTest/build-tool dependencies are not substitutes for that selected graph.

```sh
python3 Android/scripts/package-frontend-notices.py \
  /private/build/app/reports/debugRuntimeClasspath-artifacts.json \
  /private/build/generated/assets/licenses/frontend \
  --metadata-cache /private/gradle/caches/modules-2/files-2.1 \
  --source-reference https://publisher.example/corresponding-source \
  --source-revision FULL_40_HEX_SOURCE_REVISION \
  --supplements /private/checked-upstream-notice-supplements.json
```

The generator is offline. It verifies each selected original artifact SHA256,
reads outer archive notices and nested JAR notices (including classes.jar), and
copies their bytes unchanged. It retains full Maven coordinates, versions,
artifact filenames/hashes, original member paths and notice hashes. It records
original selected-version POM bytes and metadata provenance; Gradle module
name/hash mappings preserve canonical upstream URLs for platform artifact aliases
such as `ui-release.aar`. Kotlin multiplatform parent redirects remain explicit.
Duplicate identical selected rows retain their selection count without duplicating
the notice material. No local cache/artifact paths are copied into public assets.

The generated `manifest.json` has schema 1, `files[{path,sha256}]` and an HTTPS
`source_reference`, compatible with the existing offline LicenseDialog. All output
path components use its safe ASCII path alphabet. Original ZIP paths are metadata
only and are never extracted to the filesystem. The manifest additionally records
the selected configuration, portable graph hash, components, checked source
supplements and missing original notice material. Byte-identical reruns verify
the existing bundle; differing assets need a fresh generated-assets directory.

No license is assigned from a group-name prefix or guessed when an archive lacks
notices. Missing primary notice bytes fail before output by default. For an audit
only, `--allow-missing` emits the retained material and an explicitly INCOMPLETE
coverage field/list; this is not a finished release notice bundle. A POM declaration
is preserved as original publisher metadata, not silently substituted for a full
license/NOTICE or a grant for additional native/third-party material.

Checked supplemental original texts use this separate private schema:

```json
{
  "schema": 1,
  "components": [{
    "coordinate": "publisher.group:module:exact-version",
    "artifact_sha256": "FULL_ARTIFACT_SHA256",
    "notices": [{
      "name": "LICENSE.txt",
      "role": "license",
      "notice_path": "/private/checked-public-source/LICENSE.txt",
      "sha256": "FULL_ORIGINAL_TEXT_SHA256",
      "source_url": "https://upstream.example/exact-version/LICENSE.txt",
      "upstream_reference": "exact-version-or-source-commit"
    }]
  }]
}
```

Roles are `license`, `notice` or `attribution`. Supplements must match a selected
coordinate AND artifact hash, have safe names, bounded original bytes and matching
text hashes, and identify HTTPS primary provenance without embedded credentials.
Unknown/stale components, changed original bytes or ambiguous metadata fail.
The local paths in this input stay private; only checked texts and public provenance
are emitted. A license text must actually apply to that exact source/component;
the script does not authenticate or invent the publisher's grant.

The measured debug inventory at the first checkpoint has 58 selected rows and
57 unique artifacts. Five original artifacts embed LICENSE files; none of this
graph's nested classes.jar files contains additional notice entries. The nested
route is exercised by source tests rather than claimed from an absent real entry.
Most selected archives contain no notice entries, so their exact declared/source
license material needs supplemental retention. The graphics-path AAR ships a
native DSO; its Java POM alone is not a native-code attribution audit. Kotlin's
versioned license summary identifies additional stdlib source attributions, which
must be checked against the selected JVM runtime rather than copying all compiler,
test, JS or native licenses into the APK's runtime inventory.

For this checked selected graph, the production recipe can consume an offline
cache of the 20 primary source texts pinned by URL and SHA256 in the script's
`PINNED_UPSTREAM_TEXTS`, instead of a manually assembled supplements file:

```sh
python3 Android/scripts/package-frontend-notices.py /private/selected-artifacts.json \
  /private/generated/assets/licenses/frontend \
  --metadata-cache /private/gradle/caches/modules-2/files-2.1 \
  --upstream-cache /private/pinned-frontend-source-notices \
  --source-reference https://publisher.example/corresponding-source \
  --source-revision FULL_40_HEX_SOURCE_REVISION
```

Provision that legal-source cache separately; notice generation performs no
downloads. This reproducible procurement snippet fetches only the enumerated
public, versioned legal/source inputs and verifies their bytes. Gitiles source
files use its Base64 TEXT representation and are hashed after decoding:

```python
import base64, hashlib, pathlib, runpy, urllib.request
pins = runpy.run_path("Android/scripts/package-frontend-notices.py")["PINNED_UPSTREAM_TEXTS"]
cache = pathlib.Path("/private/pinned-frontend-source-notices")
cache.mkdir(parents=True, exist_ok=True)
for name, (url, digest) in sorted(pins.items()):
    data = urllib.request.urlopen(url, timeout=30).read()
    if url.endswith("?format=TEXT"):
        data = base64.b64decode(data)
    assert hashlib.sha256(data).hexdigest() == digest, name
    (cache / name).write_bytes(data)
```

The shared Apache text is used only when the exact original selected-version POM
(or its recorded original parent) explicitly references that license URL. Every
such supplement records and verifies the original POM hash and declared URL;
it is not a group-prefix license assignment. Kotlin v2.1.20's original source
license summary and the GWT, ThreeTen BSD3, Guava and Boost texts are retained for
its JVM stdlib. The original summary includes broader compiler/test/platform
sections; retaining that document does not label those components as shipped.
The JDK compatibility artifacts use their actual v1.8.20 source license;
coroutines use 1.7.3, and annotations use 23.0.0. Changed runtime versions need a
new reviewed source-notice recipe instead of silently inheriting these pins.

The graphics-path 1.0.1 native attribution inputs are pinned to the endpoint
`8a05a22af450d589ef911d772a001a49dcb05b71` linked by its
[official release history](https://developer.android.com/jetpack/androidx/releases/graphics).
Its ten original C++/math copyright-license headers are copied as unchanged
source byte ranges; full source-file hashes and byte offsets remain in the manifest.
The producer verifies each range against the checked original source, rather than
constructing attribution text. This does not claim a reproduced native build.
The ui 1.7.8 AAR additionally contains inspector.jar with nested tooling native
files; these remain original archive provenance, not automatically APK runtime
components. The inspected frozen main APKs contain neither that tooling JAR nor
its JNI libraries. Recheck the actual conveyed APK if packaging changes.

The checked production bundle has 137 files: one manifest and 136 original
notice/metadata payloads representing all 57 unique selected inputs. This count
includes the five original embedded licenses, 57 original POMs, the Guava parent,
52 POM-bound full license texts and 21 versioned source attribution/license texts.
All notice hashes, path safety, private-path exclusion and deterministic output
were checked. `ALL_SELECTED_INPUTS_REPRESENTED` describes retained checked bytes;
it is not a distribution grant or a claim that every source/licensing question
inside those upstream projects has been resolved.

Run source-only correctness checks without Gradle, a compiler or device:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s Android/scripts/tests -p test_frontend_notices.py -v
```

These assets retain checked notice bytes for selected inputs. They do not establish
DEX shrinking coverage, complete corresponding source/relink delivery, source URL
availability, combined-work classification, distribution clearance or any runtime
acceptance gate. Generated inputs, receipts and bundles remain outside public Git.
