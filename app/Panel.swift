// Tether.app -- the Liquid Glass panel that opens from the menu bar icon.
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

struct PanelView: View {
    @ObservedObject var model: TetherModel

    var body: some View {
        GlassEffectContainer(spacing: 12) {
            VStack(spacing: 12) {
                header
                StatusCard(model: model)
                RouteToggle(model: model)
                primaryButton
                footer
            }
        }
        .padding(16)
        .frame(width: 320)
        .animation(.smooth(duration: 0.35), value: model.state)
        .animation(.smooth(duration: 0.35), value: model.address)
        .animation(.smooth(duration: 0.35), value: model.isPrimary)
    }

    // MARK: Header

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: "personalhotspot")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 36, height: 36)
                .glassEffect(.regular.tint(.blue), in: .circle)
            VStack(alignment: .leading, spacing: 1) {
                Text("Tether").font(.headline)
                Text("USB tethering").font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            StatePill(state: model.state, loading: model.isLoading)
        }
    }

    // MARK: Connect / Disconnect

    private var primaryButton: some View {
        Button {
            model.running ? model.disconnect() : model.connect()
        } label: {
            Label(model.running ? "Disconnect" : "Connect",
                  systemImage: model.running ? "stop.fill" : "bolt.horizontal.fill")
                .font(.body.weight(.semibold))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 4)
        }
        .buttonStyle(.glassProminent)
        .tint(model.running ? .red : .blue)
        .controlSize(.large)
        .keyboardShortcut(.defaultAction)
    }

    // MARK: Footer

    private var footer: some View {
        HStack(spacing: 8) {
            FooterButton(symbol: model.verbose ? "text.badge.checkmark" : "text.alignleft",
                         help: model.verbose ? "Verbose logging is on" : "Turn on verbose logging",
                         highlighted: model.verbose, action: model.toggleVerbose)
            FooterButton(symbol: "doc.text.magnifyingglass", help: "Open log", action: model.openLog)
            Spacer()
            FooterButton(symbol: "trash", help: "Remove the helper", action: model.removeHelper)
            FooterButton(symbol: "power", help: "Quit Tether (disconnects)", action: model.quit)
        }
    }
}

// MARK: - Status card

private struct StatusCard: View {
    @ObservedObject var model: TetherModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .font(.system(size: 26, weight: .medium))
                    .foregroundStyle(color)
                    .frame(width: 34)
                    .contentTransition(.symbolEffect(.replace))
                    .symbolEffect(.variableColor.iterative.reversing, isActive: model.isLoading)
                    .symbolEffect(.pulse, isActive: model.state == .waiting)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.title3.weight(.semibold))
                    Text(subtitle)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }

            if case .connected(_, let iface) = model.state {
                Divider().opacity(0.5)
                DetailRow(label: "Interface", value: iface)
                HStack {
                    Text("Address").foregroundStyle(.secondary)
                    Spacer()
                    if let address = model.address {
                        Text(address)
                            .monospacedDigit()
                            .textSelection(.enabled)
                            .transition(.blurReplace)
                    } else {
                        HStack(spacing: 6) {
                            ProgressView().controlSize(.mini)
                            Text("Getting address").foregroundStyle(.secondary)
                        }
                        .transition(.blurReplace)
                    }
                }
                .font(.callout)
                if model.address != nil {
                    HStack {
                        Text("Route").foregroundStyle(.secondary)
                        Spacer()
                        Label(model.isPrimary ? "Main connection" : "Alongside Wi-Fi",
                              systemImage: model.isPrimary ? "arrow.up.forward.circle.fill" : "arrow.triangle.branch")
                            .foregroundStyle(model.isPrimary ? .green : .secondary)
                            .contentTransition(.symbolEffect(.replace))
                    }
                    .font(.callout)
                    .transition(.blurReplace)
                }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassEffect(.regular.tint(color.opacity(0.12)), in: .rect(cornerRadius: 18))
    }

    private var symbol: String {
        switch model.state {
        case .stopped: return "cable.connector.slash"
        case .starting, .waiting: return "cable.connector"
        case .connecting: return "antenna.radiowaves.left.and.right"
        case .connected: return model.address == nil ? "antenna.radiowaves.left.and.right" : "checkmark.circle.fill"
        case .failed: return "exclamationmark.triangle.fill"
        }
    }

    private var color: Color { model.state.color(loading: model.isLoading) }

    private var title: String {
        switch model.state {
        case .stopped: return "Disconnected"
        case .starting: return "Starting"
        case .waiting: return "Waiting for phone"
        case .connecting: return "Connecting"
        case .connected: return model.address == nil ? "Connecting" : "Connected"
        case .failed: return "Something went wrong"
        }
    }

    private var subtitle: String {
        switch model.state {
        case .stopped: return "Press Connect to start tethering."
        case .starting: return "Starting the helper…"
        case .waiting: return "Plug in the phone and turn on USB tethering."
        case .connecting(let d), .connected(let d, _): return d
        case .failed(let e): return e
        }
    }
}

private struct DetailRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack {
            Text(label).foregroundStyle(.secondary)
            Spacer()
            Text(value).monospaced()
        }
        .font(.callout)
    }
}

// MARK: - Main connection setting

private struct RouteToggle: View {
    @ObservedObject var model: TetherModel

    var body: some View {
        Toggle(isOn: Binding(get: { model.preferPhone }, set: { _ in model.togglePreferPhone() })) {
            VStack(alignment: .leading, spacing: 1) {
                Text("Use phone as main connection").font(.callout.weight(.medium))
                Text("Send all traffic through the phone, even with Wi-Fi on.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .toggleStyle(.switch)
        .controlSize(.small)
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .glassEffect(.regular, in: .rect(cornerRadius: 14))
    }
}

// MARK: - Small pieces

private struct StatePill: View {
    let state: TetherState
    let loading: Bool

    var body: some View {
        HStack(spacing: 5) {
            Circle()
                .fill(state.color(loading: loading))
                .frame(width: 7, height: 7)
                .shadow(color: state.color(loading: loading).opacity(0.8), radius: 3)
                .phaseAnimator([false, true], trigger: loading) { dot, phase in
                    dot.opacity(loading && phase ? 0.35 : 1)
                }
            Text(label).font(.caption.weight(.medium))
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .glassEffect(.regular, in: .capsule)
    }

    private var label: String {
        switch state {
        case .stopped: return "Off"
        case .starting, .waiting: return "Waiting"
        case .connecting: return "Linking"
        case .connected: return loading ? "Linking" : "Online"
        case .failed: return "Error"
        }
    }
}

private struct FooterButton: View {
    let symbol: String
    let help: String
    var highlighted = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 13, weight: .medium))
                .frame(width: 30, height: 30)
                .contentTransition(.symbolEffect(.replace))
        }
        .buttonStyle(.glass)
        .buttonBorderShape(.circle)
        .tint(highlighted ? .blue : nil)
        .help(help)
    }
}

extension TetherState {
    func color(loading: Bool) -> Color {
        switch self {
        case .stopped: return .gray
        case .starting, .waiting: return .blue
        case .connecting: return .orange
        case .connected: return loading ? .orange : .green
        case .failed: return .red
        }
    }
}
