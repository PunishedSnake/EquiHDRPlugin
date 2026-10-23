/*
 * EquiHDRPlugin native helper bridge
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { createHash } from "node:crypto";
import { spawn, type ChildProcess } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdir, rename, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";

import { app, type IpcMainInvokeEvent } from "electron";

const RELEASE_BASE = "https://github.com/PunishedSnake/EquiHDRPlugin/releases/download/helper-latest";
const HELPER_NAME = "EquiHDRHelper.exe";
const HASH_NAME = "EquiHDRHelper.exe.sha256";
const LICENSE_NAME = "HDR-Corrector-LICENSE.txt";

let helperProcess: ChildProcess | null = null;

export interface HelperResult {
    ok: boolean;
    message: string;
    path?: string;
    pid?: number;
}

function helperDirectory(): string {
    return join(app.getPath("userData"), "EquiHDRPlugin");
}

function helperPath(): string {
    return join(helperDirectory(), HELPER_NAME);
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
    if (!match) throw new Error("Invalid helper SHA-256 file");
    return match[1].toLowerCase();
}

async function downloadHelper(force = false): Promise<string> {
    if (process.platform !== "win32")
        throw new Error("EquiHDR native mirror currently supports Windows only");

    const dir = helperDirectory();
    const exe = helperPath();
    await mkdir(dir, { recursive: true });

    if (existsSync(exe) && !force) return exe;

    const [hashText, binary, licenseText] = await Promise.all([
        fetchText(`${RELEASE_BASE}/${HASH_NAME}`),
        fetchBytes(`${RELEASE_BASE}/${HELPER_NAME}`),
        fetchText(`${RELEASE_BASE}/${LICENSE_NAME}`)
    ]);

    const expected = parseExpectedSha256(hashText);
    const actual = createHash("sha256").update(binary).digest("hex");
    if (actual !== expected)
        throw new Error(`Helper SHA-256 mismatch. Expected ${expected}, got ${actual}`);

    const temp = `${exe}.download`;
    await rm(temp, { force: true });
    await writeFile(temp, binary);
    await rm(exe, { force: true });
    await rename(temp, exe);
    await writeFile(join(dir, LICENSE_NAME), licenseText, "utf8");
    await writeFile(join(dir, HASH_NAME), `${expected}  ${HELPER_NAME}`, "utf8");

    return exe;
}

function processIsRunning(): boolean {
    return helperProcess != null && helperProcess.exitCode == null && !helperProcess.killed;
}

export async function ensureHelper(_: IpcMainInvokeEvent, forceDownload = false): Promise<HelperResult> {
    try {
        const path = await downloadHelper(forceDownload);
        return { ok: true, message: "HDR helper is ready", path };
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function launchMirror(_: IpcMainInvokeEvent, forceDownload = false): Promise<HelperResult> {
    try {
        if (processIsRunning()) {
            return {
                ok: true,
                message: "HDR helper is already running",
                path: helperPath(),
                pid: helperProcess?.pid
            };
        }

        const path = await downloadHelper(forceDownload);
        const child = spawn(path, ["--mirror"], {
            cwd: helperDirectory(),
            windowsHide: false,
            stdio: "ignore"
        });

        helperProcess = child;
        child.once("exit", () => {
            if (helperProcess === child) helperProcess = null;
        });
        child.once("error", () => {
            if (helperProcess === child) helperProcess = null;
        });

        return {
            ok: true,
            message: "EquiHDR SDR mirror launched",
            path,
            pid: child.pid
        };
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function stopHelper(_: IpcMainInvokeEvent): Promise<HelperResult> {
    try {
        if (!processIsRunning()) {
            helperProcess = null;
            return { ok: true, message: "HDR helper is not running", path: helperPath() };
        }

        helperProcess?.kill();
        helperProcess = null;
        return { ok: true, message: "HDR helper stopped", path: helperPath() };
    } catch (error: any) {
        return { ok: false, message: error?.message ?? String(error) };
    }
}

export async function helperStatus(_: IpcMainInvokeEvent): Promise<HelperResult> {
    return {
        ok: true,
        message: processIsRunning() ? "HDR helper is running" : "HDR helper is not running",
        path: helperPath(),
        pid: processIsRunning() ? helperProcess?.pid : undefined
    };
}
