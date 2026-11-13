// 换页动画覆盖层（QML 场景图实现）。
//
// 与 Python 版 src/component/widget/gpu_anim_cover.py 的对应关系：
//
//   QRhiWidget + 两个 QRhiViewport 偏移   →   两个 Image 的 x 偏移
//   setViewport(x = -off)                 →   prevImage.x = -off
//   setViewport(x = Wpx - off)            →   nextImage.x = width - off
//
// 为什么这样等价：GPU 覆盖层每帧的工作量 = "两张整页纹理 + 两个变换"，纹理只在
// Begin 时上传一次。QML 场景图的 Image 节点同样是"一次纹理上传 + 每帧只改节点
// 变换矩阵"，两者在 GPU 侧是同一件事，只是不再需要手写 QRhi 管线与 .qsb 着色器。
//
// 三条必须守住的约束（Python 版踩过）：
//   1. off 必须是**设备像素整数**：两页共用同一个 off，接缝才能严格相接；
//      各自取整会留下 1px 竖缝（亮边或旧页残留）。
//   2. 逐帧不重传纹理：source 只在切页时换一次，动画期间只改 x。
//   3. 方向语义：前进 = 旧页左移、新页从右侧进入；后退相反。
import QtQuick

Item {
    id: root

    // 两张页面快照，形如 image://pagecache/prev_3
    property url prevSource
    property url nextSource

    // 0 = 前进（旧页左移、新页从右入）；1 = 后退（相反）；2 = 同级交叉淡入
    property int mode: 0

    // 0..1 位移进度
    property real progress: 0.0

    // 设备像素比：把位移对齐到设备像素整数（见文件头约束 1）
    property real dpr: 1.0

    // 动画时长；0 表示不自己跑（由外部逐帧设置 progress，用于截图/回归）
    property int durationMs: 250

    // 是否正在播放（stop() 与自然结束要能区分开）
    property bool running: false

    // 位移：设备像素取整后再换回逻辑像素，两页共用。
    // 淡入模式不位移（同级页面之间没有前后关系，位移会误导层级）。
    readonly property real offset: {
        if (mode === 2)
            return 0
        const dev = Math.round(progress * width * dpr)
        return dpr > 0 ? dev / dpr : 0
    }

    // 淡入模式：旧页淡出、新页淡入；位移模式两张都不透明（接缝处不能透底）
    readonly property real prevOpacity: mode === 2 ? 1.0 - progress : 1.0
    readonly property real nextOpacity: mode === 2 ? progress : 1.0

    signal finished()

    clip: true

    Image {
        id: prevImage
        source: root.prevSource
        // cache: false —— 快照 URL 每次都带新序号，留着缓存只会无限涨
        cache: false
        asynchronous: false
        width: root.width
        height: root.height
        fillMode: Image.Stretch
        x: root.mode === 0 ? -root.offset : (root.mode === 1 ? root.offset : 0)
        opacity: root.prevOpacity
        y: 0
    }

    Image {
        id: nextImage
        source: root.nextSource
        cache: false
        asynchronous: false
        width: root.width
        height: root.height
        fillMode: Image.Stretch
        // 与旧页共用同一个 offset：接缝处两页的边界值完全相同，不会露缝
        x: root.mode === 0 ? root.width - root.offset
                           : (root.mode === 1 ? -root.width + root.offset : 0)
        opacity: root.nextOpacity
        y: 0
    }

    NumberAnimation {
        id: anim
        target: root
        property: "progress"
        from: 0.0
        to: 1.0
        duration: root.durationMs
        // 曲线按语义选：位移用 OutCubic（点按后快速离手、尾巴收敛），
        // 交叉淡入用 InOutQuad（透明度变化在线性下最容易看出"灰一下"）。
        easing.type: Easing.OutCubic
        onStopped: {
            if (root.running) {
                root.running = false
                root.finished()
            }
        }
    }

    function start() {
        anim.stop()
        progress = 0.0
        anim.easing.type = mode === 2 ? Easing.InOutQuad : Easing.OutCubic
        running = true
        if (durationMs > 0)
            anim.start()
        else
            finished()
    }

    function stop() {
        running = false
        anim.stop()
    }

    // 截图/回归：冻结在指定进度，不触发 finished
    function freeze(p) {
        running = false
        anim.stop()
        progress = Math.max(0.0, Math.min(1.0, p))
    }
}
