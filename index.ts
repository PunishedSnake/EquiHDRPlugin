/*
 * EquiHDRPlugin
 * Experimental HDR screenshare fix / diagnostics for Equicord.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { OptionType } from "@utils/types";

const logger = new Logger("EquiHDRPlugin");

type CaptureBackend = "default" | "wgc" | "videoHook";

const settings = definePluginSettings({
    forceSdr: {
        description: "Force Discord Go Live desktop capture to use hdrCaptureMode = \"never\".",
        type: OptionType.BOOLEAN,
        default: true
    },

    overrideHdrExperiment: {
        description: "Also force Discord's 2026-02-go-live-hdr experiment config to SDR/never.",
        type: OptionType.BOOLEAN,
        default: true
    },

    captureBackend: {
        description: "Choose which Windows desktop capture backend Discord should use.",
        type: OptionType.SELECT,
        options: [
            {
                label: "Windows Graphics Capture (recommended)",
                value: "wgc",
                default: true
            },
            {
                label: "Discord Video Hook",
                value: "videoHook"
            },
            {
                label: "Discord default",
                value: "default"
            }
        ]
    },

    debugLogging: {
        description: "Log requested and overridden Go Live capture parameters to DevTools.",
        type: OptionType.BOOLEAN,
        default: true
    }
});

function log(name: string, requested: unknown, resolved: unknown) {
    if (!settings.store.debugLogging) return;

    logger.info(name, {
        requested,
        resolved,
        backend: settings.store.captureBackend,
        forceSdr: settings.store.forceSdr,
        overrideHdrExperiment: settings.store.overrideHdrExperiment
    });
}

export default definePlugin({
    name: "EquiHDRPlugin",
    description: "Attempts to fix blown-out HDR Go Live streams by controlling Discord HDR capture and Windows capture backends.",
    authors: [{ name: "PunishedSnake", id: 0n }],
    settings,

    patches: [
        {
            // Patch the final DesktopDescription before it reaches MediaEngine.
            find: "setGoLiveSource({desktopDescription:{",
            replacement: [
                {
                    match: /(setGoLiveSource\(\{desktopDescription:\{.{0,800}?hdrCaptureMode:)(\i)/,
                    replace: "$1$self.getHdrCaptureMode($2)"
                },
                {
                    match: /(setGoLiveSource\(\{desktopDescription:\{.{0,800}?useGraphicsCapture:)(\i)/,
                    replace: "$1$self.getUseGraphicsCapture($2)"
                },
                {
                    match: /(setGoLiveSource\(\{desktopDescription:\{.{0,800}?useVideoHook:)(\i)/,
                    replace: "$1$self.getUseVideoHook($2)"
                }
            ]
        },

        {
            // Discord currently gates Go Live HDR through this experiment.
            find: "2026-02-go-live-hdr",
            replacement: {
                match: /return (\i)\.getConfig\(\{location:(\i)\}\)/,
                replace: "return $self.getGoLiveHdrExperimentConfig($1.getConfig({location:$2}))"
            }
        }
    ],

    getHdrCaptureMode(original: unknown) {
        const resolved = settings.store.forceSdr ? "never" : original;
        log("hdrCaptureMode", original, resolved);
        return resolved;
    },

    getUseGraphicsCapture(original: unknown) {
        const backend = settings.store.captureBackend as CaptureBackend;

        const resolved =
            backend === "wgc" ? true :
            backend === "videoHook" ? false :
            original;

        log("useGraphicsCapture", original, resolved);
        return resolved;
    },

    getUseVideoHook(original: unknown) {
        const backend = settings.store.captureBackend as CaptureBackend;

        const resolved =
            backend === "wgc" ? false :
            backend === "videoHook" ? true :
            original;

        log("useVideoHook", original, resolved);
        return resolved;
    },

    getGoLiveHdrExperimentConfig(original: any) {
        if (!settings.store.overrideHdrExperiment || !settings.store.forceSdr)
            return original;

        const resolved = {
            ...(original ?? {}),
            hdrCaptureMode: "never"
        };

        log("2026-02-go-live-hdr experiment", original, resolved);
        return resolved;
    }
});
