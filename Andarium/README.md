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
