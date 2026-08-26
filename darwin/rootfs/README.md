# Legacy rootfs fragments

This directory contains payload fragments retained for old minimal images. No
current build or test target consumes them.

The supported guest image is the repository-local `rootfs.erofs`. Provisioning
and authentication are owned by `tools/rootfs`; ordinary guest runs use the
image directly and put writable state in a separate COW or OverlayFS layer.

The image owns accounts, credentials, login policy, gettys, and desktop
packages. Launchers must preserve that ownership and limit compatibility
overlays to the specific session that needs them. See
[`../../docs/testing.md`](../../docs/testing.md#rootfs-contract).
