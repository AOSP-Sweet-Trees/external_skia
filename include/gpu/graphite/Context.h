/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef skgpu_graphite_Context_DEFINED
#define skgpu_graphite_Context_DEFINED

#include <vector>
#include "include/core/SkBlendMode.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/private/SkNoncopyable.h"
#include "include/private/SkTHash.h"
#include "src/core/SkArenaAlloc.h"

class SkRuntimeEffect;
class SkShaderCodeDictionary;

namespace skgpu::graphite {

class BackendTexture;
class CommandBuffer;
class Context;
class ContextPriv;
class GlobalCache;
class Gpu;
struct MtlBackendContext;
class PaintCombinations;
class Recorder;
class Recording;
class TextureInfo;

enum class ShaderType {
    kSolidColor,

    kLinearGradient,
    kRadialGradient,
    kSweepGradient,
    kConicalGradient,

    kLocalMatrix,
    kImage,
    kBlendShader
};

struct ShaderCombo {
    ShaderCombo() {}
    ShaderCombo(std::vector<ShaderType> types,
                std::vector<SkTileMode> tileModes)
            : fTypes(std::move(types))
            , fTileModes(std::move(tileModes)) {
    }
    std::vector<ShaderType> fTypes;
    std::vector<SkTileMode> fTileModes;
};

// TODO: add SkShaderID and SkColorFilterID too
class SkBlenderID {
public:
    SkBlenderID() : fID(0) {}  // 0 is an invalid blender ID
    SkBlenderID(const SkBlenderID& src) : fID(src.fID) {}

    bool isValid() const { return fID > 0; }

    bool operator==(const SkBlenderID& other) const { return fID == other.fID; }

    SkBlenderID& operator=(const SkBlenderID& src) {
        fID = src.fID;
        return *this;
    }

private:
    friend class ::SkShaderCodeDictionary;   // for ctor and asUInt access
    friend class CombinationBuilder;         // for asUInt access
    friend class PaintCombinations;          // for asUInt access

    SkBlenderID(uint32_t id) : fID(id) {}

    uint32_t asUInt() const { return fID; }

    uint32_t fID;
};

class CombinationBuilder {
public:
    enum class BlendModeGroup {
        kPorterDuff,         // [ kClear .. kScreen ]
        kAdvanced,           // [ kOverlay .. kMultiply ]
        kColorAware,         // [ kHue .. kLuminosity ]
        kAll
    };

    CombinationBuilder(Context*);

    // Blend Modes
    void add(SkBlendMode);
    void add(SkBlendMode rangeStart, SkBlendMode rangeEnd); // inclusive
    void add(BlendModeGroup);
    void add(SkBlenderID);

    // Shaders
    void add(ShaderCombo);

    void reset();

private:
    friend class Context; // for access to fCombinations

    SkShaderCodeDictionary* fDictionary;
    SkArenaAllocWithReset fArena{64};
    PaintCombinations* fCombinations;
};

class Context final {
public:
    Context(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(const Context&) = delete;
    Context& operator=(Context&&) = delete;

    ~Context();

#ifdef SK_METAL
    static std::unique_ptr<Context> MakeMetal(const skgpu::graphite::MtlBackendContext&);
#endif

    BackendApi backend() const { return fBackend; }

    std::unique_ptr<Recorder> makeRecorder();

    void insertRecording(const InsertRecordingInfo&);
    void submit(SyncToCpu = SyncToCpu::kNo);

    /**
     * Checks whether any asynchronous work is complete and if so calls related callbacks.
     */
    void checkAsyncWorkCompletion();

    // TODO: add "SkShaderID addUserDefinedShader(sk_sp<SkRuntimeEffect>)" here
    // TODO: add "SkColorFilterID addUserDefinedColorFilter(sk_sp<SkRuntimeEffect>)" here
    SkBlenderID addUserDefinedBlender(sk_sp<SkRuntimeEffect>);

    void preCompile(const CombinationBuilder&);

    /**
     * Creates a new backend gpu texture matching the dimensinos and TextureInfo. If an invalid
     * TextureInfo or a TextureInfo Skia can't support is passed in, this will return an invalid
     * BackendTexture. Thus the client should check isValid on the returned BackendTexture to know
     * if it succeeded or not.
     *
     * If this does return a valid BackendTexture, the caller is required to use
     * Context::deleteBackendTexture to delete that texture.
     */
    BackendTexture createBackendTexture(SkISize dimensions, const TextureInfo&);

    /**
     * Called to delete the passed in BackendTexture. This should only be called if the
     * BackendTexture was created by calling Context::createBackendTexture. If the BackendTexture is
     * not valid or does not match the BackendApi of the Context then nothing happens.
     *
     * Otherwise this will delete/release the backend object that is wrapped in the BackendTexture.
     * The BackendTexture will be reset to an invalid state and should not be used again.
     */
    void deleteBackendTexture(BackendTexture&);

    // Provides access to functions that aren't part of the public API.
    ContextPriv priv();
    const ContextPriv priv() const;  // NOLINT(readability-const-return-type)

protected:
    Context(sk_sp<Gpu>, BackendApi);

private:
    friend class ContextPriv;

    sk_sp<CommandBuffer> fCurrentCommandBuffer;

    sk_sp<Gpu> fGpu;
    sk_sp<GlobalCache> fGlobalCache;
    BackendApi fBackend;
};

} // namespace skgpu::graphite

#endif // skgpu_graphite_Context_DEFINED
