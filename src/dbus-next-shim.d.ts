declare module "dbus-next/lib/service/interface.js" {
    export class Interface {
        constructor(name: string)
        $name: string
        static configureMembers(members: {
            properties?: Record<string, { signature: string; access?: string }>
            methods?: Record<string, { inSignature?: string; outSignature?: string }>
            signals?: Record<string, { signature?: string }>
        }): void
    }
}