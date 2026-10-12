# Android guest sources

Android guest inputs are organized by AOSP major version. The first maintained
lane is [`lineage-24/`](lineage-24/README.md).

This directory owns AOSP patch series, builder-local scripts, image/release
support, and Android-version metadata. The ACK kernel configuration overlay and
generated kernel patches remain under
[`../kernel/android-6.18/`](../kernel/android-6.18/README.md).

Tracked scripts run locally on the dedicated Linux builder. They accept only
builder-local filesystem paths. Workstation-to-builder transport, endpoints,
accounts, and credentials are deliberately outside this repository.
