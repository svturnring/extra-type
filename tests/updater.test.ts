import { describe, expect, test } from "bun:test"
import { compareVersions, pickLatestRelease } from "../src/updater.js"

describe("compareVersions", () => {
    test("equal versions", () => expect(compareVersions("3.0.0", "3.0.0")).toBe(0))
    test("newer patch", () => expect(compareVersions("3.0.1", "3.0.0")).toBe(1))
    test("newer minor", () => expect(compareVersions("3.1.0", "3.0.0")).toBe(1))
    test("newer major", () => expect(compareVersions("4.0.0", "3.0.0")).toBe(1))
    test("older", () => expect(compareVersions("2.0.0", "3.0.0")).toBe(-1))
    test("different lengths", () => expect(compareVersions("3.0", "3.0.0")).toBe(0))
})

describe("pickLatestRelease", () => {
    test("skips drafts, takes first published release (pre-release included)", () => {
        const rel = pickLatestRelease([
            { draft: true, tag_name: "v0.9.0-beta", assets: [] },
            { draft: false, tag_name: "v0.1.2-alpha", assets: [{ name: "extra-type-linux-x64.tar.gz", browser_download_url: "u1" }] },
            { draft: false, tag_name: "v0.1.1-alpha", assets: [] },
        ])
        expect(rel.tag).toBe("v0.1.2-alpha")
        expect(rel.version).toBe("0.1.2-alpha")
        expect(rel.assets[0].name).toBe("extra-type-linux-x64.tar.gz")
    })

    test("strips the v prefix in the version", () => {
        const rel = pickLatestRelease([{ draft: false, tag_name: "v0.1.2-alpha", assets: [] }])
        expect(rel.version).toBe("0.1.2-alpha")
    })

    test("throws when every release is a draft", () => {
        expect(() =>
            pickLatestRelease([
                { draft: true, tag_name: "v0.1.0-alpha", assets: [] },
            ]),
        ).toThrow("No published releases found")
    })
})
