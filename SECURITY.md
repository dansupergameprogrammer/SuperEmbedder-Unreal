# Security Policy

## Supported versions

Only the latest tagged release is supported. Security fixes are not backported to older releases.

## Reporting a vulnerability

Report a suspected vulnerability privately, through GitHub's Security Advisory feature for this repository
(**Security > Advisories > Report a vulnerability**). Do not open a public issue for it.

Include:

- the affected version: the plugin's `VersionName` from `Plugins/SuperEmbedderUnreal/SuperEmbedderUnreal.uplugin`,
  and the SuperEmbedder and SuperSLM tags and commits from the `VENDORED_VERSION.txt` files under
  `Plugins/SuperEmbedderUnreal/Source/ThirdParty/`;
- steps to reproduce, and the impact you assess.

A report that concerns SuperEmbedder's or SuperSLM's own code (the vendored library or engine, or the `.sslm`
format) may be reported here or to that repository; the fix lands upstream and reaches this plugin through a
re-pin.

Model files are loaded from paths you choose, and the library reads them itself. Set the files' SHA-256 in Project
Settings to have them checked before every open. Vector sets and cache files are checked against their own digests
when they load; a damaged one is refused, never used.

You will get an acknowledgement within a reasonable time, and a fix or a mitigation plan once the report is
triaged.
