import AndariumCore
import SwiftUI

@main
struct AndariumApp: App {
    @State private var model: AppModel
    private let entry: AppEntry

    init() {
        let entry = AppEntry.fromArguments()
        entry.configureTips()
        self.entry = entry
        _model = State(initialValue: entry.makeModel())
    }

    var body: some Scene {
        WindowGroup {
            RootView(entry: entry)
                .environment(model)
        }
    }
}

/// A launch moves forward only: home, one boot, its run, and how it ended.
struct RootView: View {
    @Environment(AppModel.self) private var model
    let entry: AppEntry

    var body: some View {
        @Bindable var model = model
        ZStack {
            // The guest's display lives under the screens of the boot and the
            // run, so it is one view from the first frame to the last.
            GuestLayer()
            Group {
                switch model.phase {
                case .home:
                    HomeView(hints: entry.hints)
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
            .transition(.opacity)
            if let busy = model.busy {
                BusyOverlay(state: busy)
            }
        }
        // Toggles take the jade accent too, not the system green.
        .tint(Color.accentColor)
        .animation(.easeInOut(duration: 0.4), value: model.phase)
        .animation(.easeInOut(duration: 0.2), value: model.busy != nil)
        .alert(
            model.alert?.title ?? "",
            isPresented: Binding(get: { model.alert != nil }, set: { if !$0 { model.alert = nil } }),
            presenting: model.alert
        ) { _ in
            Button("OK", role: .cancel) {}
        } message: { alert in
            Text(alert.message)
        }
        .task {
            await entry.applyDeferred(to: model)
        }
    }
}
