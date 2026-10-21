/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
import QtQuick
import QtQuick.Layouts

import Muse.UiComponents

import Audacity.ProjectScene
import Audacity.Project 1.0

Item {
    id: root

    LiveSessionStatusModel {
        id: liveSessionStatus
    }

    Component.onCompleted: liveSessionStatus.init()

    property NavigationSection navigationSection: NavigationSection {
        id: navSec
        name: "ProjectStatusBar"
        enabled: root.enabled && root.visible
        order: 1000
    }

    NavigationPanel {
        id: liveNavPanel
        name: "LiveSessionStatus"
        enabled: root.enabled && root.visible && liveSessionStatus.active
        accessible.name: qsTrc("project", "Workstations in this live session")
        order: 0
        direction: NavigationPanel.Horizontal
        section: navSec
    }

    NavigationPanel {
        id: navPanel
        name: "ProjectStatusBar"
        enabled: root.enabled && root.visible
        accessible.name: qsTrc("appshell", "Selection status")
        order: 1
        direction: NavigationPanel.Horizontal
        section: navSec
    }

    RowLayout {
        id: statusBarRow

        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: 6

        height: parent.height

        RowLayout {
            visible: liveSessionStatus.active
            Layout.maximumWidth: Math.min(360, root.width * 0.4)
            spacing: 6

            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                radius: 4
                color: !liveSessionStatus.available ? "#C48722"
                       : liveSessionStatus.recording ? "#D64242" : "#4178BE"
            }

            FlatButton {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                text: liveSessionStatus.summary
                toolTipTitle: qsTrc("project", "Workstations in this live session")
                toolTipDescription: liveSessionStatus.details
                navigation.panel: liveNavPanel
                navigation.column: 0
                navigation.accessible.name: liveSessionStatus.summary
                onClicked: liveSessionStatus.showParticipants()
            }
        }

        Item {
            Layout.fillWidth: true
        }

        SelectionStatus {
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Layout.preferredWidth: width

            navigationPanel: navPanel
        }
    }
}
