/*
 * EquiHDRPlugin native D3D11 capture hook
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hooks ID3D11VideoContext::VideoProcessorBlt inside the Discord renderer
 * process. When Discord feeds an FP16 scRGB desktop-capture texture into the
 * video processor, the texture is tone-mapped in place on the GPU before the
 * original Discord/WebRTC conversion runs.
 */

#include <napi.h>

#define CINTERFACE
#define COBJMACROS
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace {

using VideoProcessorBltFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11VideoContext*,
    ID3D11VideoProcessor*,
    ID3D11VideoProcessorOutputView*,
    UINT,
    UINT,
    const D3D11_VIDEO_PROCESSOR_STREAM*);

struct ToneConfig {
    std::atomic<bool> enabled{true};
    std::atomic<int> toneMapper{2}; // 0 clip, 1 Reinhard, 2 ACES, 3 Hable
    std::atomic<float> exposureEv{0.0f};
    std::atomic<float> paperWhiteNits{203.0f};
    std::atomic<float> peakNits{1000.0f};
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> gammaCompensation{1.0f};
    std::atomic<float> saturation{1.0f};
};

struct alignas(16) ToneConstants {
    float exposureEv;
    float paperWhiteNits;
    float peakNits;
    float saturation;
    float gammaCompensation;
    float toneMapper;
    float outputGain;
    float padding;
};

ToneConfig g_config;
std::atomic<bool> g_hookInstalled{false};
std::atomic<uint64_t> g_bltCalls{0};
std::atomic<uint64_t> g_fp16Candidates{0};
std::atomic<uint64_t> g_processedFrames{0};
std::atomic<uint32_t> g_lastWidth{0};
std::atomic<uint32_t> g_lastHeight{0};
std::atomic<int> g_lastInputFormat{DXGI_FORMAT_UNKNOWN};
std::atomic<int> g_lastOutputFormat{DXGI_FORMAT_UNKNOWN};
std::mutex g_errorMutex;
std::string g_lastError;

VideoProcessorBltFn g_originalVideoProcessorBlt = nullptr;
void* g_videoProcessorBltTarget = nullptr;
thread_local bool g_insideHook = false;

const char kShaderSource[] = R"HLSL(
Texture2D<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

cbuffer ToneConstants : register(b0) {
    float exposureEv;
    float paperWhiteNits;
    float peakNits;
    float saturation;
    float gammaCompensation;
    float toneMapper;
    float outputGain;
    float padding;
};

struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VSOut output;
    output.position = float4(positions[id], 0.0, 1.0);
    output.uv = uvs[id];
    return output;
}

float3 AcesApprox(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 HableCurve(float3 x) {
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) /
           (x * (A * x + B) + D * F)) - E / F;
}

float3 ApplyToneMap(float3 scRgb) {
    // scRGB uses 1.0 == 80 nits. Convert to a paper-white-relative linear
    // domain before applying the selected shoulder curve.
    float paper = max(paperWhiteNits, 80.0);
    float peak = max(peakNits, paper + 1.0);
    float white = max(peak / paper, 1.01);
    float exposure = exp2(exposureEv);
    float3 x = max(scRgb, 0.0) * (80.0 / paper) * exposure;

    float3 mapped;
    if (toneMapper < 0.5) {
        mapped = saturate(x);
    } else if (toneMapper < 1.5) {
        float white2 = white * white;
        mapped = (x * (1.0 + x / white2)) / (1.0 + x);
    } else if (toneMapper < 2.5) {
        float3 denom = max(AcesApprox(white.xxx), 0.0001);
        mapped = AcesApprox(x) / denom;
    } else {
        float3 denom = max(HableCurve(white.xxx), 0.0001);
        mapped = HableCurve(x) / denom;
    }

    mapped *= outputGain;

    float luma = dot(mapped, float3(0.2126, 0.7152, 0.0722));
    mapped = lerp(luma.xxx, mapped, saturation);
    mapped = saturate(mapped);

    // Deliberate compensation control. 1.0 keeps the shader output linear;
    // values >1 darken the result if Discord/driver applies an unwanted lift.
    mapped = pow(max(mapped, 0.000001), max(gammaCompensation, 0.05));
    return saturate(mapped);
}

float4 PSMain(VSOut input) : SV_TARGET {
    float3 hdr = sourceTexture.Sample(sourceSampler, input.uv).rgb;
    return float4(ApplyToneMap(hdr), 1.0);
}
)HLSL";

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->lpVtbl->Release(value);
        value = nullptr;
    }
}

void SetLastError(const std::string& value) {
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_lastError = value;
}

std::string HResultString(HRESULT hr) {
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return stream.str();
}

std::string PointerString(const void* pointer) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << reinterpret_cast<uintptr_t>(pointer);
    return stream.str();
}

const char* FormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_NV12: return "NV12";
        case DXGI_FORMAT_P010: return "P010";
        case DXGI_FORMAT_P016: return "P016";
        case DXGI_FORMAT_420_OPAQUE: return "420_OPAQUE";
        default: return "OTHER";
    }
}

struct GpuCache {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    ID3D11DeviceContext* deferred = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* constants = nullptr;
    ID3D11Texture2D* shaderInput = nullptr;
    ID3D11ShaderResourceView* shaderInputView = nullptr;
    ID3D11Texture2D* shaderOutput = nullptr;
    ID3D11RenderTargetView* shaderOutputView = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    void ResetFrameResources() {
        SafeRelease(shaderOutputView);
        SafeRelease(shaderOutput);
        SafeRelease(shaderInputView);
        SafeRelease(shaderInput);
        width = 0;
        height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }

    void Reset() {
        ResetFrameResources();
        SafeRelease(constants);
        SafeRelease(sampler);
        SafeRelease(pixelShader);
        SafeRelease(vertexShader);
        SafeRelease(deferred);
        SafeRelease(immediate);
        SafeRelease(device);
    }
};

GpuCache g_cache;
std::mutex g_cacheMutex;

bool CompileShader(const char* entry, const char* target, ID3DBlob** output) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        kShaderSource,
        sizeof(kShaderSource) - 1,
        "EquiHDRHook",
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        output,
        &errors);

    if (FAILED(hr)) {
        std::string message = "D3DCompile failed: " + HResultString(hr);
        if (errors) {
            const char* text = static_cast<const char*>(errors->lpVtbl->GetBufferPointer(errors));
            const size_t size = errors->lpVtbl->GetBufferSize(errors);
            if (text && size) message += " - " + std::string(text, size);
        }
        SetLastError(message);
        SafeRelease(errors);
        return false;
    }

    SafeRelease(errors);
    return true;
}

bool CreateStaticGpuResources(ID3D11Device* device) {
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;

    if (!CompileShader("VSMain", "vs_5_0", &vsBlob)) return false;
    if (!CompileShader("PSMain", "ps_5_0", &psBlob)) {
        SafeRelease(vsBlob);
        return false;
    }

    HRESULT hr = device->lpVtbl->CreateVertexShader(
        device,
        vsBlob->lpVtbl->GetBufferPointer(vsBlob),
        vsBlob->lpVtbl->GetBufferSize(vsBlob),
        nullptr,
        &g_cache.vertexShader);
    if (FAILED(hr)) {
        SetLastError("CreateVertexShader failed: " + HResultString(hr));
        SafeRelease(vsBlob);
        SafeRelease(psBlob);
        return false;
    }

    hr = device->lpVtbl->CreatePixelShader(
        device,
        psBlob->lpVtbl->GetBufferPointer(psBlob),
        psBlob->lpVtbl->GetBufferSize(psBlob),
        nullptr,
        &g_cache.pixelShader);
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    if (FAILED(hr)) {
        SetLastError("CreatePixelShader failed: " + HResultString(hr));
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->lpVtbl->CreateSamplerState(device, &samplerDesc, &g_cache.sampler);
    if (FAILED(hr)) {
        SetLastError("CreateSamplerState failed: " + HResultString(hr));
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ToneConstants);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->lpVtbl->CreateBuffer(device, &bufferDesc, nullptr, &g_cache.constants);
    if (FAILED(hr)) {
        SetLastError("CreateBuffer failed: " + HResultString(hr));
        return false;
    }

    hr = device->lpVtbl->CreateDeferredContext(device, 0, &g_cache.deferred);
    if (FAILED(hr)) {
        SetLastError("CreateDeferredContext failed: " + HResultString(hr));
        return false;
    }

    device->lpVtbl->GetImmediateContext(device, &g_cache.immediate);
    if (!g_cache.immediate) {
        SetLastError("GetImmediateContext returned null");
        return false;
    }

    return true;
}

bool EnsureGpuCache(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& sourceDesc) {
    if (g_cache.device != device) {
        g_cache.Reset();
        g_cache.device = device;
        g_cache.device->lpVtbl->AddRef(g_cache.device);
        if (!CreateStaticGpuResources(device)) {
            g_cache.Reset();
            return false;
        }
    }

    if (g_cache.width == sourceDesc.Width &&
        g_cache.height == sourceDesc.Height &&
        g_cache.format == sourceDesc.Format &&
        g_cache.shaderInput && g_cache.shaderOutput) {
        return true;
    }

    g_cache.ResetFrameResources();

    D3D11_TEXTURE2D_DESC inputDesc = sourceDesc;
    inputDesc.Usage = D3D11_USAGE_DEFAULT;
    inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    inputDesc.CPUAccessFlags = 0;
    inputDesc.MiscFlags = 0;

    HRESULT hr = device->lpVtbl->CreateTexture2D(device, &inputDesc, nullptr, &g_cache.shaderInput);
    if (FAILED(hr)) {
        SetLastError("CreateTexture2D(shaderInput) failed: " + HResultString(hr));
        return false;
    }

    hr = device->lpVtbl->CreateShaderResourceView(
        device,
        reinterpret_cast<ID3D11Resource*>(g_cache.shaderInput),
        nullptr,
        &g_cache.shaderInputView);
    if (FAILED(hr)) {
        SetLastError("CreateShaderResourceView failed: " + HResultString(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC outputDesc = sourceDesc;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    outputDesc.CPUAccessFlags = 0;
    outputDesc.MiscFlags = 0;

    hr = device->lpVtbl->CreateTexture2D(device, &outputDesc, nullptr, &g_cache.shaderOutput);
    if (FAILED(hr)) {
        SetLastError("CreateTexture2D(shaderOutput) failed: " + HResultString(hr));
        return false;
    }

    hr = device->lpVtbl->CreateRenderTargetView(
        device,
        reinterpret_cast<ID3D11Resource*>(g_cache.shaderOutput),
        nullptr,
        &g_cache.shaderOutputView);
    if (FAILED(hr)) {
        SetLastError("CreateRenderTargetView failed: " + HResultString(hr));
        return false;
    }

    g_cache.width = sourceDesc.Width;
    g_cache.height = sourceDesc.Height;
    g_cache.format = sourceDesc.Format;
    return true;
}

bool ToneMapTexture(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc) {
    ID3D11Device* device = nullptr;
    source->lpVtbl->GetDevice(source, &device);
    if (!device) {
        SetLastError("Input texture returned no D3D11 device");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    const bool cacheOk = EnsureGpuCache(device, desc);
    SafeRelease(device);
    if (!cacheOk) return false;

    ToneConstants constants{};
    constants.exposureEv = g_config.exposureEv.load();
    constants.paperWhiteNits = g_config.paperWhiteNits.load();
    constants.peakNits = g_config.peakNits.load();
    constants.saturation = g_config.saturation.load();
    constants.gammaCompensation = g_config.gammaCompensation.load();
    constants.toneMapper = static_cast<float>(g_config.toneMapper.load());
    constants.outputGain = g_config.outputGain.load();

    ID3D11DeviceContext* context = g_cache.deferred;
    context->lpVtbl->ClearState(context);

    context->lpVtbl->CopyResource(
        context,
        reinterpret_cast<ID3D11Resource*>(g_cache.shaderInput),
        reinterpret_cast<ID3D11Resource*>(source));

    context->lpVtbl->UpdateSubresource(
        context,
        reinterpret_cast<ID3D11Resource*>(g_cache.constants),
        0,
        nullptr,
        &constants,
        0,
        0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    context->lpVtbl->RSSetViewports(context, 1, &viewport);
    context->lpVtbl->OMSetRenderTargets(context, 1, &g_cache.shaderOutputView, nullptr);
    context->lpVtbl->IASetInputLayout(context, nullptr);
    context->lpVtbl->IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->lpVtbl->VSSetShader(context, g_cache.vertexShader, nullptr, 0);
    context->lpVtbl->PSSetShader(context, g_cache.pixelShader, nullptr, 0);
    context->lpVtbl->PSSetShaderResources(context, 0, 1, &g_cache.shaderInputView);
    context->lpVtbl->PSSetSamplers(context, 0, 1, &g_cache.sampler);
    context->lpVtbl->PSSetConstantBuffers(context, 0, 1, &g_cache.constants);
    context->lpVtbl->Draw(context, 3, 0);

    ID3D11ShaderResourceView* nullView = nullptr;
    context->lpVtbl->PSSetShaderResources(context, 0, 1, &nullView);

    context->lpVtbl->CopyResource(
        context,
        reinterpret_cast<ID3D11Resource*>(source),
        reinterpret_cast<ID3D11Resource*>(g_cache.shaderOutput));

    ID3D11CommandList* commandList = nullptr;
    HRESULT hr = context->lpVtbl->FinishCommandList(context, FALSE, &commandList);
    if (FAILED(hr) || !commandList) {
        SetLastError("FinishCommandList failed: " + HResultString(hr));
        SafeRelease(commandList);
        return false;
    }

    g_cache.immediate->lpVtbl->ExecuteCommandList(g_cache.immediate, commandList, TRUE);
    SafeRelease(commandList);
    return true;
}

bool GetTextureFromInputView(
    ID3D11VideoProcessorInputView* view,
    ID3D11Texture2D** texture,
    D3D11_TEXTURE2D_DESC* desc) {
    if (!view || !texture || !desc) return false;

    ID3D11Resource* resource = nullptr;
    view->lpVtbl->GetResource(view, &resource);
    if (!resource) return false;

    ID3D11Texture2D* result = nullptr;
    HRESULT hr = resource->lpVtbl->QueryInterface(
        resource,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&result));
    SafeRelease(resource);
    if (FAILED(hr) || !result) return false;

    result->lpVtbl->GetDesc(result, desc);
    *texture = result;
    return true;
}

DXGI_FORMAT GetOutputFormat(ID3D11VideoProcessorOutputView* view) {
    if (!view) return DXGI_FORMAT_UNKNOWN;

    ID3D11Resource* resource = nullptr;
    view->lpVtbl->GetResource(view, &resource);
    if (!resource) return DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = resource->lpVtbl->QueryInterface(
        resource,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&texture));
    SafeRelease(resource);
    if (FAILED(hr) || !texture) return DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC desc{};
    texture->lpVtbl->GetDesc(texture, &desc);
    SafeRelease(texture);
    return desc.Format;
}

HRESULT STDMETHODCALLTYPE HookVideoProcessorBlt(
    ID3D11VideoContext* self,
    ID3D11VideoProcessor* processor,
    ID3D11VideoProcessorOutputView* outputView,
    UINT outputFrame,
    UINT streamCount,
    const D3D11_VIDEO_PROCESSOR_STREAM* streams) {

    g_bltCalls.fetch_add(1, std::memory_order_relaxed);

    if (!g_originalVideoProcessorBlt || g_insideHook || !g_config.enabled.load() || !streams) {
        return g_originalVideoProcessorBlt
            ? g_originalVideoProcessorBlt(self, processor, outputView, outputFrame, streamCount, streams)
            : E_FAIL;
    }

    g_insideHook = true;
    g_lastOutputFormat.store(static_cast<int>(GetOutputFormat(outputView)), std::memory_order_relaxed);

    for (UINT index = 0; index < streamCount; ++index) {
        const auto& stream = streams[index];
        if (!stream.Enable || !stream.pInputSurface) continue;

        ID3D11Texture2D* texture = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        if (!GetTextureFromInputView(stream.pInputSurface, &texture, &desc)) continue;

        g_lastInputFormat.store(static_cast<int>(desc.Format), std::memory_order_relaxed);
        g_lastWidth.store(desc.Width, std::memory_order_relaxed);
        g_lastHeight.store(desc.Height, std::memory_order_relaxed);

        const bool candidate =
            desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            desc.Width >= 256 &&
            desc.Height >= 144 &&
            desc.ArraySize == 1 &&
            desc.MipLevels == 1 &&
            desc.SampleDesc.Count == 1;

        if (candidate) {
            g_fp16Candidates.fetch_add(1, std::memory_order_relaxed);
            if (ToneMapTexture(texture, desc)) {
                g_processedFrames.fetch_add(1, std::memory_order_relaxed);
            }
        }

        SafeRelease(texture);

        // A Go Live desktop source is normally one stream. Process the first FP16
        // candidate only so unrelated auxiliary streams are left untouched.
        if (candidate) break;
    }

    g_insideHook = false;
    return g_originalVideoProcessorBlt(self, processor, outputView, outputFrame, streamCount, streams);
}

bool InstallNativeHook(std::string& error) {
    if (g_hookInstalled.load()) return true;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context);

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device,
            &featureLevel,
            &context);
    }

    if (FAILED(hr) || !device || !context) {
        error = "Could not create a D3D11 video-capable device: " + HResultString(hr);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    ID3D11VideoContext* videoContext = nullptr;
    hr = context->lpVtbl->QueryInterface(
        context,
        __uuidof(ID3D11VideoContext),
        reinterpret_cast<void**>(&videoContext));
    if (FAILED(hr) || !videoContext) {
        error = "Could not query ID3D11VideoContext: " + HResultString(hr);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    g_videoProcessorBltTarget = reinterpret_cast<void*>(videoContext->lpVtbl->VideoProcessorBlt);
    SafeRelease(videoContext);
    SafeRelease(context);
    SafeRelease(device);

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        error = "MH_Initialize failed: " + std::to_string(static_cast<int>(status));
        return false;
    }

    status = MH_CreateHook(
        g_videoProcessorBltTarget,
        reinterpret_cast<void*>(&HookVideoProcessorBlt),
        reinterpret_cast<void**>(&g_originalVideoProcessorBlt));
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) {
        error = "MH_CreateHook failed: " + std::to_string(static_cast<int>(status));
        MH_Uninitialize();
        return false;
    }

    status = MH_EnableHook(g_videoProcessorBltTarget);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        error = "MH_EnableHook failed: " + std::to_string(static_cast<int>(status));
        MH_RemoveHook(g_videoProcessorBltTarget);
        MH_Uninitialize();
        return false;
    }

    g_hookInstalled.store(true);
    SetLastError("");
    return true;
}

void RemoveNativeHook() {
    if (g_hookInstalled.exchange(false)) {
        if (g_videoProcessorBltTarget) {
            MH_DisableHook(g_videoProcessorBltTarget);
            MH_RemoveHook(g_videoProcessorBltTarget);
        }
        MH_Uninitialize();
    }

    g_originalVideoProcessorBlt = nullptr;
    g_videoProcessorBltTarget = nullptr;

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.Reset();
}

float ReadFloat(const Napi::Object& object, const char* name, float fallback, float minimum, float maximum) {
    if (!object.Has(name)) return fallback;
    Napi::Value value = object.Get(name);
    if (!value.IsNumber()) return fallback;
    return std::clamp(value.As<Napi::Number>().FloatValue(), minimum, maximum);
}

int ReadInt(const Napi::Object& object, const char* name, int fallback, int minimum, int maximum) {
    if (!object.Has(name)) return fallback;
    Napi::Value value = object.Get(name);
    if (!value.IsNumber()) return fallback;
    return std::clamp(value.As<Napi::Number>().Int32Value(), minimum, maximum);
}

void ApplyConfig(const Napi::Object& object) {
    if (object.Has("enabled") && object.Get("enabled").IsBoolean())
        g_config.enabled.store(object.Get("enabled").As<Napi::Boolean>().Value());

    g_config.toneMapper.store(ReadInt(object, "toneMapper", g_config.toneMapper.load(), 0, 3));
    g_config.exposureEv.store(ReadFloat(object, "exposureEv", g_config.exposureEv.load(), -6.0f, 4.0f));
    g_config.paperWhiteNits.store(ReadFloat(object, "paperWhiteNits", g_config.paperWhiteNits.load(), 80.0f, 500.0f));
    g_config.peakNits.store(ReadFloat(object, "peakNits", g_config.peakNits.load(), 100.0f, 10000.0f));
    g_config.outputGain.store(ReadFloat(object, "outputGain", g_config.outputGain.load(), 0.05f, 4.0f));
    g_config.gammaCompensation.store(ReadFloat(object, "gammaCompensation", g_config.gammaCompensation.load(), 0.2f, 4.0f));
    g_config.saturation.store(ReadFloat(object, "saturation", g_config.saturation.load(), 0.0f, 2.0f));
}

Napi::Object MakeStatus(Napi::Env env, bool ok, const std::string& message) {
    Napi::Object result = Napi::Object::New(env);
    result.Set("ok", Napi::Boolean::New(env, ok));
    result.Set("message", Napi::String::New(env, message));
    result.Set("installed", Napi::Boolean::New(env, g_hookInstalled.load()));
    result.Set("target", Napi::String::New(env, PointerString(g_videoProcessorBltTarget)));
    return result;
}

Napi::Value InstallHook(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() > 0 && info[0].IsObject()) ApplyConfig(info[0].As<Napi::Object>());

    std::string error;
    if (!InstallNativeHook(error)) {
        SetLastError(error);
        return MakeStatus(env, false, error);
    }

    return MakeStatus(env, true, "D3D11 VideoProcessorBlt hook installed");
}

Napi::Value UpdateConfig(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsObject())
        return MakeStatus(env, false, "Expected a configuration object");

    ApplyConfig(info[0].As<Napi::Object>());
    return MakeStatus(env, true, "Tone-map configuration updated");
}

Napi::Value RemoveHook(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    RemoveNativeHook();
    return MakeStatus(env, true, "D3D11 tone-map hook removed");
}

Napi::Value ResetStats(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    g_bltCalls.store(0);
    g_fp16Candidates.store(0);
    g_processedFrames.store(0);
    g_lastWidth.store(0);
    g_lastHeight.store(0);
    g_lastInputFormat.store(DXGI_FORMAT_UNKNOWN);
    g_lastOutputFormat.store(DXGI_FORMAT_UNKNOWN);
    SetLastError("");
    return MakeStatus(env, true, "Hook statistics reset");
}

Napi::Value GetStats(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object result = Napi::Object::New(env);
    const auto inputFormat = static_cast<DXGI_FORMAT>(g_lastInputFormat.load());
    const auto outputFormat = static_cast<DXGI_FORMAT>(g_lastOutputFormat.load());

    result.Set("installed", Napi::Boolean::New(env, g_hookInstalled.load()));
    result.Set("enabled", Napi::Boolean::New(env, g_config.enabled.load()));
    result.Set("target", Napi::String::New(env, PointerString(g_videoProcessorBltTarget)));
    result.Set("bltCalls", Napi::Number::New(env, static_cast<double>(g_bltCalls.load())));
    result.Set("fp16Candidates", Napi::Number::New(env, static_cast<double>(g_fp16Candidates.load())));
    result.Set("processedFrames", Napi::Number::New(env, static_cast<double>(g_processedFrames.load())));
    result.Set("width", Napi::Number::New(env, g_lastWidth.load()));
    result.Set("height", Napi::Number::New(env, g_lastHeight.load()));
    result.Set("inputFormat", Napi::Number::New(env, static_cast<int>(inputFormat)));
    result.Set("inputFormatName", Napi::String::New(env, FormatName(inputFormat)));
    result.Set("outputFormat", Napi::Number::New(env, static_cast<int>(outputFormat)));
    result.Set("outputFormatName", Napi::String::New(env, FormatName(outputFormat)));

    {
        std::lock_guard<std::mutex> lock(g_errorMutex);
        result.Set("lastError", Napi::String::New(env, g_lastError));
    }

    Napi::Object config = Napi::Object::New(env);
    config.Set("toneMapper", Napi::Number::New(env, g_config.toneMapper.load()));
    config.Set("exposureEv", Napi::Number::New(env, g_config.exposureEv.load()));
    config.Set("paperWhiteNits", Napi::Number::New(env, g_config.paperWhiteNits.load()));
    config.Set("peakNits", Napi::Number::New(env, g_config.peakNits.load()));
    config.Set("outputGain", Napi::Number::New(env, g_config.outputGain.load()));
    config.Set("gammaCompensation", Napi::Number::New(env, g_config.gammaCompensation.load()));
    config.Set("saturation", Napi::Number::New(env, g_config.saturation.load()));
    result.Set("config", config);
    return result;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("installHook", Napi::Function::New(env, InstallHook));
    exports.Set("updateConfig", Napi::Function::New(env, UpdateConfig));
    exports.Set("removeHook", Napi::Function::New(env, RemoveHook));
    exports.Set("getStats", Napi::Function::New(env, GetStats));
    exports.Set("resetStats", Napi::Function::New(env, ResetStats));
    return exports;
}

} // namespace

NODE_API_MODULE(equihdr_hook, Init)
