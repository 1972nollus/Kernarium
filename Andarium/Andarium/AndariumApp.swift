import SwiftUI

@main
struct AndariumApp: App {
    @State private var model: AppModel
    private let launch: LaunchOptions

    init() {
        let launch = LaunchOptions.fromArguments()
        launch.configureTips()
        self.launch = launch
        _model = State(initialValue: launch.makeModel())
    }

    var body: some Scene {
        WindowGroup {
            RootView(launch: launch)
                .environment(model)
        }
    }
}

/// A launch moves forward only: home, one boot, its run, and how it ended.
struct RootView: View {
    @Environment(AppModel.self) private var model
    let launch: LaunchOptions

    var body: some View {
        Group {
            switch model.phase {
            case .home:
                HomeView(launch: launch)
            case .booting(let kind):
                BootView(kind: kind)
                    .id(kind)
            case .running(let mode):
                SessionView(mode: mode)
            case .poweredOff:
                EndView(failure: false)
            case .failed:
                EndView(failure: true)
            }
        }
        // Toggles take the jade accent too, not the system green.
        .tint(Color.accentColor)
        .transition(.opacity)
        .animation(.easeInOut(duration: 0.4), value: model.phase)
        .task {
            await launch.applyDeferred(to: model)
        }
    }
}
