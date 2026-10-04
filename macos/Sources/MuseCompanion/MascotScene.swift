// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
// A procedural 3D interpretation of esp32/avatar/jollybot.gif. The character
// design is subject to the repository's separate artwork terms (see README).
import SceneKit
import SwiftUI

enum MascotMotion: String, CaseIterable, Identifiable {
    case hover = "Hover", wiggle = "Wiggle", hop = "Hop", dance = "Happy dance", peek = "Peek"
    var id: String { rawValue }
    var action: SCNAction {
        let action: SCNAction
        switch self {
        case .hover: action = .sequence([.moveBy(x: 0, y: 0.08, z: 0, duration: 1.5), .moveBy(x: 0, y: -0.08, z: 0, duration: 1.5)])
        case .wiggle: action = .sequence([.rotateBy(x: 0, y: 0, z: 0.13, duration: 0.18), .rotateBy(x: 0, y: 0, z: -0.26, duration: 0.36), .rotateBy(x: 0, y: 0, z: 0.13, duration: 0.18), .wait(duration: 1)])
        case .hop: action = .sequence([.scale(to: 0.93, duration: 0.15), .group([.scale(to: 1, duration: 0.2), .moveBy(x: 0, y: 0.30, z: 0, duration: 0.2)]), .moveBy(x: 0, y: -0.30, z: 0, duration: 0.25), .wait(duration: 1.1)])
        case .dance: action = .sequence([.group([.moveBy(x: 0.12, y: 0.06, z: 0, duration: 0.3), .rotateBy(x: 0, y: 0, z: -0.1, duration: 0.3)]), .group([.moveBy(x: -0.24, y: -0.06, z: 0, duration: 0.6), .rotateBy(x: 0, y: 0, z: 0.2, duration: 0.6)]), .group([.moveBy(x: 0.12, y: 0, z: 0, duration: 0.3), .rotateBy(x: 0, y: 0, z: -0.1, duration: 0.3)])])
        case .peek: action = .sequence([.rotateBy(x: 0, y: 0, z: 0.20, duration: 0.35), .wait(duration: 0.6), .rotateBy(x: 0, y: 0, z: -0.20, duration: 0.35), .wait(duration: 1.3)])
        }
        action.timingMode = .easeInEaseOut
        return .repeatForever(action)
    }
}

struct MascotView: NSViewRepresentable {
    var spinning: Bool
    var thinking: Bool
    var speaking: Bool
    var motion: MascotMotion
    final class Coordinator { var motion: MascotMotion?; var speaking = false }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> SCNView {
        let view = SCNView()
        view.scene = makeScene()
        view.backgroundColor = .clear
        view.allowsCameraControl = true
        view.autoenablesDefaultLighting = false
        view.antialiasingMode = .multisampling4X
        view.preferredFramesPerSecond = 60
        view.isPlaying = true
        return view
    }
    func updateNSView(_ view: SCNView, context: Context) {
        guard let mascot = view.scene?.rootNode.childNode(withName: "mascot", recursively: true) else { return }
        if spinning && !speaking && mascot.action(forKey: "spin") == nil {
            mascot.runAction(.repeatForever(.rotateBy(x: 0, y: .pi * 2, z: 0, duration: 16)), forKey: "spin")
        } else if !spinning || speaking { mascot.removeAction(forKey: "spin") }
        mascot.action(forKey: "spin")?.speed = thinking ? 1.5 : 1
        if let movement = view.scene?.rootNode.childNode(withName: "motion", recursively: true), context.coordinator.motion != motion {
            movement.removeAction(forKey: "quirk")
            movement.position = SCNVector3Zero; movement.eulerAngles = SCNVector3Zero; movement.scale = SCNVector3(1, 1, 1)
            movement.runAction(motion.action, forKey: "quirk"); context.coordinator.motion = motion
        }
        if speaking != context.coordinator.speaking, let mouth = mascot.childNode(withName: "mouth", recursively: true) {
            context.coordinator.speaking = speaking
            mouth.isHidden = !speaking
            if speaking {
                mascot.runAction(.rotateTo(x: 0, y: -0.15, z: 0, duration: 0.3, usesShortestUnitArc: true))
                mouth.runAction(.repeatForever(.customAction(duration: 0.7) { node, elapsed in
                    node.scale.y = 0.35 + abs(sin(elapsed * 19)) * 1.4
                }), forKey: "talk")
            } else { mouth.removeAction(forKey: "talk") }
        }
    }
    private func makeScene() -> SCNScene {
        let scene = SCNScene()
        let camera = SCNNode()
        camera.camera = SCNCamera()
        camera.camera?.fieldOfView = 34
        camera.position = SCNVector3(0, 0.35, 7.9)
        scene.rootNode.addChildNode(camera)
        let rig = SCNNode(); rig.name = "mascot"; rig.eulerAngles.y = -0.22
        let movement = SCNNode(); movement.name = "motion"
        scene.rootNode.addChildNode(movement); movement.addChildNode(rig)
        rig.addChildNode(MascotMesh.make())
        let mouthShape = SCNBox(width: 0.19, height: 0.043, length: 0.016, chamferRadius: 0)
        let mouthMaterial = SCNMaterial(); mouthMaterial.lightingModel = .constant
        mouthMaterial.diffuse.contents = NSColor(calibratedRed: 0.23, green: 0.16, blue: 0.10, alpha: 1)
        mouthShape.materials = [mouthMaterial]
        let mouth = SCNNode(geometry: mouthShape); mouth.name = "mouth"
        mouth.position = SCNVector3(0, 0.065, 0.39); mouth.isHidden = true
        rig.addChildNode(mouth)
        // A constellation around the character also makes the depth visible.
        for i in 0..<15 {
            let angle = Double(i) * 2.39996
            let star = SCNSphere(radius: i % 3 == 0 ? 0.032 : 0.015)
            let mat = SCNMaterial()
            mat.diffuse.contents = NSColor(calibratedRed: 0.66, green: 0.49, blue: 1, alpha: 1)
            mat.emission.contents = NSColor(calibratedRed: 0.3, green: 0.18, blue: 0.6, alpha: 1)
            star.materials = [mat]
            let node = SCNNode(geometry: star)
            node.position = SCNVector3(cos(angle) * 1.65, sin(angle) * 1.6, sin(angle * 0.7) * 0.6 - 0.5)
            scene.rootNode.addChildNode(node)
        }
        func light(_ type: SCNLight.LightType, _ intensity: CGFloat, _ position: SCNVector3, _ color: NSColor) {
            let node = SCNNode(); node.light = SCNLight(); node.light?.type = type
            node.light?.intensity = intensity; node.light?.color = color; node.position = position
            node.look(at: SCNVector3(0, 0, 0)); scene.rootNode.addChildNode(node)
        }
        light(.ambient, 140, SCNVector3(0, 0, 0), NSColor(calibratedRed: 0.77, green: 0.74, blue: 0.9, alpha: 1))
        light(.omni, 500, SCNVector3(-3, 4, 5), NSColor(calibratedRed: 1, green: 0.93, blue: 0.83, alpha: 1))
        light(.omni, 300, SCNVector3(3, 1, -3), NSColor(calibratedRed: 0.63, green: 0.44, blue: 1, alpha: 1))
        return scene
    }
}
