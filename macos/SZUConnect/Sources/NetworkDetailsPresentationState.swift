import SwiftUI

/// Owned by the dashboard, so switching pages does not reset a user's disclosure choice.
/// Presentation choices stay in memory and do not depend on connectivity or saved preferences.
@MainActor
final class NetworkDetailsPresentationState: ObservableObject {
    @Published var expanded = false

    func reveal() {
        expanded = true
    }
}
