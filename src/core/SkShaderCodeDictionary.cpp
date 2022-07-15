/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkShaderCodeDictionary.h"

#include "include/effects/SkRuntimeEffect.h"
#include "include/private/SkSLString.h"
#include "src/core/SkOpts.h"
#include "src/core/SkRuntimeEffectDictionary.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/sksl/SkSLUtil.h"
#include "src/sksl/codegen/SkSLPipelineStageCodeGenerator.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"

#ifdef SK_GRAPHITE_ENABLED
#include "include/gpu/graphite/Context.h"
#endif

#ifdef SK_ENABLE_PRECOMPILE
#include "include/core/SkCombinationBuilder.h"
#endif

using DataPayloadField = SkPaintParamsKey::DataPayloadField;
using DataPayloadType = SkPaintParamsKey::DataPayloadType;

namespace {

#if defined(SK_GRAPHITE_ENABLED) && defined(SK_METAL)
std::string get_mangled_name(const std::string& baseName, int manglingSuffix) {
    return baseName + "_" + std::to_string(manglingSuffix);
}

void add_indent(std::string* result, int indent) {
    result->append(4*indent, ' ');
}
#endif

} // anonymous namespace


std::string SkShaderSnippet::getMangledUniformName(int uniformIndex, int mangleId) const {
    std::string result;
    result = fUniforms[uniformIndex].name() + std::string("_") + std::to_string(mangleId);
    return result;
}

// TODO: SkShaderInfo::toSkSL needs to work outside of both just graphite and metal. To do
// so we'll need to switch over to using SkSL's uniform capabilities.
#if defined(SK_GRAPHITE_ENABLED) && defined(SK_METAL)

// TODO: switch this over to using SkSL's uniform system
namespace skgpu::graphite {
std::string GetMtlUniforms(int bufferID,
                           const char* name,
                           const std::vector<SkPaintParamsKey::BlockReader>&,
                           bool needsDev2Local);
std::string GetMtlTexturesAndSamplers(const std::vector<SkPaintParamsKey::BlockReader>&,
                                      int* binding);
} // namespace skgpu::graphite

// Returns an expression to calculate the pre-local matrix for a given entry.

static std::string pre_local_matrix_for_entry(const SkShaderInfo& shaderInfo,
                                              int entryIndex,
                                              const std::string& parentMatrix) {
    const SkPaintParamsKey::BlockReader& reader = shaderInfo.blockReader(entryIndex);
    if (!reader.entry()->needsLocalCoords()) {
        // Return the parent matrix as-is.
        return parentMatrix;
    }

    // The snippet requested local coordinates, so the pre-local matrix must be its first uniform.
    SkASSERT(reader.entry()->fUniforms.size() >= 1);
    SkASSERT(reader.entry()->fUniforms.front().type() == SkSLType::kFloat4x4);

    std::string localMatrixUniformName = reader.entry()->getMangledUniformName(0, entryIndex);
    return SkSL::String::printf("(%s * %s)", parentMatrix.c_str(), localMatrixUniformName.c_str());
}

// Emit the glue code needed to invoke a single static helper isolated within its own scope.
// Glue code will assign the resulting color into a variable `half4 outColor%d`, where the %d is
// filled in with 'entryIndex'.
// Glue code is allowed to emit children recursively, which leads to a nested structure like this:
//
//     half4 outColor1;  // output of shader
//     {
//         half4 outColor2;  // output of first child
//         {
//             outColor2 = sk_first_child_snippet(uniformA, uniformB);
//         }
//         half4 outColor3;  // output of second child
//         {
//             outColor3 = sk_second_child_snippet(uniformC, uniformD);
//         }
//
//         outColor1 = sk_shader_snippet(uniformE, outColor2, outColor3);
//     }

static std::string emit_glue_code_for_entry(const SkShaderInfo& shaderInfo,
                                            int* entryIndex,
                                            const std::string& priorStageOutputName,
                                            const std::string& parentPreLocalName,
                                            std::string* preamble,
                                            std::string* mainBody,
                                            int indent) {
    const SkPaintParamsKey::BlockReader& reader = shaderInfo.blockReader(*entryIndex);
    int curEntryIndex = *entryIndex;

    std::string scopeOutputVar = get_mangled_name("outColor", curEntryIndex);

    add_indent(mainBody, indent);
    SkSL::String::appendf(mainBody,
                          "half4 %s; // output of %s\n",
                          scopeOutputVar.c_str(),
                          reader.entry()->fName);
    add_indent(mainBody, indent);
    *mainBody += "{\n";

    std::string currentPreLocalName;
    if (reader.entry()->needsLocalCoords()) {
        currentPreLocalName = get_mangled_name("preLocal", curEntryIndex);
        std::string preLocalExpression = pre_local_matrix_for_entry(shaderInfo,
                                                                    curEntryIndex,
                                                                    parentPreLocalName);
        add_indent(mainBody, indent + 1);
        SkSL::String::appendf(mainBody,
                              "float4x4 %s = %s;\n",
                              currentPreLocalName.c_str(),
                              preLocalExpression.c_str());
    } else {
        // Inherit the parent matrix; reuse the same variable instead of introducing a new one.
        currentPreLocalName = parentPreLocalName;
    }

    std::string expr = (reader.entry()->fExpressionGenerator)(shaderInfo, entryIndex,
                                                              reader, priorStageOutputName,
                                                              currentPreLocalName, preamble);
    add_indent(mainBody, indent + 1);
    SkSL::String::appendf(mainBody, "%s = %s;\n", scopeOutputVar.c_str(), expr.c_str());

    add_indent(mainBody, indent);
    *mainBody += "}\n";

    return scopeOutputVar;
}

static std::vector<std::string> emit_child_glue_code(const SkShaderInfo& shaderInfo,
                                                     int* entryIndex,
                                                     const std::string& priorStageOutputName,
                                                     const std::string& currentPreLocalName,
                                                     std::string* preamble,
                                                     std::string* mainBody,
                                                     int indent) {
    const SkPaintParamsKey::BlockReader& reader = shaderInfo.blockReader(*entryIndex);
    const int numChildren = reader.numChildren();

    std::vector<std::string> childOutputVarNames;
    for (int j = 0; j < numChildren; ++j) {
        *entryIndex += 1;
        std::string childOutputVar = emit_glue_code_for_entry(shaderInfo, entryIndex,
                                                              priorStageOutputName,
                                                              currentPreLocalName,
                                                              preamble, mainBody, indent);
        childOutputVarNames.push_back(std::move(childOutputVar));
    }
    return childOutputVarNames;
}

// The current, incomplete, model for shader construction is:
//   - Static code snippets (which can have an arbitrary signature) live in the Graphite
//     pre-compiled module, which is located at `src/sksl/sksl_graphite_frag.sksl`.
//   - Glue code is generated in a `main` method which calls these static code snippets.
//     The glue code is responsible for:
//            1) gathering the correct (mangled) uniforms
//            2) passing the uniforms and any other parameters to the helper method
//   - The result of the final code snippet is then copied into "sk_FragColor".
//   Note: each entry's 'fStaticFunctionName' field is expected to match the name of a function
//   in the Graphite pre-compiled module.
std::string SkShaderInfo::toSkSL() const {
    std::string preamble = "layout(location = 0, index = 0) out half4 sk_FragColor;\n";

    // The uniforms are mangled by having their index in 'fEntries' as a suffix (i.e., "_%d")
    // TODO: replace hard-coded bufferID of 2 with the backend's paint uniform-buffer index.
    preamble += skgpu::graphite::GetMtlUniforms(/*bufferID=*/2, "FS", fBlockReaders,
                                                this->needsLocalCoords());
    int binding = 0;
    preamble += skgpu::graphite::GetMtlTexturesAndSamplers(fBlockReaders, &binding);

    std::string mainBody = "void main() {\n"
                           "    const float4x4 initialPreLocal = float4x4(1.0);\n";

    std::string parentPreLocal = "initialPreLocal";
    std::string lastOutputVar = "initialColor";

    // TODO: what is the correct initial color to feed in?
    add_indent(&mainBody, 1);
    SkSL::String::appendf(&mainBody, "    half4 %s = half4(0);", lastOutputVar.c_str());

    for (int entryIndex = 0; entryIndex < (int) fBlockReaders.size(); ++entryIndex) {
        lastOutputVar = emit_glue_code_for_entry(*this, &entryIndex, lastOutputVar, parentPreLocal,
                                                 &preamble, &mainBody, /*indent=*/1);
    }

    SkSL::String::appendf(&mainBody, "    sk_FragColor = %s;\n", lastOutputVar.c_str());
    mainBody += "}\n";

    return preamble + "\n" + mainBody;
}
#endif

SkShaderCodeDictionary::Entry* SkShaderCodeDictionary::makeEntry(
        const SkPaintParamsKey& key
#ifdef SK_GRAPHITE_ENABLED
        , const skgpu::BlendInfo& blendInfo
#endif
        ) {
    uint8_t* newKeyData = fArena.makeArray<uint8_t>(key.sizeInBytes());
    memcpy(newKeyData, key.data(), key.sizeInBytes());

    SkSpan<const uint8_t> newKeyAsSpan = SkSpan(newKeyData, key.sizeInBytes());
#ifdef SK_GRAPHITE_ENABLED
    return fArena.make([&](void *ptr) { return new(ptr) Entry(newKeyAsSpan, blendInfo); });
#else
    return fArena.make([&](void *ptr) { return new(ptr) Entry(newKeyAsSpan); });
#endif
}

size_t SkShaderCodeDictionary::SkPaintParamsKeyPtr::Hash::operator()(SkPaintParamsKeyPtr p) const {
    return SkOpts::hash_fn(p.fKey->data(), p.fKey->sizeInBytes(), 0);
}

size_t SkShaderCodeDictionary::RuntimeEffectKey::Hash::operator()(RuntimeEffectKey k) const {
    return SkOpts::hash_fn(&k, sizeof(k), 0);
}

const SkShaderCodeDictionary::Entry* SkShaderCodeDictionary::findOrCreate(
        SkPaintParamsKeyBuilder* builder) {
    const SkPaintParamsKey& key = builder->lockAsKey();

    SkAutoSpinlock lock{fSpinLock};

    Entry** existingEntry = fHash.find(SkPaintParamsKeyPtr{&key});
    if (existingEntry) {
        SkASSERT(fEntryVector[(*existingEntry)->uniqueID().asUInt()] == *existingEntry);
        return *existingEntry;
    }

#ifdef SK_GRAPHITE_ENABLED
    Entry* newEntry = this->makeEntry(key, builder->blendInfo());
#else
    Entry* newEntry = this->makeEntry(key);
#endif
    newEntry->setUniqueID(fEntryVector.size());
    fHash.set(SkPaintParamsKeyPtr{&newEntry->paintParamsKey()}, newEntry);
    fEntryVector.push_back(newEntry);

    return newEntry;
}

const SkShaderCodeDictionary::Entry* SkShaderCodeDictionary::lookup(
        SkUniquePaintParamsID codeID) const {

    if (!codeID.isValid()) {
        return nullptr;
    }

    SkAutoSpinlock lock{fSpinLock};

    SkASSERT(codeID.asUInt() < fEntryVector.size());

    return fEntryVector[codeID.asUInt()];
}

SkSpan<const SkUniform> SkShaderCodeDictionary::getUniforms(SkBuiltInCodeSnippetID id) const {
    return fBuiltInCodeSnippets[(int) id].fUniforms;
}

SkSpan<const DataPayloadField> SkShaderCodeDictionary::dataPayloadExpectations(
        int codeSnippetID) const {
    // All callers of this entry point should already have ensured that 'codeSnippetID' is valid
    return this->getEntry(codeSnippetID)->fDataPayloadExpectations;
}

const SkShaderSnippet* SkShaderCodeDictionary::getEntry(int codeSnippetID) const {
    if (codeSnippetID < 0) {
        return nullptr;
    }

    if (codeSnippetID < kBuiltInCodeSnippetIDCount) {
        return &fBuiltInCodeSnippets[codeSnippetID];
    }

    int userDefinedCodeSnippetID = codeSnippetID - kBuiltInCodeSnippetIDCount;
    if (userDefinedCodeSnippetID < SkTo<int>(fUserDefinedCodeSnippets.size())) {
        return fUserDefinedCodeSnippets[userDefinedCodeSnippetID].get();
    }

    return nullptr;
}

void SkShaderCodeDictionary::getShaderInfo(SkUniquePaintParamsID uniqueID, SkShaderInfo* info) {
    auto entry = this->lookup(uniqueID);

    entry->paintParamsKey().toShaderInfo(this, info);

#ifdef SK_GRAPHITE_ENABLED
    info->setBlendInfo(entry->blendInfo());
#endif
}

//--------------------------------------------------------------------------------------------------
namespace {

#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
static std::string append_default_snippet_arguments(const SkShaderSnippet* entry,
                                                    int entryIndex,
                                                    const std::string& currentPreLocalName,
                                                    SkSpan<const std::string> childOutputs) {
    std::string code = "(";

    // Append uniform names.
    const char* separator = "";
    for (size_t i = 0; i < entry->fUniforms.size(); ++i) {
        code += separator;
        separator = ", ";

        if (i == 0 && entry->needsLocalCoords()) {
            code += currentPreLocalName + " * dev2LocalUni";
        } else {
            code += entry->getMangledUniformName(i, entryIndex);
        }
    }

    // Append child output names.
    for (const std::string& childOutputVar : childOutputs) {
        code += separator;
        separator = ", ";
        code += childOutputVar;
    }
    code.push_back(')');

    return code;
}
#endif

// The default glue code just calls a built-in function with the signature:
//    half4 BuiltinFunctionName(/* all uniforms as parameters */);
// and stores the result in a variable named "resultName".
std::string GenerateDefaultGlueCode(const SkShaderInfo& shaderInfo,
                                    int* entryIndex,
                                    const SkPaintParamsKey::BlockReader& reader,
                                    const std::string& priorStageOutputName,
                                    const std::string& currentPreLocalName,
                                    std::string* preamble) {
#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
    const SkShaderSnippet* entry = reader.entry();
    SkASSERT(entry->fNumChildren == 0);

    if (entry->needsLocalCoords()) {
        // Any snippet that requests local coordinates must have a localMatrix as its first uniform.
        SkASSERT(entry->fUniforms.size() >= 1);
        SkASSERT(entry->fUniforms.front().type() == SkSLType::kFloat4x4);
    }

    return entry->fStaticFunctionName +
           append_default_snippet_arguments(entry, *entryIndex, currentPreLocalName,
                                            /*childOutputs=*/{});
#else
    return priorStageOutputName;
#endif  // defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
}

// The default-with-children glue code creates a function in the preamble with a signature of:
//     half4 BuiltinFunctionName_N(half4 inColor, float4x4 preLocal) { ... }
// This function invokes each child in sequence, and then calls the built-in function, passing all
// uniforms and child outputs along:
//     half4 BuiltinFunctionName(/* all uniforms as parameters */,
//                               /* all child output variable names as parameters */);
std::string GenerateDefaultGlueCodeWithChildren(const SkShaderInfo& shaderInfo,
                                                int* entryIndex,
                                                const SkPaintParamsKey::BlockReader& reader,
                                                const std::string& priorStageOutputName,
                                                const std::string& currentPreLocalName,
                                                std::string* preamble) {
#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
    const SkShaderSnippet* entry = reader.entry();
    SkASSERT(entry->fNumChildren > 0);

    if (entry->needsLocalCoords()) {
        // Any snippet that requests local coordinates must have a localMatrix as its first uniform.
        SkASSERT(entry->fUniforms.size() >= 1);
        SkASSERT(entry->fUniforms.front().type() == SkSLType::kFloat4x4);
    }

    // Create a helper function that invokes each of the children, then calls the snippet.
    int curEntryIndex = *entryIndex;
    std::string helperFnName = get_mangled_name(entry->fStaticFunctionName, curEntryIndex);
    std::string helperFn = SkSL::String::printf("half4 %s(half4 inColor, float4x4 preLocal) {",
                                                helperFnName.c_str());
    // Invoke all children from inside the helper function.
    std::vector<std::string> childOutputVarNames = emit_child_glue_code(shaderInfo,
                                                                        entryIndex,
                                                                        "inColor",
                                                                        "preLocal",
                                                                        preamble,
                                                                        &helperFn,
                                                                        /*indent=*/0);
    SkASSERT((int)childOutputVarNames.size() == entry->fNumChildren);

    // Finally, invoke the snippet from the helper function, passing uniforms and child outputs.
    SkSL::String::appendf(&helperFn, "    return %s", entry->fStaticFunctionName);
    helperFn += append_default_snippet_arguments(entry, curEntryIndex, "preLocal",
                                                 childOutputVarNames);
    helperFn += ";\n"
                "}\n";
    // Add the helper function to the bottom of the preamble.
    *preamble += helperFn;

    // Return an expression invoking the helper function.
    return SkSL::String::printf("%s(%s, %s)",
                                helperFnName.c_str(),
                                priorStageOutputName.c_str(),
                                currentPreLocalName.c_str());
#else
    return priorStageOutputName;
#endif  // defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
}

//--------------------------------------------------------------------------------------------------
static constexpr int kFourStopGradient = 4;
static constexpr int kEightStopGradient = 8;

static constexpr SkUniform kLinearGradientUniforms4[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat,  kFourStopGradient },
        { "point0",      SkSLType::kFloat2 },
        { "point1",      SkSLType::kFloat2 },
        { "tilemode",    SkSLType::kInt },
};
static constexpr SkUniform kLinearGradientUniforms8[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat,  kEightStopGradient },
        { "point0",      SkSLType::kFloat2 },
        { "point1",      SkSLType::kFloat2 },
        { "tilemode",    SkSLType::kInt },
};

static constexpr SkUniform kRadialGradientUniforms4[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat,  kFourStopGradient },
        { "center",      SkSLType::kFloat2 },
        { "radius",      SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
};
static constexpr SkUniform kRadialGradientUniforms8[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat,  kEightStopGradient },
        { "center",      SkSLType::kFloat2 },
        { "radius",      SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
};

static constexpr SkUniform kSweepGradientUniforms4[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat,  kFourStopGradient },
        { "center",      SkSLType::kFloat2 },
        { "bias",        SkSLType::kFloat },
        { "scale",       SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
};
static constexpr SkUniform kSweepGradientUniforms8[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat,  kEightStopGradient },
        { "center",      SkSLType::kFloat2 },
        { "bias",        SkSLType::kFloat },
        { "scale",       SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
};

static constexpr SkUniform kConicalGradientUniforms4[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat,  kFourStopGradient },
        { "point0",      SkSLType::kFloat2 },
        { "point1",      SkSLType::kFloat2 },
        { "radius0",     SkSLType::kFloat },
        { "radius1",     SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
};
static constexpr SkUniform kConicalGradientUniforms8[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat,  kEightStopGradient },
        { "point0",      SkSLType::kFloat2 },
        { "point1",      SkSLType::kFloat2 },
        { "radius0",     SkSLType::kFloat },
        { "radius1",     SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
};

static constexpr char kLinearGradient4Name[] = "sk_linear_grad_4_shader";
static constexpr char kLinearGradient8Name[] = "sk_linear_grad_8_shader";
static constexpr char kRadialGradient4Name[] = "sk_radial_grad_4_shader";
static constexpr char kRadialGradient8Name[] = "sk_radial_grad_8_shader";
static constexpr char kSweepGradient4Name[] = "sk_sweep_grad_4_shader";
static constexpr char kSweepGradient8Name[] = "sk_sweep_grad_8_shader";
static constexpr char kConicalGradient4Name[] = "sk_conical_grad_4_shader";
static constexpr char kConicalGradient8Name[] = "sk_conical_grad_8_shader";

//--------------------------------------------------------------------------------------------------
static constexpr SkUniform kSolidShaderUniforms[] = {
        { "color", SkSLType::kFloat4 }
};

static constexpr char kSolidShaderName[] = "sk_solid_shader";

//--------------------------------------------------------------------------------------------------
static constexpr SkUniform kLocalMatrixShaderUniforms[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
};

static constexpr int kNumLocalMatrixShaderChildren = 1;

static constexpr char kLocalMatrixShaderName[] = "sk_local_matrix_shader";

//--------------------------------------------------------------------------------------------------
static constexpr SkUniform kImageShaderUniforms[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
        { "subset",      SkSLType::kFloat4 },
        { "tilemodeX",   SkSLType::kInt },
        { "tilemodeY",   SkSLType::kInt },
        { "imgWidth",    SkSLType::kInt },
        { "imgHeight",   SkSLType::kInt },
};

static constexpr int kNumImageShaderTexturesAndSamplers = 1;
static constexpr SkTextureAndSampler kISTexturesAndSamplers[kNumImageShaderTexturesAndSamplers] = {
        {"sampler"},
};

static_assert(0 == static_cast<int>(SkTileMode::kClamp),  "ImageShader code depends on SkTileMode");
static_assert(1 == static_cast<int>(SkTileMode::kRepeat), "ImageShader code depends on SkTileMode");
static_assert(2 == static_cast<int>(SkTileMode::kMirror), "ImageShader code depends on SkTileMode");
static_assert(3 == static_cast<int>(SkTileMode::kDecal),  "ImageShader code depends on SkTileMode");

static constexpr char kImageShaderName[] = "sk_compute_coords";

// This is _not_ what we want to do.
// Ideally the "compute_coords" code snippet could just take texture and
// sampler references and do everything. That is going to take more time to figure out though so,
// for the sake of expediency, we're generating custom code to do the sampling.
std::string GenerateImageShaderGlueCode(const SkShaderInfo&,
                                        int* entryIndex,
                                        const SkPaintParamsKey::BlockReader& reader,
                                        const std::string& priorStageOutputName,
                                        const std::string& currentPreLocalName,
                                        std::string* preamble) {
#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
    std::string samplerVarName = std::string("sampler_") + std::to_string(*entryIndex) + "_0";

    // Uniform slot 0 is used to make the preLocalMatrix; it's handled in emit_glue_code_for_entry.
    std::string subsetName = reader.entry()->getMangledUniformName(1, *entryIndex);
    std::string tmXName = reader.entry()->getMangledUniformName(2, *entryIndex);
    std::string tmYName = reader.entry()->getMangledUniformName(3, *entryIndex);
    std::string imgWidthName = reader.entry()->getMangledUniformName(4, *entryIndex);
    std::string imgHeightName = reader.entry()->getMangledUniformName(5, *entryIndex);

    return SkSL::String::printf("sample(%s, %s(%s * dev2LocalUni, %s, %s, %s, %s, %s))",
                                samplerVarName.c_str(),
                                reader.entry()->fStaticFunctionName,
                                currentPreLocalName.c_str(),
                                subsetName.c_str(),
                                tmXName.c_str(),
                                tmYName.c_str(),
                                imgWidthName.c_str(),
                                imgHeightName.c_str());
#else
    return priorStageOutputName;
#endif  // defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
}

//--------------------------------------------------------------------------------------------------
static constexpr SkUniform kBlendShaderUniforms[] = {
        { "blendMode", SkSLType::kInt },
};

static constexpr int kNumBlendShaderChildren = 2;

static constexpr char kBlendShaderName[] = "sk_blend_shader";

//--------------------------------------------------------------------------------------------------
static constexpr char kRuntimeShaderName[] = "RuntimeEffect";

#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)

class GraphitePipelineCallbacks : public SkSL::PipelineStage::Callbacks {
public:
    GraphitePipelineCallbacks(std::string* preamble, int entryIndex)
            : fPreamble(preamble)
            , fEntryIndex(entryIndex) {}

    std::string declareUniform(const SkSL::VarDeclaration* decl) override {
        return get_mangled_name(std::string(decl->var().name()), fEntryIndex);
    }

    void defineFunction(const char* decl, const char* body, bool isMain) override {
        if (isMain) {
            SkSL::String::appendf(fPreamble,
                                  "half4 %s_%d(float4x4 preLocal, half4 inColor) {\n"
                                  "    float2 coords=(preLocal * dev2LocalUni * sk_FragCoord).xy;\n"
                                  "%s"
                                  "}\n",
                                  kRuntimeShaderName,
                                  fEntryIndex,
                                  body);
        } else {
            SkSL::String::appendf(fPreamble, "%s {\n%s}\n", decl, body);
        }
    }

    void declareFunction(const char* decl) override {
        *fPreamble += std::string(decl) + ";\n";
    }

    void defineStruct(const char* definition) override {
        *fPreamble += std::string(definition) + ";\n";
    }

    void declareGlobal(const char* declaration) override {
        *fPreamble += std::string(declaration) + ";\n";
    }

    std::string sampleShader(int index, std::string coords) override {
        // TODO(skia:13508): implement child shaders
        return "half4(0)";
    }

    std::string sampleColorFilter(int index, std::string color) override {
        // TODO(skia:13508): implement child color-filters
        return "half4(0)";
    }

    std::string sampleBlender(int index, std::string src, std::string dst) override {
        // TODO(skia:13508): implement child blenders
        return src;
    }

    std::string toLinearSrgb(std::string color) override {
        // TODO(skia:13508): implement to-linear-SRGB child effect
        return color;
    }
    std::string fromLinearSrgb(std::string color) override {
        // TODO(skia:13508): implement from-linear-SRGB child effect
        return color;
    }

    std::string getMangledName(const char* name) override {
        return get_mangled_name(name, fEntryIndex);
    }

private:
    std::string* fPreamble;
    int fEntryIndex;
};

#endif

std::string GenerateRuntimeShaderGlueCode(const SkShaderInfo& shaderInfo,
                                          int* entryIndex,
                                          const SkPaintParamsKey::BlockReader& reader,
                                          const std::string& priorStageOutputName,
                                          const std::string& currentPreLocalName,
                                          std::string* preamble) {
#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
    const SkShaderSnippet* entry = reader.entry();

    // Find this runtime effect in the runtime-effect dictionary.
    const int codeSnippetId = reader.codeSnippetId();
    const SkRuntimeEffect* effect = shaderInfo.runtimeEffectDictionary()->find(codeSnippetId);
    SkASSERT(effect);
    const SkSL::Program& program = SkRuntimeEffectPriv::Program(*effect);

    GraphitePipelineCallbacks callbacks{preamble, *entryIndex};
    SkASSERT(std::string_view(entry->fName) == kRuntimeShaderName);  // the callbacks assume this
    SkSL::PipelineStage::ConvertProgram(program, "coords", "inColor", "half4(1)", &callbacks);

    // We prepend a preLocalMatrix as the first uniform, ahead of the runtime effect's uniforms.
    // TODO: we can eliminate this uniform entirely if it's the identity matrix.
    // TODO: if we could inherit the parent's transform, this could be removed entirely.
    SkASSERT(entry->needsLocalCoords());
    SkASSERT(entry->fUniforms.front().type() == SkSLType::kFloat4x4);

    return SkSL::String::printf("%s_%d(%s, %s)",
                                entry->fName, *entryIndex,
                                currentPreLocalName.c_str(),
                                priorStageOutputName.c_str());
#else
    return priorStageOutputName;
#endif  // defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
}

//--------------------------------------------------------------------------------------------------
static constexpr char kErrorName[] = "sk_error";

//--------------------------------------------------------------------------------------------------
// This method generates the glue code for the case where the SkBlendMode-based blending is
// handled with fixed function blending.
std::string GenerateFixedFunctionBlenderGlueCode(const SkShaderInfo&,
                                                 int* entryIndex,
                                                 const SkPaintParamsKey::BlockReader& reader,
                                                 const std::string& priorStageOutputName,
                                                 const std::string& currentPreLocalName,
                                                 std::string* preamble) {
#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
    SkASSERT(reader.entry()->fUniforms.empty());
    SkASSERT(reader.numDataPayloadFields() == 0);

    // The actual blending is set up via the fixed function pipeline so we don't actually
    // need to access the blend mode in the glue code.
#endif  // defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)

    return priorStageOutputName;
}

//--------------------------------------------------------------------------------------------------
static constexpr SkUniform kShaderBasedBlenderUniforms[] = {
        { "blendMode", SkSLType::kInt },
};

static constexpr char kBlendHelperName[] = "sk_blend";

// This method generates the glue code for the case where the SkBlendMode-based blending must occur
// in the shader (i.e., fixed function blending isn't possible).
// It exists as custom glue code so that we can deal with the dest reads. If that can be
// standardized (e.g., via a snippets requirement flag) this could be removed.
std::string GenerateShaderBasedBlenderGlueCode(const SkShaderInfo&,
                                               int* entryIndex,
                                               const SkPaintParamsKey::BlockReader& reader,
                                               const std::string& priorStageOutputName,
                                               const std::string& currentPreLocalName,
                                               std::string* preamble) {
#if defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
    SkASSERT(reader.entry()->fUniforms.size() == 1);
    SkASSERT(reader.numDataPayloadFields() == 0);

    std::string uniformName = reader.entry()->getMangledUniformName(0, *entryIndex);

    // TODO: emit function to perform dest read into preamble, and replace half(1) with that call

    return SkSL::String::printf("%s(%s, %s, half4(1))",
                                reader.entry()->fStaticFunctionName,
                                uniformName.c_str(),
                                priorStageOutputName.c_str());
#else
    return priorStageOutputName;
#endif  // defined(SK_GRAPHITE_ENABLED) && defined(SK_ENABLE_SKSL)
}

//--------------------------------------------------------------------------------------------------

} // anonymous namespace

bool SkShaderCodeDictionary::isValidID(int snippetID) const {
    if (snippetID < 0) {
        return false;
    }

    if (snippetID < kBuiltInCodeSnippetIDCount) {
        return true;
    }

    int userDefinedCodeSnippetID = snippetID - kBuiltInCodeSnippetIDCount;
    return userDefinedCodeSnippetID < SkTo<int>(fUserDefinedCodeSnippets.size());
}

static constexpr int kNoChildren = 0;

int SkShaderCodeDictionary::addUserDefinedSnippet(
        const char* name,
        SkSpan<const SkUniform> uniforms,
        SnippetRequirementFlags snippetRequirementFlags,
        SkSpan<const SkTextureAndSampler> texturesAndSamplers,
        const char* functionName,
        SkShaderSnippet::GenerateExpressionForSnippetFn expressionGenerator,
        int numChildren,
        SkSpan<const SkPaintParamsKey::DataPayloadField> dataPayloadExpectations) {
    // TODO: the memory for user-defined entries could go in the dictionary's arena but that
    // would have to be a thread safe allocation since the arena also stores entries for
    // 'fHash' and 'fEntryVector'
    fUserDefinedCodeSnippets.push_back(std::make_unique<SkShaderSnippet>(name,
                                                                         uniforms,
                                                                         snippetRequirementFlags,
                                                                         texturesAndSamplers,
                                                                         functionName,
                                                                         expressionGenerator,
                                                                         numChildren,
                                                                         dataPayloadExpectations));

    return kBuiltInCodeSnippetIDCount + fUserDefinedCodeSnippets.size() - 1;
}

// TODO: this version needs to be removed
int SkShaderCodeDictionary::addUserDefinedSnippet(
        const char* name,
        SkSpan<const DataPayloadField> dataPayloadExpectations) {
    return this->addUserDefinedSnippet("UserDefined",
                                       {},  // no uniforms
                                       SnippetRequirementFlags::kNone,
                                       {},  // no samplers
                                       name,
                                       GenerateDefaultGlueCode,
                                       kNoChildren,
                                       dataPayloadExpectations);
}

#ifdef SK_ENABLE_PRECOMPILE
SkBlenderID SkShaderCodeDictionary::addUserDefinedBlender(sk_sp<SkRuntimeEffect> effect) {
    if (!effect) {
        return {};
    }

    // TODO: at this point we need to extract the uniform definitions, children and helper functions
    // from the runtime effect in order to create a real SkShaderSnippet
    // Additionally, we need to hash the provided code to deduplicate the runtime effects in case
    // the client keeps giving us different rtEffects w/ the same backing SkSL.
    int codeSnippetID = this->addUserDefinedSnippet("UserDefined",
                                                    {},  // missing uniforms
                                                    SnippetRequirementFlags::kNone,
                                                    {},  // missing samplers
                                                    "foo",
                                                    GenerateDefaultGlueCode,
                                                    kNoChildren,
                                                    /*dataPayloadExpectations=*/{});
    return SkBlenderID(codeSnippetID);
}

const SkShaderSnippet* SkShaderCodeDictionary::getEntry(SkBlenderID id) const {
    return this->getEntry(id.asUInt());
}

#endif // SK_ENABLE_PRECOMPILE

static SkSLType uniform_type_to_sksl_type(const SkRuntimeEffect::Uniform& u) {
    using Type = SkRuntimeEffect::Uniform::Type;
    if (u.flags & SkRuntimeEffect::Uniform::kHalfPrecision_Flag) {
        switch (u.type) {
            case Type::kFloat:    return SkSLType::kHalf;
            case Type::kFloat2:   return SkSLType::kHalf2;
            case Type::kFloat3:   return SkSLType::kHalf3;
            case Type::kFloat4:   return SkSLType::kHalf4;
            case Type::kFloat2x2: return SkSLType::kHalf2x2;
            case Type::kFloat3x3: return SkSLType::kHalf3x3;
            case Type::kFloat4x4: return SkSLType::kHalf4x4;
            case Type::kInt:      return SkSLType::kShort;
            case Type::kInt2:     return SkSLType::kShort2;
            case Type::kInt3:     return SkSLType::kShort3;
            case Type::kInt4:     return SkSLType::kShort4;
        }
    } else {
        switch (u.type) {
            case Type::kFloat:    return SkSLType::kFloat;
            case Type::kFloat2:   return SkSLType::kFloat2;
            case Type::kFloat3:   return SkSLType::kFloat3;
            case Type::kFloat4:   return SkSLType::kFloat4;
            case Type::kFloat2x2: return SkSLType::kFloat2x2;
            case Type::kFloat3x3: return SkSLType::kFloat3x3;
            case Type::kFloat4x4: return SkSLType::kFloat4x4;
            case Type::kInt:      return SkSLType::kInt;
            case Type::kInt2:     return SkSLType::kInt2;
            case Type::kInt3:     return SkSLType::kInt3;
            case Type::kInt4:     return SkSLType::kInt4;
        }
    }
    SkUNREACHABLE;
}

const char* SkShaderCodeDictionary::addTextToArena(std::string_view text) {
    char* textInArena = fArena.makeArrayDefault<char>(text.size() + 1);
    memcpy(textInArena, text.data(), text.size());
    textInArena[text.size()] = '\0';
    return textInArena;
}

SkSpan<const SkUniform> SkShaderCodeDictionary::convertUniforms(const SkRuntimeEffect* effect) {
    using Uniform = SkRuntimeEffect::Uniform;
    SkSpan<const Uniform> uniforms = effect->uniforms();

    // Convert the SkRuntimeEffect::Uniform array into its SkUniform equivalent.
    int numUniforms = uniforms.size() + 1;
    SkUniform* uniformArray = fArena.makeInitializedArray<SkUniform>(numUniforms, [&](int index) {
        // Graphite wants a `localMatrix` float4x4 uniform at the front of the uniform list.
        if (index == 0) {
            return SkUniform("localMatrix", SkSLType::kFloat4x4);
        }
        const Uniform& u = uniforms[index - 1];

        // The existing uniform names live in the passed-in SkRuntimeEffect and may eventually
        // disappear. Copy them into fArena. (It's safe to do this within makeInitializedArray; the
        // entire array is allocated in one big slab before any initialization calls are done.)
        const char* name = this->addTextToArena(u.name);

        // Add one SkUniform to our array.
        SkSLType type = uniform_type_to_sksl_type(u);
        return (u.flags & Uniform::kArray_Flag) ? SkUniform(name, type, u.count)
                                                : SkUniform(name, type);
    });

    return SkSpan<const SkUniform>(uniformArray, numUniforms);
}

int SkShaderCodeDictionary::findOrCreateRuntimeEffectSnippet(const SkRuntimeEffect* effect) {
    // Use the combination of {SkSL program hash, uniform size} as our key.
    // In the unfortunate event of a hash collision, at least we'll have the right amount of
    // uniform data available.
    RuntimeEffectKey key;
    key.fHash = SkRuntimeEffectPriv::Hash(*effect);
    key.fUniformSize = effect->uniformSize();

    SkAutoSpinlock lock{fSpinLock};

    int32_t* existingCodeSnippetID = fRuntimeEffectMap.find(key);
    if (existingCodeSnippetID) {
        return *existingCodeSnippetID;
    }

    int newCodeSnippetID = this->addUserDefinedSnippet("RuntimeEffect",
                                                       this->convertUniforms(effect),
                                                       SnippetRequirementFlags::kLocalCoords,
                                                       /*texturesAndSamplers=*/{},
                                                       kRuntimeShaderName,
                                                       GenerateRuntimeShaderGlueCode,
                                                       /*numChildren=*/0,
                                                       /*dataPayloadExpectations=*/{});
    fRuntimeEffectMap.set(key, newCodeSnippetID);
    return newCodeSnippetID;
}

SkShaderCodeDictionary::SkShaderCodeDictionary() {
    // The 0th index is reserved as invalid
    fEntryVector.push_back(nullptr);

    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kError] = {
            "Error",
            { },     // no uniforms
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kErrorName,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kSolidColorShader] = {
            "SolidColor",
            SkSpan(kSolidShaderUniforms),
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kSolidShaderName,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kLinearGradientShader4] = {
            "LinearGradient4",
            SkSpan(kLinearGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kLinearGradient4Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kLinearGradientShader8] = {
            "LinearGradient8",
            SkSpan(kLinearGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kLinearGradient8Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kRadialGradientShader4] = {
            "RadialGradient4",
            SkSpan(kRadialGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kRadialGradient4Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kRadialGradientShader8] = {
            "RadialGradient8",
            SkSpan(kRadialGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kRadialGradient8Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kSweepGradientShader4] = {
            "SweepGradient4",
            SkSpan(kSweepGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kSweepGradient4Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kSweepGradientShader8] = {
            "SweepGradient8",
            SkSpan(kSweepGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kSweepGradient8Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kConicalGradientShader4] = {
            "ConicalGradient4",
            SkSpan(kConicalGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kConicalGradient4Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kConicalGradientShader8] = {
            "ConicalGradient8",
            SkSpan(kConicalGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kConicalGradient8Name,
            GenerateDefaultGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kLocalMatrixShader] = {
            "LocalMatrixShader",
            SkSpan(kLocalMatrixShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kLocalMatrixShaderName,
            GenerateDefaultGlueCodeWithChildren,
            kNumLocalMatrixShaderChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kImageShader] = {
            "ImageShader",
            SkSpan(kImageShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kISTexturesAndSamplers, kNumImageShaderTexturesAndSamplers),
            kImageShaderName,
            GenerateImageShaderGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kBlendShader] = {
            "BlendShader",
            SkSpan(kBlendShaderUniforms),
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kBlendShaderName,
            GenerateDefaultGlueCodeWithChildren,
            kNumBlendShaderChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kFixedFunctionBlender] = {
            "FixedFunctionBlender",
            { },     // no uniforms
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            "FF-blending",  // fixed function blending doesn't use static SkSL
            GenerateFixedFunctionBlenderGlueCode,
            kNoChildren,
            { }      // no data payload
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kShaderBasedBlender] = {
            "ShaderBasedBlender",
            SkSpan(kShaderBasedBlenderUniforms),
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kBlendHelperName,
            GenerateShaderBasedBlenderGlueCode,
            kNoChildren,
            { }      // no data payload
    };
}
