import SwiftUI

/// Disclosure choices are local to this dashboard session, never persisted or coupled to network state.
final class HelpDisclosureState: ObservableObject {
    @Published var firstUseExpanded = false
    @Published var expandedFAQs: Set<String> = []

    func faq(_ key: String) -> Binding<Bool> {
        Binding(get: { self.expandedFAQs.contains(key) }, set: { expanded in
            if expanded { self.expandedFAQs.insert(key) } else { self.expandedFAQs.remove(key) }
        })
    }
}

