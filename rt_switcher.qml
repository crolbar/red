/*

required programs:
    socat

*/

import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Widgets
import Quickshell.Wayland
import Quickshell.Io

ShellRoot {
    id: root

    property var windows: ({})

    Process {
        command: {
            ["sh", "-c", `wins(){ echo "windows" | socat - UNIX-CONNECT:"$RED_SOCKET";}; wins; (echo "sub"; tail -f /dev/null) | socat - UNIX-CONNECT:"$RED_SOCKET" 2> /dev/null | while read -r l; do wins; done`];
        }

        running: true

        stdout: SplitParser {
            splitMarker: "\n"
            onRead: d => {
                // trigger a rerender
                root.windows = ({});
                root.windows = JSON.parse(d);
            }
        }
    }

    Process {
        id: upProc
        command: ["sh", "-c", `echo "rt_fi_update" | socat - UNIX-CONNECT:"$RED_SOCKET"`]
        running: false
    }

    Loader {
        id: winLoader
        active: false
        sourceComponent: PanelWindow {
            exclusionMode: ExclusionMode.Ignore

            WlrLayershell.layer: WlrLayer.Overlay
            WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

            implicitHeight: grid.height
            implicitWidth: grid.width
            color: "#181818"

            Item {
                anchors.fill: parent

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: me => {
                        if (me.button == Qt.LeftButton) {
                            Quickshell.execDetached({
                                command: ["sh", "-c", `echo "rt_fi_update" | socat - UNIX-CONNECT:"$RED_SOCKET"`]
                            });
                        } else if (me.button == Qt.RightButton) {
                            winLoader.active = false;
                        }
                    }
                    onWheel: we => {
                        Quickshell.execDetached({
                            command: ["sh", "-c", `echo "focus_${we.angleDelta.y > 0 ? "prev" : "next"}" | socat - UNIX-CONNECT:"$RED_SOCKET"`]
                        });
                    }
                }

                focus: true
                Keys.onPressed: event => {
                    switch (event.key) {
                    case Qt.Key_Escape:
                    case Qt.Key_Return:
                        winLoader.active = false;
                        break;
                    case Qt.Key_Tab:
                    case Qt.Key_QuoteLeft:
                    case Qt.Key_Right:
                        Quickshell.execDetached({
                            command: ["sh", "-c", `echo "focus_next" | socat - UNIX-CONNECT:"$RED_SOCKET"`]
                        });
                        break;
                    case Qt.Key_Backtab:
                    case Qt.Key_AsciiTilde:
                    case Qt.Key_Left:
                        Quickshell.execDetached({
                            command: ["sh", "-c", `echo "focus_prev" | socat - UNIX-CONNECT:"$RED_SOCKET"`]
                        });
                        break;
                    }
                }

                GridLayout {
                    id: grid

                    anchors.margins: 20
                    columns: 5

                    Repeater {
                        model: root.windows

                        Rectangle {
                            id: item
                            required property var modelData

                            Layout.preferredWidth: 200
                            Layout.preferredHeight: 150
                            Layout.margins: 10

                            MouseArea {
                                id: ma
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: me => {
                                    Quickshell.execDetached({
                                        command: ["sh", "-c", `echo "focus_n ${modelData.idx}" | socat - UNIX-CONNECT:"$RED_SOCKET"`]
                                    });
                                    winLoader.active = false;
                                }
                            }

                            Loader {
                                anchors.bottom: text.top
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 5
                                anchors.rightMargin: 5
                                anchors.bottomMargin: -10
                                active: modelData.fi_path != "(null)"
                                sourceComponent: FrameImage {
                                    path: modelData.fi_path
                                }
                            }

                            color: (modelData.is_focused || ma.containsMouse) ? "#232323" : "transparent"

                            Text {
                                id: text
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                horizontalAlignment: Text.AlignHCenter
                                anchors.margins: 10
                                anchors.bottomMargin: 2

                                color: "white"
                                text: modelData.title
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }

    IpcHandler {
        target: "main"

        function toggle(): string {
            if (!winLoader.active)
                upProc.running = true;

            winLoader.active = !winLoader.active;
            return "ok";
        }
    }

    component FrameImage: IconImage {
        required property string path

        implicitHeight: 150
        source: `file://${path}`
    }
}
