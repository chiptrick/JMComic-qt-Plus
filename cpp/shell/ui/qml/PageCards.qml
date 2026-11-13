// 卡片墙列表（QML scene graph，GPU 渲染）。
//
// 为什么有这个文件：widgets 的 QListView 是 CPU 光栅化，真机 1440x3168@1.33dpr
// 滚动只有 ~30fps（每帧全视口/条带重绘 ≈ 32MB 内存操作）；换页动画走 QQuickWidget
// 场景图是 ~112fps。列表搬进来之后滚动与动画同一条 GPU 路径。
//
// 职责边界：几何（cardW/cellW/cellH）与配色全部由 C++（ComicGrid）算好 setProperty
// 进来，本文件只负责渲染与手势——列数规则只有一份（comic_view.cpp ApplyGeometry）。
//
// 与 CardPaint::Paint 的视觉对应：
//   封面（等比裁切）+ 右上角标（更新/完结）+ 底部进度条 + 标题（省略号）+ 作者行
// 颜色经 cover provider 的 image://covers/<b64 串>|<rev>：rev 只在封面到货时
// 变化，避免整屏 Image 一起重载（模型端按行 dataChanged）。
//
// 刻意只用 QtQuick 核心模块：repack 出的 APK 只带 qtquick2plugin，没有
// QtQuick.Controls（androiddeployqt 不会再跑）。滚动指示条用 visibleArea 自绘。
import QtQuick

Item {
    id: root

    // C++ 侧设置（ComicGrid::ApplyGeometry / SyncTheme）
    property int cardW: 168
    property int cellW: 186
    property int cellH: 320
    property color cBg: "#161616"
    property color cSurface: "#1e1e1e"
    property color cSurfaceHover: "#2a2a2a"
    property color cText: "#e6e6e6"
    property color cTextDim: "#9a9a9a"
    property color cAccent: "#ff8a00"

    // 字体：由 C++（ComicGrid::SyncFont）推下来，跟随设置页的"字体/字重/字号"。
    // 之前这里写死 font.pixelSize（13/11）与 Font.Medium —— 手机上调字体/字重/字宽
    // 时卡片文字毫无变化，调字号时格子变高而文字不变，看起来像"页面被拉长"。
    property string uiFontFamily: ""
    property int uiFontWeight: 400
    property real uiScale: 1.0
    // 文字块尺寸与 C++ 的 cellH 用同一套公式（见 ComicGrid::ApplyGeometry）
    property int titlePx: Math.round(13 * uiScale)
    property int metaPx: Math.round(11 * uiScale)
    property int titleH: Math.round(18 * uiScale)
    property int metaH: Math.round(14 * uiScale)
    property int textTop: Math.round(8 * uiScale)

    function scrollToTop() {
        list.positionViewAtBeginning()
    }

    function scrollToEnd() {
        list.positionViewAtEnd()
    }

    GridView {
        id: list
        anchors.fill: parent
        model: comicModel
        clip: true
        // 触摸滚动手感：QScroller 的近似参数（拖动跟手、fling 有惯性）
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 5000
        maximumFlickVelocity: 5000
        // 缓冲屏外一小段，快速滑动时 delegate 提前建好（封面仍受 LRU 保护）
        cacheBuffer: 600
        cellWidth: root.cellW
        cellHeight: root.cellH

        delegate: Item {
            width: root.cellW
            height: root.cellH

            // 卡片本体：格内居中（cellW 可能比 cardW 宽，例如最后一列）
            Item {
                id: card
                x: (parent.width - root.cardW) / 2
                y: 0
                width: root.cardW
                height: root.cellH
                // 按下反馈以卡片中心为缩放原点（默认是左上角，缩起来会"往左上缩"）
                transformOrigin: Item.Center
                scale: 1.0
                Behavior on scale {
                    NumberAnimation { duration: 90; easing.type: Easing.OutCubic }
                }

                // 封面：provider 返回的就是 cardW × cardW*4/3 的裁切结果
                Image {
                    id: cover
                    x: 0
                    y: 0
                    width: root.cardW
                    height: root.cardW * 4 / 3
                    // rev 变化（封面到货）→ URL 变化 → 重取；cache 关掉防止
                    // URL 代次累积占内存（provider 侧有自己的 LRU）
                    source: "image://covers/" + model.coverReq + "." + model.coverRev
                    cache: false
                    asynchronous: true
                    fillMode: Image.Stretch
                }

                // 右上角标：已完结 → "完结"（灰），否则 → "更新"（强调色）
                Rectangle {
                    x: root.cardW - badge.width - 8
                    y: 8
                    width: badge.width + 12
                    height: 18
                    color: Qt.rgba(0, 0, 0, 0.59)
                    visible: model.status !== ""
                    Text {
                        id: badge
                        anchors.centerIn: parent
                        text: model.status === "已完结" ? "完结" : "更新"
                        color: model.status === "已完结" ? root.cTextDim : root.cAccent
                        font.pixelSize: 10
                    }
                }

                // 阅读进度（底部细条）
                Rectangle {
                    x: 0
                    y: root.cardW * 4 / 3 - 3
                    width: root.cardW
                    height: 3
                    color: Qt.rgba(0, 0, 0, 0.43)
                    visible: model.chapters > 0 && model.readChapter > 0

                    Rectangle {
                        width: parent.width * model.readChapter / model.chapters
                        height: 3
                        color: root.cAccent
                    }
                }

                Text {
                    x: 1
                    y: root.cardW * 4 / 3 + root.textTop
                    width: root.cardW - 2
                    height: root.titleH
                    text: model.title
                    color: root.cText
                    font.family: root.uiFontFamily
                    // 标题比正文重一档（原来写死 Font.Medium，用户的字重设置进不来）
                    font.weight: Math.min(900, root.uiFontWeight + 100)
                    font.pixelSize: root.titlePx
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                Text {
                    x: 1
                    y: root.cardW * 4 / 3 + root.textTop + root.titleH + Math.round(2 * root.uiScale)
                    width: root.cardW - 2
                    height: root.metaH
                    // 文案由 C++（ComicMetaText）拼好：话数未知时只有作者，
                    // 不能在这里拼 "author · 0 话"（列表接口不返回章节数）。
                    text: model.metaText
                    color: root.cTextDim
                    font.family: root.uiFontFamily
                    font.weight: root.uiFontWeight
                    font.pixelSize: root.metaPx
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                MouseArea {
                    anchors.fill: parent
                    // 触摸拖动时 hover 无意义（还可能让列表在滚动中出现误高亮）
                    hoverEnabled: cardBridge ? !cardBridge.touchMode : false
                    onClicked: cardBridge.cardClicked(index)
                    onPressAndHold: cardBridge.cardLongPress(index)
                    // 按下：轻微内缩 + 一层薄底色。手指按下到页面切换之间有 ~250ms
                    // 的换页动画，这层反馈负责说明"这一次点击已经生效"。
                    onPressed: {
                        card.scale = 0.975
                        pressVeil.opacity = 0.45
                    }
                    onReleased: {
                        card.scale = 1.0
                        pressVeil.opacity = 0
                    }
                    // 拖动列表时 press 会被 GridView 抢走 → 必须复位，否则卡片会一直缩着
                    onCanceled: {
                        card.scale = 1.0
                        pressVeil.opacity = 0
                    }
                }

                // 按压底色：盖在整张卡片上（最后声明 = 最上层）
                Rectangle {
                    id: pressVeil
                    anchors.fill: parent
                    color: root.cSurfaceHover
                    opacity: 0
                    visible: opacity > 0.001
                    Behavior on opacity {
                        NumberAnimation { duration: 90; easing.type: Easing.OutCubic }
                    }
                }
            }
        }

        // 滚动指示条：visibleArea 自绘（QtQuick.Controls 不在 APK 里）
        Rectangle {
            parent: list
            visible: list.moving || list.flicking
            x: list.width - 4
            y: list.visibleArea.yPosition * list.height
            width: 3
            height: Math.max(48, list.visibleArea.heightRatio * list.height)
            radius: 1.5
            color: root.cAccent
            opacity: 0.55
        }
    }
}
