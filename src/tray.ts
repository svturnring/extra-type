import { sessionBus, type MessageBus } from "dbus-next"
import { Variant } from "dbus-next"
import { Interface } from "dbus-next/lib/service/interface.js"
import { log } from "./logger.js"

const SNI_PATH = "/StatusNotifierItem"
const SNI_IFACE = "org.kde.StatusNotifierItem"
const WATCHER_SERVICE = "org.kde.StatusNotifierWatcher"
const WATCHER_PATH = "/StatusNotifierWatcher"
const WATCHER_IFACE = "org.kde.StatusNotifierWatcher"

const MENU_PATH = "/MenuBar"
const MENU_IFACE = "com.canonical.dbusmenu"

export interface TrayCallbacks {
    onActivate: () => void
    onSecondaryActivate: () => void
    onSettings: () => void
    onQuit: () => void
}

class SniInterface extends Interface {
    public callbacks: TrayCallbacks
    public _icon: string
    public _listening: boolean

    constructor(callbacks: TrayCallbacks) {
        super(SNI_IFACE)
        this.callbacks = callbacks
        this._icon = "audio-input-microphone"
        this._listening = false
    }

    get Category(): string {
        return "ApplicationStatus"
    }

    get Id(): string {
        return "extra-type"
    }

    get Title(): string {
        return "Extra Type"
    }

    get Status(): string {
        return "Active"
    }

    get IconName(): string {
        return this._icon
    }

    get ItemIsMenu(): boolean {
        return false
    }

    get Menu(): string {
        return MENU_PATH
    }

    get WindowId(): number {
        return 0
    }

    get ToolTip(): unknown {
        return ["extra-type", [], this._listening ? "Listening" : "Idle", "Extra Type dictation"]
    }

    Activate(_x: number, _y: number): void {
        this.callbacks.onActivate()
    }

    SecondaryActivate(_x: number, _y: number): void {
        this.callbacks.onSecondaryActivate()
    }

    ContextMenu(_x: number, _y: number): void {}

    ScrollDelta(_delta: number, _orientation: string): void {}
}

SniInterface.configureMembers({
    properties: {
        Category: { signature: "s", access: "read" },
        Id: { signature: "s", access: "read" },
        Title: { signature: "s", access: "read" },
        Status: { signature: "s", access: "read" },
        IconName: { signature: "s", access: "read" },
        ItemIsMenu: { signature: "b", access: "read" },
        Menu: { signature: "o", access: "read" },
        WindowId: { signature: "i", access: "read" },
        ToolTip: { signature: "(sa(iiay)ss)", access: "read" },
    },
    methods: {
        Activate: { inSignature: "ii", outSignature: "" },
        SecondaryActivate: { inSignature: "ii", outSignature: "" },
        ContextMenu: { inSignature: "ii", outSignature: "" },
        ScrollDelta: { inSignature: "is", outSignature: "" },
    },
})

const ID_TOGGLE = 1
const ID_SEPARATOR = 2
const ID_SETTINGS = 3
const ID_QUIT = 4

type MenuAction = "toggle" | "separator" | "settings" | "quit"

class MenuInterface extends Interface {
    public callbacks: TrayCallbacks
    public _listening: boolean
    public revisionHandler: () => number

    constructor(callbacks: TrayCallbacks, revisionHandler: () => number) {
        super(MENU_IFACE)
        this.callbacks = callbacks
        this._listening = false
        this.revisionHandler = revisionHandler
    }

    GetLayout(parentId: number, recursionDepth: number, _propertyNames: string[]): [number, unknown] {
        const [id, props, children] = this.node(0)
        void parentId
        void recursionDepth
        return [this.revisionHandler(), [id, props, children]]
    }

    GetGroupProperties(ids: number[], _propertyNames: string[]): unknown[] {
        const wanted = new Set<number>(ids)
        const out: unknown[] = []
        for (const [nid, nprops] of this.allNodes()) {
            if (ids.length === 0 || wanted.has(nid)) {
                out.push([nid, nprops])
            }
        }
        return out
    }

    GetProperty(id: number, name: string): unknown {
        const [, props] = this.findNode(id)
        return props[name] ?? new Variant("s", "")
    }

    GetParent(id: number): number {
        const [parent] = this.findParent(id)
        return parent
    }

    AboutToShow(id: number): boolean {
        void id
        return false
    }

    AboutToShowGroup(ids: number[]): unknown[] {
        return ids.map(() => false)
    }

    Event(id: number, eventId: string, _data: unknown, _timestamp: number): void {
        if (eventId !== "clicked") return
        const { action } = this.actionForId(id)
        if (action === "toggle") {
            this.callbacks.onActivate()
        } else if (action === "settings") {
            this.callbacks.onSettings()
        } else if (action === "quit") {
            this.callbacks.onQuit()
        }
    }

    get Version(): number {
        return 3
    }

    get Status(): string {
        return "normal"
    }

    get TextDirection(): string {
        return "ltr"
    }

    get ChildrenDisplay(): string {
        return "submenus"
    }

    get IconThemePath(): string {
        return ""
    }

    private menuProps(id: number): Record<string, Variant> {
        const { action } = this.actionForId(id)
        const props: Record<string, Variant> = {
            visible: new Variant("b", true),
            enabled: new Variant("b", true),
        }
        if (action === "separator") {
            props["type"] = new Variant("s", "separator")
            props["enabled"] = new Variant("b", false)
            props["label"] = new Variant("s", "")
        } else {
            props["type"] = new Variant("s", "")
            props["label"] = new Variant("s", this.toggleLabel())
            if (action === "settings") {
                props["label"] = new Variant("s", "Settings...")
            } else if (action === "quit") {
                props["label"] = new Variant("s", "Quit")
            }
        }
        return props
    }

    private toggleLabel(): string {
        return this._listening ? "Pause dictation" : "Resume dictation"
    }

    private node(id: number): [number, Record<string, Variant>, unknown[]] {
        const children: unknown[] = []
        for (const cid of this.childrenOf(id)) {
            const [nid, np, nc] = this.node(cid)
            children.push(new Variant("(ia{sv}av)", [nid, np, nc]))
        }
        let props: Record<string, Variant>
        if (id === 0) {
            props = {}
        } else {
            props = this.menuProps(id)
        }
        return [id, props, children]
    }

    private childrenOf(id: number): number[] {
        switch (id) {
            case 0:
                return [ID_TOGGLE, ID_SEPARATOR, ID_SETTINGS, ID_QUIT]
            default:
                return []
        }
    }

    private actionForId(id: number): { action: MenuAction } {
        switch (id) {
            case ID_TOGGLE:
                return { action: "toggle" }
            case ID_SEPARATOR:
                return { action: "separator" }
            case ID_SETTINGS:
                return { action: "settings" }
            case ID_QUIT:
                return { action: "quit" }
            default:
                return { action: "separator" }
        }
    }

    private findNode(id: number): [number, Record<string, Variant>] {
        if (id === 0) return [0, {}]
        return [id, this.menuProps(id)]
    }

    private findParent(id: number): [number] {
        if (id === 0) return [0]
        return [0]
    }

    private allNodes(): Array<[number, Record<string, Variant>]> {
        return [
            [ID_TOGGLE, this.menuProps(ID_TOGGLE)],
            [ID_SEPARATOR, this.menuProps(ID_SEPARATOR)],
            [ID_QUIT, this.menuProps(ID_QUIT)],
        ]
    }
}

MenuInterface.configureMembers({
    properties: {
        Version: { signature: "u", access: "read" },
        Status: { signature: "s", access: "read" },
        TextDirection: { signature: "s", access: "read" },
        ChildrenDisplay: { signature: "s", access: "read" },
        IconThemePath: { signature: "s", access: "read" },
    },
    methods: {
        GetLayout: { inSignature: "iias", outSignature: "u(ia{sv}av)" },
        GetGroupProperties: { inSignature: "aias", outSignature: "a(ia{sv})" },
        GetProperty: { inSignature: "is", outSignature: "v" },
        GetParent: { inSignature: "i", outSignature: "i" },
        AboutToShow: { inSignature: "i", outSignature: "b" },
        AboutToShowGroup: { inSignature: "ai", outSignature: "ab" },
        Event: { inSignature: "isvu", outSignature: "" },
    },
})

/**
 * StatusNotifierItem (SNI) tray icon registered on the session bus.
 * Appears in the system tray only while the daemon is running.
 * Icon reflects dictation state; left click toggles; right click
 * opens a DBusMenu context menu (pause/resume, quit).
 */
export class Tray {
    private bus: MessageBus | null = null
    private serviceName: string | null = null
    private iface: SniInterface | null = null
    private menu: MenuInterface | null = null
    private _listening = false
    private revision = 1

    constructor(callbacks: TrayCallbacks) {
        this.iface = new SniInterface(callbacks)
        this.menu = new MenuInterface(callbacks, () => this.revision)
        this.menu._listening = false
    }

    get listening(): boolean {
        return this._listening
    }

    set listening(v: boolean) {
        if (v === this._listening) return
        this._listening = v
        this.iface!._listening = v
        this.menu!._listening = v
        this.iface!._icon = v ? "microphone-sensitivity-high" : "audio-input-microphone"
        this.revision++
        this.emitIconChanged()
        this.emitLayoutUpdated()
    }

    private emitIconChanged() {
        if (!this.bus || !this.iface) return
        try {
            const signal = `org.freedesktop.DBus.Properties.PropertiesChanged`
            this.bus.emit(signal, [
                SNI_IFACE,
                { IconName: this.iface!._icon },
                [],
            ])
        } catch (e) {
            log("TRAY", `PropertiesChanged emit failed: ${e}`)
        }
    }

    private emitLayoutUpdated() {
        if (!this.bus || !this.menu) return
        try {
            const signal = `${MENU_IFACE}.LayoutUpdated`
            this.bus.emit(signal, [this.revision, 0])
        } catch (e) {
            log("TRAY", `LayoutUpdated emit failed: ${e}`)
        }
    }

    async start(): Promise<void> {
        if (this.bus) return
        try {
            const bus = sessionBus()
            this.bus = bus
            const id = process.pid
            this.serviceName = `org.kde.StatusNotifierItem-${id}-1`

            await bus.requestName(this.serviceName, 0)
            bus.export(SNI_PATH, this.iface as unknown as never)
            bus.export(MENU_PATH, this.menu as unknown as never)

            const proxy = await bus.getProxyObject(WATCHER_SERVICE, WATCHER_PATH)
            const watcher = proxy.getInterface(WATCHER_IFACE)
            await watcher.RegisterStatusNotifierItem(this.serviceName)

            log("TRAY", `registered SNI ${this.serviceName} with DBusMenu`)
        } catch (e) {
            const err = e as { name?: string; message?: string }
            log("TRAY", `failed to register SNI: ${err?.name ?? err?.message ?? e}`)
            this.dispose()
        }
    }

    dispose(): void {
        try {
            if (this.menu) {
                this.bus?.unexport(MENU_PATH, this.menu as unknown as never)
            }
            if (this.iface) {
                this.bus?.unexport(SNI_PATH, this.iface as unknown as never)
            }
        } catch (e) {
            log("TRAY", `unexport failed: ${e}`)
        }
        try {
            if (this.serviceName) {
                this.bus?.releaseName(this.serviceName).catch(() => {})
            }
        } catch (e) {
            log("TRAY", `releaseName failed: ${e}`)
        }
        try {
            this.bus?.disconnect()
        } catch (e) {
            log("TRAY", `disconnect failed: ${e}`)
        }
        this.bus = null
        this.serviceName = null
        this.iface = null
        this.menu = null
    }
}