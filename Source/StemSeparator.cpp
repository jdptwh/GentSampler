// =============================================================================
//  StemSeparator.cpp  --  implementation
//
//  Build requirements (see STEM_ENGINE_NOTES.md):
//    - ONNX Runtime with the DirectML execution provider
//      (NuGet: Microsoft.ML.OnnxRuntime.DirectML), headers + onnxruntime.lib
//    - dml_provider_factory.h on the include path (ships with the DML package)
//
//  Every numbered comment block maps to the matching step in run_onnx.py.
// =============================================================================

#include "StemSeparator.h"

// Manual-init mode: the C++ API does NOT auto-call OrtGetApiBase (which would be
// a link-time import). We load onnxruntime.dll ourselves and feed the API table
// to Ort::InitApi at runtime, so the plugin has ZERO load-time dependency on it.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#undef ORT_API_MANUAL_INIT

#include <array>
#include <cmath>
#include <cstring>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

// Execution-provider entry points, resolved at runtime (no link-time imports).
// Each is null if this ORT build doesn't carry that provider, which lets the
// provider-priority chain in makeSession() probe-and-skip cleanly. Add future
// providers (CoreML, WebGPU) here as more fn-pointers + GetProcAddress lookups.
using CudaAppendFn  = OrtStatus* (ORT_API_CALL*) (OrtSessionOptions*, int);  // device_id
using DmlAppendFn   = OrtStatus* (ORT_API_CALL*) (OrtSessionOptions*, int);  // device_id (future insert)
static CudaAppendFn g_cudaAppend = nullptr;
static DmlAppendFn  g_dmlAppend  = nullptr;
// cudaSetDevice from cudart64_110.dll -- binds the calling (worker) thread to the
// CUDA device so ORT's CUDA EP creates/uses its context on the right thread.
using CudaSetDeviceFn = int (*) (int);
static CudaSetDeviceFn g_cudaSetDevice = nullptr;

// GPU groundwork is complete and the model is PROVEN to run on the RTX 2060 standalone
// (verified in Python with ORT 1.18 + cuDNN 8, full 7.8s model). But inside FL Studio's
// process the CUDA libs crash on use -- cudnn64_8.dll 0xC0000409 (stack/CFG fast-fail)
// and onnxruntime_providers_cuda.dll 0xC0000005 (access violation) per the Windows event
// log -- a host-process integration issue (mitigations / CUDA context), NOT a model or
// hardware fault. Until that's chased down with a debugger attached to the host, keep
// CUDA OFF so the GPU opt-in can never take FL down; the provider chain runs CPU. Flip
// this to true to resume GPU work (see the provider chain + the cudaSetDevice hook).
static const bool kEnableCuda = false;
static bool         g_ortReady   = false;
static juce::String g_ortVersion;
static int          g_cudaDllsFound = 0;   // CUDA/cuDNN DLLs present beside the plugin (of 13)

// ---------------------------------------------------------------------------
// Load onnxruntime.dll explicitly from THIS plugin's own folder (so DirectML.dll
// next to it resolves regardless of how the host searches), then initialise the
// ONNX Runtime C++ API from the loaded module. Safe to call repeatedly.
// ---------------------------------------------------------------------------
static bool ensureOrtLoaded()
{
#if JUCE_WINDOWS
    static bool tried = false;
    if (tried) return g_ortReady;
    tried = true;

    HMODULE self = nullptr;
    if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR> (&ensureOrtLoaded), &self))
        return false;

    wchar_t buf[MAX_PATH] = {};
    if (! GetModuleFileNameW (self, buf, MAX_PATH))
        return false;

    const juce::String selfPath (buf);
    const juce::File here { selfPath };
    const juce::File dll = here.getParentDirectory().getChildFile ("onnxruntime.dll");

    // LOAD_WITH_ALTERED_SEARCH_PATH => onnxruntime.dll's own folder is searched
    // first for ITS dependencies (DirectML.dll, onnxruntime_providers_shared.dll).
    HMODULE h = LoadLibraryExW (dll.getFullPathName().toWideCharPointer(),
                                nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h == nullptr)
        return false;

    // GPU groundwork, GATED OFF while kEnableCuda==false (the plugin is CPU-only at
    // runtime). Cold-loading this ~2.6 GB CUDA/cuDNN pack on the construct path stalled
    // plugin construction indefinitely -- pluginval "Open plugin (cold)" timed out at
    // 30s and the stem-engine-check log never got written, because DllMain CUDA init on
    // several of these libs blocks/faults inside a host process (the same host-process
    // integration fault that keeps CUDA off; see kEnableCuda note above). Nothing on the
    // CPU path consumes these: the CUDA EP is only appended, and g_cudaSetDevice only
    // called, under the same kEnableCuda guard in makeSession(). The CPU ORT load
    // (onnxruntime.dll + InitApi below) is fully independent of this block. When
    // kEnableCuda flips true to resume GPU work, this preload returns with it.
    // (Its eventual proper home is makeSession(), just before the CUDA EP append.)
    if (kEnableCuda)
    {
        // Pre-load the CUDA/cuDNN runtime from the plugin folder so it's resident BY
        // NAME before ONNX Runtime loads onnxruntime_providers_cuda.dll. That provider
        // DLL statically imports cudart/cublas/cublasLt/cufft/cudnn, which Windows would
        // otherwise resolve against the HOST's search path (not the plugin folder) ->
        // "Failed to load shared library". Loading each here with the altered search
        // path resolves its own deps from the same folder; absent files (CPU-only
        // installs) are silently skipped. Order: leaf deps -> cudnn -> the EP itself.
        {
            const juce::File pluginDir = dll.getParentDirectory();
            // CUDA 11.8 + cuDNN 8.9 runtime set for ORT 1.18 (leaf deps first, then cudnn,
            // then the EP). cuDNN 8 splits into *_infer sublibs (inference only needs those).
            static const char* kCudaDlls[] = {
                "cudart64_110.dll", "cublasLt64_11.dll", "cublas64_11.dll",
                "cufft64_10.dll", "curand64_10.dll", "nvrtc64_112_0.dll",
                "cudnn_ops_infer64_8.dll", "cudnn_cnn_infer64_8.dll", "cudnn_adv_infer64_8.dll",
                "cudnn64_8.dll", "onnxruntime_providers_cuda.dll"
            };
            for (auto* name : kCudaDlls)
            {
                const juce::File f = pluginDir.getChildFile (name);
                if (f.existsAsFile())
                {
                    LoadLibraryExW (f.getFullPathName().toWideCharPointer(),
                                    nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);  // best-effort
                    ++g_cudaDllsFound;
                }
            }
        }

        // Resolve cudaSetDevice so makeSession can bind the worker thread to GPU 0.
        if (HMODULE cudart = GetModuleHandleW (L"cudart64_110.dll"))
            g_cudaSetDevice = reinterpret_cast<CudaSetDeviceFn> (GetProcAddress (cudart, "cudaSetDevice"));
    }

    using GetApiBaseFn = const OrtApiBase* (ORT_API_CALL*) ();
    auto getApiBase = reinterpret_cast<GetApiBaseFn> (GetProcAddress (h, "OrtGetApiBase"));
    if (getApiBase == nullptr)
        return false;

    const OrtApiBase* base = getApiBase();
    if (base == nullptr)
        return false;
    if (const char* v = base->GetVersionString())
        g_ortVersion = juce::String (v);

    const OrtApi* api = base->GetApi (ORT_API_VERSION);
    if (api == nullptr)
        return false;
    Ort::InitApi (api);   // <-- all subsequent Ort:: C++ calls use this table

    // Resolve EP entry points present in this binary. A null pointer just means
    // that provider isn't compiled in -> the chain skips it. The Gpu.Windows ORT
    // build carries CUDA; a DirectML build would carry DML instead (future insert).
    g_cudaAppend = reinterpret_cast<CudaAppendFn> (
        GetProcAddress (h, "OrtSessionOptionsAppendExecutionProvider_CUDA"));
    g_dmlAppend = reinterpret_cast<DmlAppendFn> (
        GetProcAddress (h, "OrtSessionOptionsAppendExecutionProvider_DML"));

    g_ortReady = true;
    return true;
#else
    return true;
#endif
}

// Public wrapper so other ORT consumers (the Basic Pitch transcriber) can trigger the
// one-time manual DLL-load + Ort::InitApi without duplicating it. Ort::Global::api_ is a
// single process-wide table, so once this runs any TU's Ort:: calls work.
bool gentEnsureOrtLoaded() { return ensureOrtLoaded(); }

// ---------------------------------------------------------------------------
// Stage-1 build/link proof (see header). Loads ONNX Runtime from the bundle and
// reports its version. Never crashes the host: if the DLL can't be loaded, it
// logs that and returns without calling any ONNX Runtime function.
// ---------------------------------------------------------------------------
juce::String gentCheckStemEngine (const juce::File& logFile, const juce::File& modelsDir)
{
    juce::String out;
    out << "GentSampler stem-engine check\n";
    out << "built: " << __DATE__ << " " << __TIME__ << "\n";

    if (! ensureOrtLoaded())
    {
        out << "ONNX Runtime: could NOT load onnxruntime.dll from the plugin folder\n";
        logFile.replaceWithText (out);
        return out;
    }

    out << "ONNX Runtime: " << (g_ortVersion.isNotEmpty() ? g_ortVersion : juce::String ("(loaded)"))
        << "   (loaded OK)\n";
    out << "CUDA EP: "     << (g_cudaAppend != nullptr ? "available\n" : "not available\n");
    out << "CUDA runtime DLLs beside plugin: " << g_cudaDllsFound << "/11\n";
    out << "DirectML EP: " << (g_dmlAppend  != nullptr ? "available\n" : "not available\n");

    const bool present = modelsDir.getChildFile ("manifest.json").existsAsFile();
    out << "models folder: " << modelsDir.getFullPathName()
        << (present ? "   [manifest.json found]\n" : "   [not found yet -- ok for Stage 1]\n");

    logFile.replaceWithText (out);
    return out;
}


// -----------------------------------------------------------------------------
// small planar stem container: data laid out [source][channel][sample]
// -----------------------------------------------------------------------------
namespace
{
    struct Stems
    {
        int numSources  = 0;
        int numChannels = 0;
        int numSamples  = 0;
        std::vector<float> d;

        void resize (int s, int c, int n)
        {
            numSources = s; numChannels = c; numSamples = n;
            d.assign ((size_t) s * c * n, 0.0f);
        }
        inline float& at (int s, int c, int n) noexcept
        {
            return d[(((size_t) s * numChannels + c) * numSamples) + n];
        }
        inline const float& at (int s, int c, int n) const noexcept
        {
            return d[(((size_t) s * numChannels + c) * numSamples) + n];
        }
    };
}

// -----------------------------------------------------------------------------
// Impl: holds ORT env/sessions + per-model metadata
// -----------------------------------------------------------------------------
struct StemSeparator::Impl
{
    struct ModelInfo
    {
        juce::String name;
        juce::StringArray sources;
        // bagWeights[submodel][source]; empty => weight 1.0 everywhere
        std::vector<std::vector<float>> bagWeights;
        juce::StringArray onnxFiles;     // absolute paths; sessions created on demand
    };

    // Created lazily in initialise() AFTER ensureOrtLoaded(), so merely
    // constructing a StemSeparator never touches ONNX Runtime.
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::MemoryInfo> memInfo;
    std::map<juce::String, ModelInfo> models;  // "htdemucs_6s", "htdemucs_ft"
    bool useGpu    = false;
    bool gpuProviderResolved = false;   // a GPU EP symbol (CUDA/...) exists in this binary
    bool gpuActive = false;             // a GPU provider actually built the live session
    bool deviceHung = false;            // a GPU run failed and we fell back to CPU
    juce::String gpuError;              // human-readable reason GPU failed (diagnostics)
    juce::String selectedProvider = "CPU";   // "CUDA" | "CPU" | future ("CoreML"/"DirectML"/"WebGPU")
    int segmentSamples = StemSeparator::kSegmentSamples;  // per-model fixed export length, from manifest

    // parse manifest metadata only (no sessions => no ONNX Runtime memory yet)
    bool parseManifest (const juce::File& dir, const juce::var& manifest, juce::String& errorOut);

    // create ONE session for a model file (DirectML if useGpu && !forceCpu, else CPU).
    // Caller owns it and should release it before loading the next model so only one
    // model is resident on the GPU at a time (avoids 6 GB-card device hangs).
    std::unique_ptr<Ort::Session> makeSession (const juce::File& path, juce::String& errorOut,
                                               bool forceCpu = false);

    // run one .onnx session over the whole (normalized) mix with centered
    // segmenting + center-trim + triangular overlap-add  (== run_onnx.run_one_model)
    Stems runOneModel (Ort::Session& sess, const float* mix, int C, int N,
                       float overlap, int chunkBase, int chunkTotal,
                       const juce::String& label, const ProgressFn& progress);

    // load one .onnx file and run it, automatically rebuilding on CPU if a DirectML
    // session/inference fails (e.g. DXGI_ERROR_DEVICE_HUNG 0x887A0006). Never throws;
    // returns empty Stems + errorOut on hard failure.
    Stems runModelFile (const juce::File& onnxFile, const float* mix, int C, int N,
                        float overlap, int chunkBase, int chunkTotal,
                        const juce::String& label, const ProgressFn& progress,
                        juce::String& errorOut);

    // run all submodels of a bag (one resident at a time) and combine by weights
    Stems runBag (ModelInfo& mi, const float* mix, int C, int N, float overlap,
                  Mode /*mode*/, int& chunkBase, int chunkTotal,
                  const ProgressFn& progress);
};

// -----------------------------------------------------------------------------
// session creation via an ordered EP provider-priority chain (one model resident
// at a time). v1: CUDA -> CPU. Each entry probes its provider, builds a FRESH
// SessionOptions, and tries to construct the Session; the first that succeeds
// wins and records selectedProvider/gpuActive. Future providers (CoreML, WebGPU,
// DirectML-with-mitigations) slot in at the marked point, in priority order.
// -----------------------------------------------------------------------------
std::unique_ptr<Ort::Session> StemSeparator::Impl::makeSession (const juce::File& path, juce::String& errorOut,
                                                               bool forceCpu)
{
   #if JUCE_WINDOWS
    const std::wstring wpath = path.getFullPathName().toWideCharPointer();
   #else
    const juce::String p8 = path.getFullPathName();
   #endif

    const bool wantGpu = useGpu && ! forceCpu;

    // Bind THIS (worker) thread to GPU 0 before ORT creates the CUDA session, so the
    // EP's context/stream are created on the thread that will also run inference.
    if (kEnableCuda && wantGpu && g_cudaAppend != nullptr && g_cudaSetDevice != nullptr)
        g_cudaSetDevice (0);

    struct Attempt { const char* name; std::function<void (Ort::SessionOptions&)> configure; };
    std::vector<Attempt> chain;

    if (kEnableCuda && wantGpu && g_cudaAppend != nullptr)
        chain.push_back ({ "CUDA", [] (Ort::SessionOptions& o)
        {
            // Default CUDA EP options. The legacy ORT 1.18 / cuDNN-8 path runs the full
            // 7.8s htdemucs model cleanly (verified on the RTX 2060), so none of the
            // cuDNN-9-era workarounds are needed. (g_cudaAppend's presence is just our
            // probe that the CUDA EP is compiled into this binary.)
            const OrtApi& api = Ort::GetApi();
            OrtCUDAProviderOptionsV2* copts = nullptr;
            Ort::ThrowOnError (api.CreateCUDAProviderOptions (&copts));
            OrtStatus* st = api.SessionOptionsAppendExecutionProvider_CUDA_V2 (
                                static_cast<OrtSessionOptions*> (o), copts);
            api.ReleaseCUDAProviderOptions (copts);
            Ort::ThrowOnError (st);   // non-null status -> append failed, chain falls through
        } });

    // ---- FUTURE provider inserts (priority order, BEFORE the CPU terminal) ----
    //   macOS CoreML:  #if JUCE_MAC  if (wantGpu && g_coremlAppend) chain.push_back ({ "CoreML", ... });
    //   WebGPU:        if (wantGpu && g_webgpuAppend) chain.push_back ({ "WebGPU", ... });
    //   DirectML:      if (wantGpu && g_dmlAppend) chain.push_back ({ "DirectML", [] (auto& o) {
    //                      o.SetExecutionMode (ExecutionMode::ORT_SEQUENTIAL); o.DisableMemPattern();  // DML-only quirks
    //                      Ort::ThrowOnError (g_dmlAppend ((OrtSessionOptions*) o, 0)); } });
    // Keep each provider's session quirks INSIDE its own lambda, never global.

    chain.push_back ({ "CPU", [] (Ort::SessionOptions& o)
        { o.SetIntraOpNumThreads ((int) juce::SystemStats::getNumCpus()); } });

    for (auto& a : chain)
    {
        try
        {
            Ort::SessionOptions opts;
            opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
            a.configure (opts);                         // may throw if the EP can't attach

           #if JUCE_WINDOWS
            auto sess = std::make_unique<Ort::Session> (*env, wpath.c_str(), opts);
           #else
            auto sess = std::make_unique<Ort::Session> (*env, p8.toRawUTF8(), opts);
           #endif

            selectedProvider = a.name;
            gpuActive = (juce::String (a.name) != "CPU");
            return sess;
        }
        catch (const Ort::Exception& e)
        {
            const juce::String msg = juce::String (a.name) + " unavailable (code "
                                     + juce::String ((int) e.GetOrtErrorCode()) + "): " + e.what();
            DBG (msg);
            if (juce::String (a.name) != "CPU")
                gpuError = msg;          // remember why the GPU provider was skipped
            // fall through to the next provider in the chain
        }
    }

    errorOut = "failed to load " + path.getFileName() + " on any provider";
    gpuActive = false;
    selectedProvider = "none";
    return nullptr;
}

// -----------------------------------------------------------------------------
// parse manifest (metadata + file paths only)
// -----------------------------------------------------------------------------
bool StemSeparator::Impl::parseManifest (const juce::File& dir, const juce::var& manifest, juce::String& errorOut)
{
    const juce::var modelsVar = manifest.getProperty ("models", juce::var());
    if (! modelsVar.isArray()) { errorOut = "manifest.json has no 'models' array"; return false; }

    for (auto& mv : *modelsVar.getArray())
    {
        ModelInfo mi;
        mi.name = mv.getProperty ("name", "").toString();

        // Segment length this model was exported at (its fixed ONNX input width).
        // Prefer an explicit sample count; else seconds*samplerate; else the default.
        {
            const int sr   = (int) mv.getProperty ("samplerate", (int) StemSeparator::kModelSampleRate);
            const int segS  = (int) mv.getProperty ("segment_samples", 0);
            const double sec = (double) mv.getProperty ("segment", 0.0);
            if (segS > 0)        segmentSamples = segS;
            else if (sec > 0.0)  segmentSamples = (int) std::lround (sec * sr);
        }

        if (auto* srcs = mv.getProperty ("sources", juce::var()).getArray())
            for (auto& s : *srcs) mi.sources.add (s.toString());

        if (auto* bw = mv.getProperty ("bag_weights", juce::var()).getArray())
            for (auto& row : *bw)
            {
                std::vector<float> r;
                if (auto* ra = row.getArray())
                    for (auto& v : *ra) r.push_back ((float) (double) v);
                mi.bagWeights.push_back (std::move (r));
            }

        auto* files = mv.getProperty ("onnx_files", juce::var()).getArray();
        if (files == nullptr) { errorOut = "model '" + mi.name + "' has no onnx_files"; return false; }

        for (auto& f : *files)
        {
            const juce::File path = dir.getChildFile (f.toString());
            if (! path.existsAsFile())
            {
                errorOut = "missing ONNX file: " + path.getFullPathName();
                return false;
            }
            mi.onnxFiles.add (path.getFullPathName());
        }

        models[mi.name] = std::move (mi);
    }
    return true;
}

// -----------------------------------------------------------------------------
// run_one_model  (centered window + center_trim + triangular overlap-add)
// -----------------------------------------------------------------------------
Stems StemSeparator::Impl::runOneModel (Ort::Session& sess, const float* mix, int C, int N,
                                        float overlap, int chunkBase, int chunkTotal,
                                        const juce::String& label, const ProgressFn& progress)
{
    const int SEG = segmentSamples;
    const int stride = juce::jmax (1, (int) ((1.0f - overlap) * (float) SEG));

    // triangular weight, peak-normalized (== np triangular weight)
    std::vector<float> w ((size_t) SEG);
    {
        const int half = SEG / 2;
        for (int i = 0; i < half; ++i)              w[(size_t) i] = (float) (i + 1);
        for (int i = 0; i < SEG - half; ++i)        w[(size_t) (half + i)] = (float) (SEG - half - i);
        float mx = 0.0f; for (float v : w) mx = juce::jmax (mx, v);
        for (auto& v : w) v /= mx;
    }

    // discover S from a probe run shape after first chunk; allocate then.
    Stems out;
    std::vector<float> sw ((size_t) N, 0.0f);

    std::vector<float> chunk ((size_t) C * SEG);
    const char* inNames[]  = { "mix" };
    const char* outNames[] = { "stems" };

    int chunkIdx = 0;
    int totalChunksThisModel = 0;
    for (int off = 0; off < N; off += stride) ++totalChunksThisModel;

    for (int off = 0; off < N; off += stride)
    {
        const int length = juce::jmin (SEG, N - off);
        const int delta  = SEG - length;
        const int start  = off - delta / 2;
        const int end    = start + SEG;
        const int cs     = juce::jmax (0, start);
        const int ce     = juce::jmin (N, end);
        const int pl     = cs - start;               // left zero-pad

        std::fill (chunk.begin(), chunk.end(), 0.0f);
        const int realLen = ce - cs;
        for (int c = 0; c < C; ++c)
            std::memcpy (&chunk[(size_t) c * SEG + pl],
                         &mix[(size_t) c * N + cs],
                         sizeof (float) * (size_t) realLen);

        std::array<int64_t, 3> inShape { 1, (int64_t) C, (int64_t) SEG };
        Ort::Value inT = Ort::Value::CreateTensor<float> (*memInfo, chunk.data(), chunk.size(),
                                                          inShape.data(), inShape.size());
        auto outputs = sess.Run (Ort::RunOptions { nullptr }, inNames, &inT, 1, outNames, 1);

        const float* y = outputs[0].GetTensorData<float>();
        const auto shp = outputs[0].GetTensorTypeAndShapeInfo().GetShape(); // {1,S,C,SEG}
        const int S = (int) shp[1];

        if (out.numSources == 0)
            out.resize (S, C, N);

        const int lo = delta / 2;                    // center_trim offset
        for (int s = 0; s < S; ++s)
            for (int c = 0; c < C; ++c)
            {
                const float* ySrc = y + (((size_t) s * C + c) * SEG) + lo;
                float* oDst = &out.at (s, c, off);
                for (int t = 0; t < length; ++t)
                    oDst[t] += w[(size_t) t] * ySrc[t];
            }
        for (int t = 0; t < length; ++t)
            sw[(size_t) (off + t)] += w[(size_t) t];

        if (progress)
        {
            Progress p;
            p.chunksDone  = chunkBase + (++chunkIdx);
            p.chunksTotal = chunkTotal;
            p.fraction    = chunkTotal > 0 ? (float) p.chunksDone / (float) chunkTotal : 0.0f;
            p.label       = label;
            progress (p);
        }
        juce::ignoreUnused (totalChunksThisModel);
    }

    // divide by summed weights
    for (int s = 0; s < out.numSources; ++s)
        for (int c = 0; c < out.numChannels; ++c)
            for (int n = 0; n < N; ++n)
                out.at (s, c, n) /= juce::jmax (1e-8f, sw[(size_t) n]);

    return out;
}

// -----------------------------------------------------------------------------
// run one model file with automatic GPU -> CPU fallback on any provider failure
// (session creation OR inference). If a GPU dispatch fails mid-run (e.g. CUDA OOM
// on a 6 GB card, or a DXGI device hang on a future DirectML path), rather than
// take the host down we rebuild on CPU and re-run that model from scratch.
// -----------------------------------------------------------------------------
Stems StemSeparator::Impl::runModelFile (const juce::File& onnxFile, const float* mix, int C, int N,
                                         float overlap, int chunkBase, int chunkTotal,
                                         const juce::String& label, const ProgressFn& progress,
                                         juce::String& errorOut)
{
    for (int attempt = 0; attempt < 2; ++attempt)   // 0 = preferred (GPU if enabled), 1 = forced CPU
    {
        const bool forceCpu = (attempt == 1);
        juce::String err;
        auto sess = makeSession (onnxFile, err, forceCpu);

        if (sess == nullptr)
        {
            if (! forceCpu && useGpu)                 // GPU session failed -> try CPU
            {
                deviceHung = true; useGpu = false; gpuActive = false;
                DBG ("GPU session failed (" << err << "); falling back to CPU");
                continue;
            }
            errorOut = err;
            return {};
        }

        try
        {
            Stems s = runOneModel (*sess, mix, C, N, overlap, chunkBase, chunkTotal, label, progress);
            sess.reset();                             // free this model before the next
            return s;
        }
        catch (const Ort::Exception& e)
        {
            sess.reset();
            if (! forceCpu && (useGpu || gpuActive))  // device hang / EP failure -> CPU
            {
                gpuError = juce::String (selectedProvider) + " inference failed (code "
                           + juce::String ((int) e.GetOrtErrorCode()) + "): " + e.what();
                deviceHung = true; useGpu = false; gpuActive = false;
                DBG (gpuError);
                continue;
            }
            errorOut = juce::String ("inference failed: ") + e.what();
            return {};
        }
    }

    errorOut = "model run failed";
    return {};
}

// -----------------------------------------------------------------------------
// run_bag  (combine submodel outputs per source by bagWeights)
// -----------------------------------------------------------------------------
Stems StemSeparator::Impl::runBag (ModelInfo& mi, const float* mix, int C, int N, float overlap,
                                   Mode, int& chunkBase, int chunkTotal, const ProgressFn& progress)
{
    Stems totals;
    std::vector<float> wsum;

    const int nSub = mi.onnxFiles.size();
    int chunksPerModel = 0;
    { const int stride = juce::jmax (1, (int) ((1.0f - overlap) * (float) segmentSamples));
      for (int off = 0; off < N; off += stride) ++chunksPerModel; }

    for (int i = 0; i < nSub; ++i)
    {
        const juce::String label = mi.name + " (" + juce::String (i + 1) + "/" + juce::String (nSub) + ")";
        juce::String err;
        Stems est = runModelFile (juce::File (mi.onnxFiles[i]), mix, C, N, overlap,
                                  chunkBase, chunkTotal, label, progress, err);
        chunkBase += chunksPerModel;
        if (est.numSources == 0)
        {
            DBG ("runBag: " << err);
            continue;
        }

        if (totals.numSources == 0)
        {
            totals.resize (est.numSources, C, N);
            wsum.assign ((size_t) est.numSources, 0.0f);
        }

        for (int s = 0; s < est.numSources; ++s)
        {
            float wv = 1.0f;
            if (i < (int) mi.bagWeights.size() && s < (int) mi.bagWeights[(size_t) i].size())
                wv = mi.bagWeights[(size_t) i][(size_t) s];
            wsum[(size_t) s] += wv;
            for (int c = 0; c < C; ++c)
                for (int n = 0; n < N; ++n)
                    totals.at (s, c, n) += wv * est.at (s, c, n);
        }
    }

    for (int s = 0; s < totals.numSources; ++s)
    {
        const float inv = 1.0f / juce::jmax (1e-8f, wsum[(size_t) s]);
        for (int c = 0; c < C; ++c)
            for (int n = 0; n < N; ++n)
                totals.at (s, c, n) *= inv;
    }
    return totals;
}

// =============================================================================
// public class
// =============================================================================
StemSeparator::StemSeparator() : impl (std::make_unique<Impl>()) {}
StemSeparator::~StemSeparator() = default;

const juce::StringArray& StemSeparator::sixStemOrder()
{
    static const juce::StringArray order { "drums", "bass", "vocals", "guitar", "piano", "other" };
    return order;
}

bool StemSeparator::initialise (const juce::File& modelsDir, bool useGPU, juce::String& errorOut)
{
    initialised = false; gpuActive = false;

    if (! ensureOrtLoaded())
    {
        errorOut = "could not load onnxruntime.dll from the plugin folder";
        return false;
    }

    // Now that the API is initialised, build the env + memory info.
    if (impl->env == nullptr)
        impl->env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "GentStems");
    if (impl->memInfo == nullptr)
        impl->memInfo = std::make_unique<Ort::MemoryInfo> (
            Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault));

    const juce::File mf = modelsDir.getChildFile ("manifest.json");
    if (! mf.existsAsFile()) { errorOut = "manifest.json not found in " + modelsDir.getFullPathName(); return false; }

    juce::var manifest = juce::JSON::parse (mf.loadFileAsString());
    if (! manifest.isObject()) { errorOut = "could not parse manifest.json"; return false; }

    impl->useGpu = useGPU;
    impl->deviceHung = false;
    impl->gpuError.clear();
    impl->selectedProvider = "CPU";
    impl->models.clear();
    if (! impl->parseManifest (modelsDir, manifest, errorOut))
        return false;

    // A GPU EP symbol present in this binary (CUDA today; CoreML/etc. later).
    // Gated by kEnableCuda so the GPU path stays fully off until the in-host crash is fixed.
    impl->gpuProviderResolved = kEnableCuda && (g_cudaAppend != nullptr);

    // sessions are created one-at-a-time in separate(); this is the OPTIMISTIC
    // pre-run signal (real provider is set when the first session is built).
    impl->gpuActive = (useGPU && impl->gpuProviderResolved);
    gpuActive = impl->gpuActive;

    initialised = true;
    return true;
}

bool StemSeparator::usingGPU() const noexcept { return impl->gpuActive; }
bool StemSeparator::deviceFellBackToCpu() const noexcept { return impl->deviceHung; }
juce::String StemSeparator::gpuErrorMessage() const { return impl->gpuError; }
juce::String StemSeparator::selectedProvider() const { return impl->selectedProvider; }

// htdemucs_ft "max quality" is 4x the work -> only offer it when a GPU provider is
// plausibly usable AND the ft bag is actually present in the loaded models.
bool StemSeparator::canUseMaxQuality() const
{
    return impl->useGpu && impl->gpuProviderResolved
           && impl->models.find ("htdemucs_ft") != impl->models.end();
}

// --- helpers -----------------------------------------------------------------
namespace
{
    // make a (2, N) planar float vector from any buffer, resampling to 44100.
    std::vector<float> toStereo44k (const juce::AudioBuffer<float>& in, double inSR, int& outN)
    {
        const int inCh = in.getNumChannels();
        const int inN  = in.getNumSamples();

        // collapse to stereo source channels
        auto getCh = [&] (int c) -> const float* { return in.getReadPointer (juce::jmin (c, inCh - 1)); };
        const float* L = getCh (0);
        const float* R = inCh > 1 ? getCh (1) : getCh (0);

        if (std::abs (inSR - StemSeparator::kModelSampleRate) < 1.0)
        {
            outN = inN;
            std::vector<float> out ((size_t) 2 * inN);
            std::memcpy (&out[0],            L, sizeof (float) * (size_t) inN);
            std::memcpy (&out[(size_t) inN], R, sizeof (float) * (size_t) inN);
            return out;
        }

        const double ratio = inSR / (double) StemSeparator::kModelSampleRate; // speedRatio for Lagrange
        outN = (int) std::ceil (inN / ratio);
        std::vector<float> out ((size_t) 2 * outN, 0.0f);

        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process (ratio, L, &out[0],            outN);
        interp.reset();
        interp.process (ratio, R, &out[(size_t) outN], outN);
        return out; // NOTE: linear-phase Lagrange; fine for source separation input
    }
}

std::map<juce::String, juce::AudioBuffer<float>>
StemSeparator::separate (const juce::AudioBuffer<float>& input, double inputSampleRate,
                         Mode mode, float overlap, ProgressFn progress, juce::String& errorOut)
{
    std::map<juce::String, juce::AudioBuffer<float>> result;
    if (! initialised) { errorOut = "StemSeparator not initialised"; return result; }

    // ---- (1) make stereo @ 44100 ----
    int N = 0;
    std::vector<float> mix = toStereo44k (input, inputSampleRate, N);
    const int C = 2;
    if (N <= 0) { errorOut = "empty input"; return result; }

    // ---- (2) global normalization (mean/std over all samples & channels) ----
    double sum = 0.0, sumsq = 0.0;
    const size_t total = (size_t) C * N;
    for (size_t i = 0; i < total; ++i) { sum += mix[i]; sumsq += (double) mix[i] * mix[i]; }
    const float refMean = (float) (sum / (double) total);
    const float var     = (float) (sumsq / (double) total) - refMean * refMean;
    const float refStd  = std::sqrt (juce::jmax (0.0f, var)) + 1e-8f;
    for (size_t i = 0; i < total; ++i) mix[i] = (mix[i] - refMean) / refStd;

    // ---- model/quality gating ----
    // htdemucs_ft (Hybrid/FourStem) is the GPU-only "max quality" path: 4 submodels,
    // ~4x the work, unusable on CPU (~10+ min). If a GPU provider isn't plausibly
    // available, downgrade to the single htdemucs_6s pass (standard, all 6 stems)
    // rather than grinding on CPU.
    // Hybrid/FourStem need the htdemucs_ft sub-models. They run fine on CPU (just
    // slower — ~5 passes); only downgrade if the ft model isn't actually present.
    if ((mode == Mode::Hybrid || mode == Mode::FourStem)
        && impl->models.find ("htdemucs_ft") == impl->models.end())
    {
        impl->gpuError = "htdemucs_ft model not available; used standard htdemucs_6s";
        mode = Mode::SixStem;
    }

    // ---- chunk accounting for progress ----
    const int stride = juce::jmax (1, (int) ((1.0f - overlap) * (float) impl->segmentSamples));
    int chunksPerModel = 0; for (int off = 0; off < N; off += stride) ++chunksPerModel;
    const bool needFt = (mode == Mode::Hybrid || mode == Mode::FourStem);
    const bool need6s = (mode == Mode::Hybrid || mode == Mode::SixStem);

    auto findModel = [&] (const juce::String& nm) -> Impl::ModelInfo*
    {
        auto it = impl->models.find (nm);
        return it == impl->models.end() ? nullptr : &it->second;
    };

    Impl::ModelInfo* ft = needFt ? findModel ("htdemucs_ft") : nullptr;
    Impl::ModelInfo* s6 = need6s ? findModel ("htdemucs_6s") : nullptr;
    if (needFt && ft == nullptr) { errorOut = "htdemucs_ft not loaded"; return result; }
    if (need6s && s6 == nullptr) { errorOut = "htdemucs_6s not loaded"; return result; }

    int chunkTotal = 0;
    if (needFt) chunkTotal += chunksPerModel * ft->onnxFiles.size();
    if (need6s) chunkTotal += chunksPerModel * s6->onnxFiles.size();
    int chunkBase = 0;

    // ---- (3) run models (normalized domain), one model resident at a time ----
    Stems ftOut, s6Out;
    if (needFt)
    {
        ftOut = impl->runBag (*ft, mix.data(), C, N, overlap, mode, chunkBase, chunkTotal, progress);
        if (ftOut.numSources == 0)   // all ft submodels failed -> bail cleanly (don't read empty buffers)
        { errorOut = "htdemucs_ft failed (separation produced no output)"; return result; }
    }
    if (need6s)
    {
        juce::String err6;
        s6Out = impl->runModelFile (juce::File (s6->onnxFiles[0]), mix.data(), C, N, overlap,
                                    chunkBase, chunkTotal, "htdemucs_6s (1/1)", progress, err6);
        chunkBase += chunksPerModel;
        if (s6Out.numSources == 0) { errorOut = "htdemucs_6s: " + err6; return result; }
    }

    // index helper: source name -> index within a model's source list
    auto idx = [] (const Impl::ModelInfo* mi, const char* nm) -> int
    {
        return mi ? mi->sources.indexOf (nm) : -1;
    };

    // ---- (4) assemble final stems (still normalized) ----
    auto makeBuffer = [&] (const Stems& st, int s) -> juce::AudioBuffer<float>
    {
        juce::AudioBuffer<float> b (C, N);
        for (int c = 0; c < C; ++c)
            std::memcpy (b.getWritePointer (c), &st.at (s, c, 0), sizeof (float) * (size_t) N);
        return b;
    };

    std::map<juce::String, juce::AudioBuffer<float>> finalN; // normalized

    if (mode == Mode::Hybrid)
    {
        const int iD = idx (ft, "drums"),  iB = idx (ft, "bass"),
                  iV = idx (ft, "vocals"), iO = idx (ft, "other");
        const int gG = idx (s6, "guitar"), gP = idx (s6, "piano");
        if (juce::jmin (iD, iB, iV, iO) < 0 || juce::jmin (gG, gP) < 0)
        { errorOut = "source name mismatch in manifest"; return result; }

        finalN["drums"]  = makeBuffer (ftOut, iD);
        finalN["bass"]   = makeBuffer (ftOut, iB);
        finalN["vocals"] = makeBuffer (ftOut, iV);
        finalN["guitar"] = makeBuffer (s6Out, gG);
        finalN["piano"]  = makeBuffer (s6Out, gP);

        // other = ft.other - 6s.guitar - 6s.piano   (normalized domain)
        juce::AudioBuffer<float> other (C, N);
        for (int c = 0; c < C; ++c)
        {
            float* o = other.getWritePointer (c);
            const float* fo = &ftOut.at (iO, c, 0);
            const float* g  = &s6Out.at (gG, c, 0);
            const float* p  = &s6Out.at (gP, c, 0);
            for (int n = 0; n < N; ++n) o[n] = fo[n] - g[n] - p[n];
        }
        finalN["other"] = std::move (other);
    }
    else if (mode == Mode::FourStem)
    {
        for (int s = 0; s < ftOut.numSources; ++s)
            finalN[ft->sources[s]] = makeBuffer (ftOut, s);
    }
    else // SixStem
    {
        for (int s = 0; s < s6Out.numSources; ++s)
            finalN[s6->sources[s]] = makeBuffer (s6Out, s);
    }

    // ---- (5) un-normalize once ----
    for (auto& kv : finalN)
    {
        auto& b = kv.second;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            float* d = b.getWritePointer (c);
            for (int n = 0; n < b.getNumSamples(); ++n) d[n] = d[n] * refStd + refMean;
        }
        result[kv.first] = std::move (b);
    }

    return result;
}
