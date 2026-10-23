/*
 * EquiHDRPlugin
 * In-process HDR -> SDR correction for Discord Go Live.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { makeRange, OptionType, type PluginNative } from "@utils/types";

const logger = new Logger("EquiHDRPlugin");
const Native = VencordNative.pluginHelpers.EquiHDRPlugin as PluginNative<typeof import("./native")>;

type CaptureBackend = "default" | "wgc" | "videoHook";
type HdrCaptureMode = "discord" | "never" | "always";

let hookStarted = false;
let configUpdateTimer: ReturnType<typeof setTimeout> | null = null;

function queueHookConfigUpdate() {
    if (!hookStarted) return;
    if (configUpdateTimer != null) clearTimeout(configUpdateTimer);
    configUpdateTimer = setTimeout(() => {
        configUpdateTimer = null;
        void syncHookConfig();
    }, 80);
}

const settings = definePluginSettings({
    toneMapEnabled: {
        description: "Enable the in-process D3D11 HDR tone-map hook.",
        type: OptionType.BOOLEAN,
        default: true,
        onChange: queueHookConfigUpdate
    },

    toneMapper: {
        description: "Tone-mapping curve applied to FP16/scRGB capture frames before Discord encodes them.",
        type: OptionType.SELECT,
        options: [
            { label: "ACES approximation (recommended)", value: 2, default: true },
            { label: "Reinhard with white point", value: 1 },
            { label: "Hable / Uncharted 2", value: 3 },
            { label: "Hard clip (diagnostic)", value: 0 }
        ],
        onChange: queueHookConfigUpdate
    },

    exposureEv: {
        description: "Exposure before tone mapping, in EV. Negative values darken the HDR source.",
        type: OptionType.SLIDER,
        markers: makeRange(-4, 2, 0.25),
        default: 0,
        stickToMarkers: true,
        onChange: queueHookConfigUpdate
    },

    paperWhiteNits: {
        description: "HDR paper-white level. scRGB 1.0 is 80 nits; this controls how aggressively normal whites are compressed.",
        type: OptionType.SLIDER,
        markers: [80, 100, 120, 160, 203, 250, 300, 400, 500],
        default: 203,
        stickToMarkers: true,
        onChange: queueHookConfigUpdate
    },

    peakNits: {
        description: "Assumed source peak luminance used as the tone-map white point.",
        type: OptionType.SLIDER,
        markers: [400, 600, 800, 1000, 1200, 1500, 2000, 4000],
        default: 1000,
        stickToMarkers: true,
        onChange: queueHookConfigUpdate
    },

    outputGain: {
        description: "Final linear output gain after the tone-map curve.",
        type: OptionType.SLIDER,
        markers: makeRange(0.25, 1.5, 0.05),
        default: 1,
        stickToMarkers: true,
        onChange: queueHookConfigUpdate
    },

    gammaCompensation: {
        description: "Compensates for Discord/driver gamma lift. 1.0 is neutral; values above 1.0 make the encoded result darker.",
        type: OptionType.SLIDER,
        markers: makeRange(0.5, 2.5, 0.05),
        default: 1,
        stickToMarkers: true,
        onChange: queueHookConfigUpdate
    },

    saturation: {
        description: "Saturation after tone mapping.",
        type: OptionType.SLIDER,
        markers: makeRange(0, 1.5, 0.05),
        default: 1,
        stickToMarkers: true,
        onChange: queueHookConfigUpdate
    },

    hdrCaptureMode: {
        description: "HDR mode passed to Discord capture. Use Always so the native hook receives FP16/scRGB instead of an already-damaged SDR conversion.",
        type: OptionType.SELECT,
        options: [
            { label: "Always HDR for native hook (recommended)", value: "always", default: true },
            { label: "Never HDR", value: "never" },
            { label: "Discord default", value: "discord" }
        ]
    },

    overrideHdrExperiment: {
        description: "Override Discord's 2026-02-go-live-hdr experiment with the selected HDR capture mode.",
        type: OptionType.BOOLEAN,
        default: true
    },

    captureBackend: {
        description: "Capture backend. Windows Graphics Capture is the primary path for the native FP16 hook.",
        type: OptionType.SELECT,
        options: [
            { label: "Windows Graphics Capture (recommended)", value: "wgc", default: true },
            { label: "Discord Video Hook", value: "videoHook" },
            { label: "Discord default", value: "default" }
        ]
    },

    debugLogging: {
        description: "Log capture overrides and native-hook status to DevTools.",
        type: OptionType.BOOLEAN,
        default: true
    }
});

function toneMapConfig() {
    return {
        enabled: settings.store.toneMapEnabled,
        toneMapper: Number(settings.store.toneMapper),
        exposureEv: settings.store.exposureEv,
        paperWhiteNits: settings.store.paperWhiteNits,
        peakNits: settings.store.peakNits,
        outputGain: settings.store.outputGain,
        gammaCompensation: settings.store.gammaCompensation,
        saturation: settings.store.saturation
    };
}

async function syncHookConfig() {
    try {
        const result = await Native.updateHook(toneMapConfig());
        if (!result.ok) logger.error("Native hook config update failed:", result.message);
        else if (settings.store.debugLogging) logger.info(result.message, toneMapConfig());
    } catch (error) {
        logger.error("Native hook config update failed:", error);
    }
}

async function startNativeHook() {
    try {
        const nativeModules = globalThis.DiscordNative?.nativeModules;
        if (!nativeModules?.requireModule)
            throw new Error("DiscordNative.nativeModules is unavailable");

        // Ensure the voice/media module is resident in this renderer process before
        // loading our N-API hook next to it.
        nativeModules.requireModule("discord_voice");

        const result = await Native.installHook(toneMapConfig());
        if (!result.ok) {
            logger.error("Native D3D11 hook failed:", result.message);
            return;
        }

        hookStarted = true;
        logger.info("Native D3D11 tone-map hook installed", {
            target: result.target,
            addonPath: result.addonPath,
            config: toneMapConfig()
        });
    } catch (error) {
        logger.error("Native D3D11 hook failed:", error);
    }
}

function log(name: string, requested: unknown, resolved: unknown) {
    if (!settings.store.debugLogging) return;
    logger.info(name, {
        requested,
        resolved,
        backend: settings.store.captureBackend,
        hdrCaptureMode: settings.store.hdrCaptureMode
    });
}

export default definePlugin({
    name: "EquiHDRPlugin",
    description: "Hooks Discord's D3D11 capture path and tone-maps FP16/scRGB HDR frames in-process before Go Live encoding.",
    authors: [{ name: "PunishedSnake", id: 0n }],
    settings,

    patches: [
        {
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
            find: "2026-02-go-live-hdr",
            replacement: {
                match: /return (\i)\.getConfig\(\{location:(\i)\}\)/,
                replace: "return $self.getGoLiveHdrExperimentConfig($1.getConfig({location:$2}))"
            }
        }
    ],

    start() {
        void startNativeHook();
    },

    stop() {
        hookStarted = false;
        if (configUpdateTimer != null) {
            clearTimeout(configUpdateTimer);
            configUpdateTimer = null;
        }

        void Native.removeHook().then(result => {
            if (!result.ok) logger.error("Could not remove native hook:", result.message);
            else if (settings.store.debugLogging) logger.info(result.message);
        });
    },

    toolboxActions: {
        "Log EquiHDR hook stats": () => {
            void Native.hookStats().then(result => logger.info("Native hook stats", result));
        },
        "Reset EquiHDR hook stats": () => {
            void Native.resetHookStats().then(result => logger.info(result.message));
        }
    },

    getHdrCaptureMode(original: unknown) {
        const mode = settings.store.hdrCaptureMode as HdrCaptureMode;
        const resolved = mode === "discord" ? original : mode;
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
        const mode = settings.store.hdrCaptureMode as HdrCaptureMode;
        if (!settings.store.overrideHdrExperiment || mode === "discord") return original;

        const resolved = {
            ...(original ?? {}),
            hdrCaptureMode: mode
        };
        log("2026-02-go-live-hdr experiment", original, resolved);
        return resolved;
    }
});
