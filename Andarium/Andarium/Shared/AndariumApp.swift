import AndariumCore
import SwiftUI
import UIKit

@main
final class AndariumApp: UIResponder, UIApplicationDelegate {
    private var sceneConnection: NSObjectProtocol?

    func application(_ application: UIApplication, didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // Existing installations can restore the scene configuration saved
        // by the former SwiftUI App entry point. Adopt those scenes too;
        // their old delegate no longer has an App graph to create a window.
        sceneConnection = NotificationCenter.default.addObserver(
            forName: UIScene.willConnectNotification, object: nil, queue: .main
        ) { notification in
            guard let scene = notification.object as? UIWindowScene else { return }
            Task { @MainActor in Self.adopt(scene) }
        }
        Task { @MainActor in
            for case let scene as UIWindowScene in application.connectedScenes { Self.adopt(scene) }
        }
        return true
    }

    private static func adopt(_ scene: UIWindowScene) {
        guard !(scene.delegate is AndariumSceneDelegate) else { return }
        let delegate = AndariumSceneDelegate()
        scene.delegate = delegate
        delegate.connect(scene)
    }

    func application(
        _ application: UIApplication, configurationForConnecting session: UISceneSession,
        options: UIScene.ConnectionOptions
    ) -> UISceneConfiguration {
        let configuration = UISceneConfiguration(name: "Andarium", sessionRole: session.role)
        configuration.delegateClass = AndariumSceneDelegate.self
        return configuration
    }
}

final class AndariumSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    /// One model, runtime and motion manager for the scene's whole life.
    private(set) var model: AppModel?

    func windowScene(_ windowScene: UIWindowScene, didUpdateEffectiveGeometry previousGeometry: UIWindowScene.Geometry) {
        (window?.rootViewController as? SceneGeometryObserver)?.sceneGeometryDidChange()
    }

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let scene = scene as? UIWindowScene else { return }
        connect(scene)
    }

    func connect(_ scene: UIWindowScene) {
        guard window == nil else { return }
        let entry = AppEntry.fromArguments()
        entry.configureTips()
        let model = entry.makeModel()
        self.model = model
        let window = UIWindow(windowScene: scene)
        window.rootViewController = AndariumHostingController(
            rootView: RootView(entry: entry).environment(model), model: model)
        self.window = window
        window.makeKeyAndVisible()
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
