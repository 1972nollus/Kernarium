# LibVNCServer release backports

The base source is the official `LibVNCServer-0.9.15` release package. Upstream
does not currently issue maintenance point releases and its `SECURITY.md` asks
downstream packagers to apply fixes from the main branch to releases.

[`0001-release-backports.patch`](0001-release-backports.patch) concatenates
the following official commit patches in application order. Every original
40-character commit identity and author/date header is retained in the file:

| Commit | Purpose |
| --- | --- |
| `ae7656ef8ab2f61f70a58fafd88e527f35b4e8de` | release temporary clipboard-compression storage |
| `e7ee07ddd7a3015a5b059a884f18181f3bd27877` | reject an invalid common socket descriptor |
| `b17c59257fd940af3fb076ae9c4cf88c40e133ac` | reject invalid client socket descriptors |
| `eae4f703b7a29573963fa91f556e1257932cc860` | reject invalid server socket descriptors |
| `23d7ef6c34c118cb938ce1d0aaa093a9a5ca842d` | close a client socket when setup fails |
| `ad5592717435310fee6334821d0681f555373b50` | report already-buffered client input correctly |
| `dc78dee51a7e270e537a541a17befdf2073f5314` | validate optional HTTP proxy request fields |
| `009008e2f4d5a54dd71f422070df3af7b3dbc931` | validate client rectangle counts before decoding |
| `5b270544b85233668b98161323297d418a8f5fd1` | bound client gradient decoding scratch storage |
| `5ea5fc30e9093154d2d22a1f6316867f0fedaf7f` | avoid repeated close operations during server updates |

Each patch is available from the official repository at
`https://github.com/LibVNC/libvncserver/commit/<commit>.patch`. The aggregate
patch hash and the fully patched source-tree hash are both locked in the main
dependency manifest. Broad unreleased feature and build-system changes are not
included.
