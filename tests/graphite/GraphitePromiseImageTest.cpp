/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/gpu/graphite/BackendTexture.h"

#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/Recording.h"
#include "src/gpu/graphite/Caps.h"
#include "src/gpu/graphite/ContextPriv.h"
#include "tests/Test.h"

using namespace skgpu::graphite;

namespace {

struct PromiseTextureChecker {
    PromiseTextureChecker() = default;

    explicit PromiseTextureChecker(const BackendTexture& backendTex,
                                   skiatest::Reporter* reporter)
            : fBackendTex(backendTex)
            , fReporter(reporter) {
    }

    void checkImageReleased(skiatest::Reporter* reporter, int expectedReleaseCnt) {
        REPORTER_ASSERT(reporter, expectedReleaseCnt == fImageReleaseCount);
    }

    BackendTexture fBackendTex;
    skiatest::Reporter* fReporter = nullptr;
    int fFulfillCount = 0;
    int fImageReleaseCount = 0;
    int fTextureReleaseCount = 0;

    static std::tuple<BackendTexture, void*> Fulfill(void* self) {
        auto checker = reinterpret_cast<PromiseTextureChecker*>(self);

        checker->fFulfillCount++;
        return { checker->fBackendTex, self };
    }

    static void ImageRelease(void* self) {
        auto checker = reinterpret_cast<PromiseTextureChecker*>(self);

        checker->fImageReleaseCount++;
    }

    static void TextureRelease(void* self) {
        auto checker = reinterpret_cast<PromiseTextureChecker*>(self);

        checker->fTextureReleaseCount++;
    }
};

enum class ReleaseBalanceExpectation {
    kBalanced,
    kOffByOne,     // fulfill calls ahead of release calls by 1
    kOffByTwo,     // fulfill calls ahead of release calls by 2
    kFulfillsOnly, // 'n' fulfill calls, 0 release calls
};

void check_fulfill_and_release_cnts(skiatest::Reporter* reporter,
                                    const PromiseTextureChecker& promiseChecker,
                                    int expectedFulfillCnt,
                                    ReleaseBalanceExpectation releaseBalanceExpectation) {
    SkASSERT(promiseChecker.fFulfillCount == expectedFulfillCnt);
    REPORTER_ASSERT(reporter, promiseChecker.fFulfillCount == expectedFulfillCnt);
    if (!expectedFulfillCnt) {
        // Release should only ever be called after Fulfill.
        REPORTER_ASSERT(reporter, !promiseChecker.fImageReleaseCount);
        REPORTER_ASSERT(reporter, !promiseChecker.fTextureReleaseCount);
        return;
    }

    int releaseDiff = promiseChecker.fFulfillCount - promiseChecker.fTextureReleaseCount;
    switch (releaseBalanceExpectation) {
        case ReleaseBalanceExpectation::kBalanced:
            SkASSERT(!releaseDiff);
            REPORTER_ASSERT(reporter, !releaseDiff);
            break;
        case ReleaseBalanceExpectation::kOffByOne:
            SkASSERT(releaseDiff == 1);
            REPORTER_ASSERT(reporter, releaseDiff == 1);
            break;
        case ReleaseBalanceExpectation::kOffByTwo:
            SkASSERT(releaseDiff == 2);
            REPORTER_ASSERT(reporter, releaseDiff == 2);
            break;
        case ReleaseBalanceExpectation::kFulfillsOnly:
            REPORTER_ASSERT(reporter, promiseChecker.fTextureReleaseCount == 0);
            break;
    }
}

void check_unfulfilled(const PromiseTextureChecker& promiseChecker,
                       skiatest::Reporter* reporter) {
    check_fulfill_and_release_cnts(reporter, promiseChecker, /* expectedFulfillCnt= */ 0,
                                   ReleaseBalanceExpectation::kBalanced);
}

void check_fulfilled_ahead_by_one(skiatest::Reporter* reporter,
                                  const PromiseTextureChecker& promiseChecker,
                                  int expectedFulfillCnt) {
    check_fulfill_and_release_cnts(reporter, promiseChecker, expectedFulfillCnt,
                                   ReleaseBalanceExpectation::kOffByOne);
}

void check_fulfilled_ahead_by_two(skiatest::Reporter* reporter,
                                  const PromiseTextureChecker& promiseChecker,
                                  int expectedFulfillCnt) {
    check_fulfill_and_release_cnts(reporter, promiseChecker, expectedFulfillCnt,
                                   ReleaseBalanceExpectation::kOffByTwo);
}

void check_all_done(skiatest::Reporter* reporter,
                    const PromiseTextureChecker& promiseChecker,
                    int expectedFulfillCnt) {
    check_fulfill_and_release_cnts(reporter, promiseChecker, expectedFulfillCnt,
                                   ReleaseBalanceExpectation::kBalanced);
}

void check_fulfills_only(skiatest::Reporter* reporter,
                         const PromiseTextureChecker& promiseChecker,
                         int expectedFulfillCnt) {
    check_fulfill_and_release_cnts(reporter, promiseChecker, expectedFulfillCnt,
                                   ReleaseBalanceExpectation::kFulfillsOnly);
}

struct TestCtx {
    TestCtx() {}

    std::unique_ptr<Recorder> fRecorder;
    BackendTexture fBackendTex;
    PromiseTextureChecker fPromiseChecker;
    sk_sp<SkImage> fImg;
    sk_sp<SkSurface> fSurface;
};

void setup_test_context(Context* context,
                        skiatest::Reporter* reporter,
                        TestCtx* testCtx,
                        SkISize dimensions,
                        Volatile isVolatile,
                        bool invalidBackendTex) {
    const Caps* caps = context->priv().caps();
    testCtx->fRecorder = context->makeRecorder();

    TextureInfo textureInfo = caps->getDefaultSampledTextureInfo(kRGBA_8888_SkColorType,
                                                                 Mipmapped::kNo,
                                                                 skgpu::Protected::kNo,
                                                                 Renderable::kYes);

    if (invalidBackendTex) {
        // This will invalidate all fulfill calls
        testCtx->fBackendTex = {};
        REPORTER_ASSERT(reporter, !testCtx->fBackendTex.isValid());
    } else {
        testCtx->fBackendTex = testCtx->fRecorder->createBackendTexture(dimensions, textureInfo);
        REPORTER_ASSERT(reporter, testCtx->fBackendTex.isValid());
    }

    testCtx->fPromiseChecker = PromiseTextureChecker(testCtx->fBackendTex, reporter);

    SkImageInfo ii = SkImageInfo::Make(dimensions.fWidth,
                                       dimensions.fHeight,
                                       kRGBA_8888_SkColorType,
                                       kPremul_SkAlphaType);

    testCtx->fImg = SkImage::MakeGraphitePromiseTexture(testCtx->fRecorder.get(),
                                                        dimensions,
                                                        textureInfo,
                                                        ii.colorInfo(),
                                                        isVolatile,
                                                        PromiseTextureChecker::Fulfill,
                                                        PromiseTextureChecker::ImageRelease,
                                                        PromiseTextureChecker::TextureRelease,
                                                        &testCtx->fPromiseChecker);

    testCtx->fSurface = SkSurface::MakeGraphite(testCtx->fRecorder.get(), ii);
}

} // anonymous namespace

DEF_GRAPHITE_TEST_FOR_RENDERING_CONTEXTS(NonVolatileGraphitePromiseImageTest,
                                         reporter,
                                         context) {
    constexpr SkISize kDimensions { 16, 16 };

    TestCtx testContext;
    setup_test_context(context, reporter, &testContext,
                       kDimensions, Volatile::kNo, /* invalidBackendTex= */ false);

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_unfulfilled(testContext.fPromiseChecker, reporter); // NVPIs not fulfilled at snap

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 1); // NVPIs fulfilled at insert
    }

    context->submit(SyncToCpu::kNo);
    // testContext.fImg still has a ref so we should not have called TextureRelease.
    check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                 /* expectedFulfillCnt= */ 1);

    context->submit(SyncToCpu::kYes);
    check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                 /* expectedFulfillCnt= */ 1);

    // Test that more draws and insertions don't refulfill the NVPI
    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        canvas->drawImage(testContext.fImg, 0, 0);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 1); // No new fulfill

        context->insertRecording({ recording.get() });
        // testContext.fImg should still be fulfilled from the first time we inserted a Recording.
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 1);
    }

    context->submit(SyncToCpu::kYes);
    check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                 /* expectedFulfillCnt= */ 1);

    // Test that dropping the SkImage's ref doesn't change anything
    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        testContext.fImg.reset();

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 1);

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 1);
    }

    // fImg's proxy is reffed by the recording so, despite fImg being reset earlier,
    // the imageRelease callback doesn't occur until the recording is deleted.
    testContext.fPromiseChecker.checkImageReleased(reporter, /* expectedReleaseCnt= */ 1);

    // testContext.fImg no longer holds a ref but the last recording is still not submitted.
    check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                 /* expectedFulfillCnt= */ 1);

    context->submit(SyncToCpu::kYes);

    // Now TextureRelease should definitely have been called.
    check_all_done(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 1);

    context->deleteBackendTexture(testContext.fBackendTex);
}

DEF_GRAPHITE_TEST_FOR_RENDERING_CONTEXTS(NonVolatileGraphitePromiseImageFulfillFailureTest,
                                         reporter,
                                         context) {
    constexpr SkISize kDimensions { 16, 16 };

    TestCtx testContext;
    setup_test_context(context, reporter, &testContext,
                       kDimensions, Volatile::kNo, /* invalidBackendTex= */ true);

    // Draw the image a few different ways.
    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 1);

        // Test that reinserting gives uninstantiated PromiseImages a second chance
        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 2);
    }

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        SkPaint paint;
        paint.setColorFilter(SkColorFilters::LinearToSRGBGamma());
        canvas->drawImage(testContext.fImg, 0, 0, SkSamplingOptions(), &paint);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 2);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 3);
    }

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        sk_sp<SkShader> shader = testContext.fImg->makeShader(SkSamplingOptions());
        REPORTER_ASSERT(reporter, shader);

        SkPaint paint;
        paint.setShader(std::move(shader));
        canvas->drawRect(SkRect::MakeWH(1, 1), paint);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 3);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 4);
    }

    testContext.fSurface.reset();
    testContext.fImg.reset();

    // Despite fulfill failing 4x, the imageRelease callback still fires
    testContext.fPromiseChecker.checkImageReleased(reporter, /* expectedReleaseCnt= */ 1);

    context->submit(SyncToCpu::kYes);
    // fulfill should've been called 4x while release should never have been called
    check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 4);
}

DEF_GRAPHITE_TEST_FOR_RENDERING_CONTEXTS(NonVolatileGraphitePromiseImageCreationFailureTest,
                                         reporter,
                                         context) {
    // Note: these dimensions are invalid and will cause MakeGraphitePromiseTexture to fail
    constexpr SkISize kDimensions { 0, 0 };

    TestCtx testContext;
    setup_test_context(context, reporter, &testContext,
                       kDimensions, Volatile::kNo, /* invalidBackendTex= */ true);

    SkASSERT(!testContext.fImg);

    // Despite MakeGraphitePromiseTexture failing, ImageRelease is called
    REPORTER_ASSERT(reporter, testContext.fPromiseChecker.fFulfillCount == 0);
    REPORTER_ASSERT(reporter, testContext.fPromiseChecker.fImageReleaseCount == 1);
    REPORTER_ASSERT(reporter, testContext.fPromiseChecker.fTextureReleaseCount == 0);
}

DEF_GRAPHITE_TEST_FOR_RENDERING_CONTEXTS(VolatileGraphitePromiseImageTest,
                                         reporter,
                                         context) {
    constexpr SkISize kDimensions { 16, 16 };

    TestCtx testContext;
    setup_test_context(context, reporter, &testContext,
                       kDimensions, Volatile::kYes, /* invalidBackendTex= */ false);

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        // Nothing happens at snap time for VPIs
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 1);  // VPIs fulfilled on insert

        // Test that multiple insertions will clobber prior fulfills
        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_two(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 2);
    }

    context->submit(SyncToCpu::kYes);
    check_all_done(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 2);

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        canvas->drawImage(testContext.fImg, 0, 0);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        // Nothing happens at snap time for volatile images
        check_all_done(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 2);

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 3);

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_two(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 4);
    }

    context->submit(SyncToCpu::kYes);
    check_all_done(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 4);

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        testContext.fImg.reset();

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        // Nothing happens at snap time for volatile images
        check_all_done(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 4);

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_one(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 5);

        context->insertRecording({ recording.get() });
        check_fulfilled_ahead_by_two(reporter, testContext.fPromiseChecker,
                                     /* expectedFulfillCnt= */ 6);
    }

    // testContext.fImg no longer holds a ref but the last recordings are still not submitted.
    check_fulfilled_ahead_by_two(reporter, testContext.fPromiseChecker,
                                 /* expectedFulfillCnt= */ 6);

    context->submit(SyncToCpu::kYes);

    // Now all Releases should definitely have been called.
    check_all_done(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 6);

    context->deleteBackendTexture(testContext.fBackendTex);
}

DEF_GRAPHITE_TEST_FOR_RENDERING_CONTEXTS(VolatileGraphitePromiseImageFulfillFailureTest,
                                         reporter,
                                         context) {
    constexpr SkISize kDimensions { 16, 16 };

    TestCtx testContext;
    setup_test_context(context, reporter, &testContext,
                       kDimensions, Volatile::kYes, /* invalidBackendTex= */ true);

    // Draw the image a few different ways.
    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 1);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 2);
    }

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        SkPaint paint;
        paint.setColorFilter(SkColorFilters::LinearToSRGBGamma());
        canvas->drawImage(testContext.fImg, 0, 0, SkSamplingOptions(), &paint);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 2);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 3);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 4);
    }

    {
        SkCanvas* canvas = testContext.fSurface->getCanvas();

        sk_sp<SkShader> shader = testContext.fImg->makeShader(SkSamplingOptions());
        REPORTER_ASSERT(reporter, shader);

        SkPaint paint;
        paint.setShader(std::move(shader));
        canvas->drawRect(SkRect::MakeWH(1, 1), paint);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 4);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 5);

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 6);
    }

    testContext.fSurface.reset();
    testContext.fImg.reset();

    context->submit(SyncToCpu::kYes);
    check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 6);
}

// Test out dropping the Recorder prior to inserting the Recording
DEF_GRAPHITE_TEST_FOR_RENDERING_CONTEXTS(GraphitePromiseImageRecorderLoss,
                                         reporter,
                                         context) {
    constexpr SkISize kDimensions{ 16, 16 };

    for (Volatile isVolatile : { Volatile::kNo, Volatile::kYes }) {
        TestCtx testContext;
        setup_test_context(context, reporter, &testContext,
                           kDimensions, isVolatile, /* invalidBackendTex= */ false);

        SkCanvas* canvas = testContext.fSurface->getCanvas();

        canvas->drawImage(testContext.fImg, 0, 0);
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        std::unique_ptr<Recording> recording = testContext.fRecorder->snap();
        check_unfulfilled(testContext.fPromiseChecker, reporter);

        testContext.fRecorder.reset();  // Recorder drop

        context->insertRecording({ recording.get() });
        check_fulfills_only(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 1);

        context->submit(SyncToCpu::kYes);

        testContext.fSurface.reset();
        testContext.fImg.reset();
        recording.reset();

        check_all_done(reporter, testContext.fPromiseChecker, /* expectedFulfillCnt= */ 1);

        context->deleteBackendTexture(testContext.fBackendTex);
    }
}
