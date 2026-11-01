# Andarium

Andarium is the iOS application that runs one Android (LineageOS 24, Android
17) guest on the Darwin runtime. Android ships inside the app; what the user
creates, sizes and erases is the guest. It is an iOS device product like
[Kernarium](../Kernarium/README.md) and runs on an Apple silicon Mac in
Designed for iPad mode for the same reason
([`graphics.md`](../docs/graphics.md#current-path)). Kernarium stays the
developer application for Linux guests and for Android bundles.

## Ownership

```text
Andarium/             SwiftUI application: Home, boot, session, end screens
AndariumCore/         Swift package: the application's model, no UIKit
Andarium.xcodeproj/   JSON project (project.xcproj) and its shared schemes
```

The project file is the JSON format Xcode 27 reads and Xcode 27.2 writes
(`project.xcproj`), kept in the canonical form
`xcrun xcprojformatter --update Andarium/Andarium.xcodeproj` produces: format
after every hand edit. Kernarium keeps its `.pbxproj`.

## Preview

`AndariumPreview` is a Simulator-only target built from the application's
sources with mock data. It embeds no runtime and boots nothing; the iOS
Simulator has no Metal 4 device for the guest's graphics. Its launch arguments
(`-screen <name>`, `-play YES`, `-lowSpace YES`, `-resetTips YES`) open any
screen or state directly so a Simulator driven only by `simctl` reaches every
flow:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme AndariumPreview \
    -destination 'generic/platform=iOS Simulator' build
```

## Build

Packaging the Android runtime alone, and staging the system from the accepted
release, need no Linux runtime build:

```sh
cmake --preset kernarium-ios-device-android
cmake --build --preset kernarium-ios-device-android
Kernarium/scripts/package-ios-frameworks.sh --runtimes android
Andarium/scripts/stage-system-image.py
```

The frameworks go to `.build/kernarium-xcframeworks`, which Kernarium shares,
and the system to `.build/andarium-system`.

## Model

`AndariumCore` holds everything the screens decide with, so it is tested on
the host (`andarium-core`, [`testing.md`](../docs/testing.md#andarium)): the
limits and display formats, the memory policy, the guest and its life
(`Lifecycle`: create, start, boot, erase, how a run ends), the system's
description read from the release manifest, boot milestones found in the
console, and the guest's storage. The application adds only what a screen
holds: the open sheet, display options and the session's state.

A screen shows a control only when the capability behind it exists
(`Capabilities`). The Simulator preview has them all; a build whose runtime
cannot do a thing leaves its control out instead of offering it.

### Memory

Guest memory runs from 1 GB in steps of 256 MB. The top of the ruler is one
step below the recommended maximum, which is the largest guest memory that
leaves the guest's graphics budget at its floor on that device; so no
position on the ruler is one iOS is likely to end the application for. The
default is 1.5 GB, or the ruler's top where that is lower. The budget terms
(`MemoryTerms`) are measured provisional values; replacing them changes only
that value.

### Storage

A guest's files live in `Library/Application Support/Guest/` of the
application's container, behind `GuestStorage`. The first release keeps the
whole system and the guest's data in one GPT image (`SingleImageStorage`),
written sparse from the system image the application carries: Create and
Erase take tens of seconds and each guest holds its own copy of the system,
and a new system reaches an existing guest only by erasing it. That layout is
interim; a layout with the system apart from the data replaces it behind the
same protocol.

The guest is left out of iCloud and computer backups unless the user turns on
Include in Backups for it: Android can set itself up again, and the files take
several gigabytes.
