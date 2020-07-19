import QtQuick 2.10
import QtQuick.Controls 2.1
import im.nheko 1.0
import im.nheko.EmojiModel 1.0

import "../"

ImageButton {
    property var colors: currentActivePalette
    property var emojiPicker
    property string event_id

    image: ":/icons/icons/ui/smile.png"
    id: emojiButton
    onClicked: emojiPicker.visible ? emojiPicker.close() : emojiPicker.show(emojiButton, event_id)

}
