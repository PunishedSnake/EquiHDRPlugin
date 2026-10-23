/*
 * EquiHDRPlugin native hook bridge
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";

import { app, type IpcMainInvokeEvent } from "electron";

const PRELOAD_WORLD_ID = 999;
const RELEASE_BASE = "https://github.com/PunishedSnake/EquiHDRPlugin/releases/download/hook-latest";
const ADDON_NAME = "equihdr_hook.node";
const HASH_NAME = "equihdr_hook.node.sha256";

export interface ToneMapConfig {
    enabled: boolean;
    toneMapper: number;
    exposureEv: number;
    paperWhiteNits: number;
    peakNits: number;
    outputGain: number;
    gammaCompensation: number;
    saturation: number;
}

export interface HookResult {
    ok: boolean;
    message: string;
    installed?: boolean;
    target?: string;
    addonPath?: string;
    [key: string]: any;
}

function hookDirectory(): string {
    return join(app.getPath("userData"), "EquiHDRPlugin", "native-hook");
}

async function fetchBytes(url: string): Promise<Buffer> {
    const response = await fetch(url, { redirect: "follow", cache: "no-store" });
    if (!response.ok)
        throw new Error(`Download failed: ${response.status} ${response.statusText}`);

    return Buffer.from(await response.arrayBuffer());
}

async function fetchText(url: string): Promise<string> {
    const response = await fetch(url, { redirect: "follow", cache: "no-store" });
    if (!response.ok)
        throw new Error(`Download failed: ${response.status} ${response.statusText}`);

    return response.text();
}

function parseExpectedSha256(text: string): string {
    const match = text.trim().match(/^([a-fA-F0-9]{64})(?:\s+.+)?$/);
    if (!match) throw new Error("Invalid native-hook SHA-256 file");
    return match[1].toLowerCase();
}

async function fileSha256(path: string): Promise<string> {
    return createHash("sha256").update(await readFile(path)).digest("hex");
}

async function resolveHookAddon(): Promise<string> {
    if (process.platform !== "win32")
        throw new Error("EquiHDR native D3D11 hook currently supports Windows only");

    const dir = hookDirectory();
    await mkdir(dir, { recursive: true });

    // Use a hash-versioned filename because Windows keeps loaded .node modules
    // locked. A new release can then coexist with the previous loaded binary
    // until Discord is restarted.
    const hashText = await fetchText(`${RELEASE_BASE}/${HASH_NAME}`);
    const expected = parseExpectedSha256(hashText);
    const target = join(dir, `equihdr_hook-${expected.slice(0, 16)}.node`);

    if (existsSync(target)) {
        try {
            if (await fileSha256(target) === expected) return target;
        } catch {
            // Re-download below.
        }
    }

    const binary = await fetchBytes(`${RELEASE_BASE}/${ADDON_NAME}`);
    const actual = createHash("sha256").update(binary).digest("hex");
    if (actual !== expected)
        throw new Error(`Native-hook SHA-256 mismatch. Expected ${expected}, got ${actual}`);

    await writeFile(target, binary);
    return target;
}

function isolatedRequireBootstrap(addonPath: string) {
    return `
        const requireFn =
            typeof globalThis.require === "function"
                ? globalThis.require
                : typeof globalThis.module?.require === "function"
                    ? globalThis.module.require.bind(globalThis.module)
                    : (() => {
                        const moduleBuiltin = globalThis.process?.getBuiltinModule?.("module")
                            ?? globalThis.process?.getBuiltinModule?.("node:module");

                        if (!moduleBuiltin?.createRequire) {
                            throw new Error("No require function available in isolated world");
                        }

                        return moduleBuiltin.createRequire(${JSON.stringify(addonPath)});
                    })();

        const equiHdrHook = requireFn(${JSON.stringify(addonPath)});
    `;
}

async function invokeAddon(
    event: IpcMainInvokeEvent,
    method: string,
    args: unknown[] = []
): Promise<HookResult> {
    const addonPath = await resolveHookAddon();
    const encodedArgs = args.map(arg => JSON.stringify(arg)).join(",");

    const result = await event.sender.executeJavaScriptInIsolatedWorld(PRELOAD_WORLD_ID, [{
        code: `(() => {
            try {
                ${isolatedRequireBootstrap(addonPath)}
                if (typeof equiHdrHook[${JSON.stringify(method)}] !== "function") {
                    throw new Error("Native hook does not export ${method}");
                }
                return equiHdrHook[${JSON.stringify(method)}](${encodedArgs});
            } catch (error) {
                return {
                    ok: false,
                    message: error instanceof Error
                        ? \`\${error.name}: \${error.message}\`
                        : String(error)
                };
            }
        })();`
    }]);

    if (result == null) {
        return { ok: false, message: "Native hook isolated-world execution returned no result", addonPath };
    }

    return { addonPath, ...result };
}

export async function ensureHookAddon(_: IpcMainInvokeEvent): Promise<HookResult> {
    try {
        const addonPath = await resolveHookAddon();
        return { ok: true, message: "Native D3D11 hook is ready", addonPath };
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function installHook(event: IpcMainInvokeEvent, config: ToneMapConfig): Promise<HookResult> {
    try {
        return await invokeAddon(event, "installHook", [config]);
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function updateHook(event: IpcMainInvokeEvent, config: ToneMapConfig): Promise<HookResult> {
    try {
        return await invokeAddon(event, "updateConfig", [config]);
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function removeHook(event: IpcMainInvokeEvent): Promise<HookResult> {
    try {
        return await invokeAddon(event, "removeHook");
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function hookStats(event: IpcMainInvokeEvent): Promise<HookResult> {
    try {
        return await invokeAddon(event, "getStats");
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function resetHookStats(event: IpcMainInvokeEvent): Promise<HookResult> {
    try {
        return await invokeAddon(event, "resetStats");
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}
