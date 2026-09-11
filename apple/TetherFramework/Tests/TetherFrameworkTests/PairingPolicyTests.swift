//
//  PairingPolicyTests.swift
//  TetherFrameworkTests
//

import Testing
@testable import TetherFramework

struct PairingPolicyTests {
    // A peer that dialled us is the one asking; the local user approves it. Anything
    // beyond its request — a self-issued pair_accepted above all — must be dropped.
    @Test func inboundPeerMaySendOnlyItsRequest() {
        #expect(TetherCommand.allowedWhileUnpaired(.pairRequest, inbound: true))

        #expect(!TetherCommand.allowedWhileUnpaired(.pairAccepted, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(.pairPending, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(.error, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(.clipboardUpdated, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(.clipboardContent, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(.fileStart, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(.fileChunk, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(.fileEnd, inbound: true))
        #expect(!TetherCommand.allowedWhileUnpaired(nil, inbound: true))
    }

    // We dialled, so the peer's user holds the verdict and we may hear it.
    @Test func outboundPeerMaySendOnlyItsVerdict() {
        #expect(TetherCommand.allowedWhileUnpaired(.pairAccepted, inbound: false))
        #expect(TetherCommand.allowedWhileUnpaired(.pairPending, inbound: false))
        #expect(TetherCommand.allowedWhileUnpaired(.error, inbound: false))

        #expect(!TetherCommand.allowedWhileUnpaired(.pairRequest, inbound: false))
        #expect(!TetherCommand.allowedWhileUnpaired(.clipboardUpdated, inbound: false))
        #expect(!TetherCommand.allowedWhileUnpaired(.clipboardContent, inbound: false))
        #expect(!TetherCommand.allowedWhileUnpaired(.fileStart, inbound: false))
        #expect(!TetherCommand.allowedWhileUnpaired(nil, inbound: false))
    }
}
