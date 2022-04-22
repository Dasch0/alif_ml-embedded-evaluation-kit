/*
 * Copyright (c) 2021-2022 Arm Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "UseCaseHandler.hpp"

#include "InputFiles.hpp"
#include "Classifier.hpp"
#include "MicroNetKwsModel.hpp"
#include "hal.h"
#include "AudioUtils.hpp"
#include "ImageUtils.hpp"
#include "UseCaseCommonUtils.hpp"
#include "KwsResult.hpp"
#include "log_macros.h"
#include "KwsProcessing.hpp"

#include <vector>

using KwsClassifier = arm::app::Classifier;

namespace arm {
namespace app {


    /**
     * @brief           Presents KWS inference results.
     * @param[in]       results     Vector of KWS classification results to be displayed.
     * @return          true if successful, false otherwise.
     **/
    static bool PresentInferenceResult(const std::vector<arm::app::kws::KwsResult>& results);

    /* KWS inference handler. */
    bool ClassifyAudioHandler(ApplicationContext& ctx, uint32_t clipIndex, bool runAll)
    {
        auto& profiler = ctx.Get<Profiler&>("profiler");
        auto& model = ctx.Get<Model&>("model");
        const auto mfccFrameLength = ctx.Get<int>("frameLength");
        const auto mfccFrameStride = ctx.Get<int>("frameStride");
        const auto scoreThreshold = ctx.Get<float>("scoreThreshold");
        /* If the request has a valid size, set the audio index. */
        if (clipIndex < NUMBER_OF_FILES) {
            if (!SetAppCtxIfmIdx(ctx, clipIndex,"clipIndex")) {
                return false;
            }
        }
        auto initialClipIdx = ctx.Get<uint32_t>("clipIndex");

        constexpr uint32_t dataPsnTxtInfStartX = 20;
        constexpr uint32_t dataPsnTxtInfStartY = 40;
        constexpr int minTensorDims = static_cast<int>(
            (arm::app::MicroNetKwsModel::ms_inputRowsIdx > arm::app::MicroNetKwsModel::ms_inputColsIdx)?
             arm::app::MicroNetKwsModel::ms_inputRowsIdx : arm::app::MicroNetKwsModel::ms_inputColsIdx);


        if (!model.IsInited()) {
            printf_err("Model is not initialised! Terminating processing.\n");
            return false;
        }

        TfLiteTensor* inputTensor = model.GetInputTensor(0);
        if (!inputTensor->dims) {
            printf_err("Invalid input tensor dims\n");
            return false;
        } else if (inputTensor->dims->size < minTensorDims) {
            printf_err("Input tensor dimension should be >= %d\n", minTensorDims);
            return false;
        }

        /* Get input shape for feature extraction. */
        TfLiteIntArray* inputShape = model.GetInputShape(0);
        const uint32_t numMfccFeatures = inputShape->data[arm::app::MicroNetKwsModel::ms_inputColsIdx];

        /* We expect to be sampling 1 second worth of data at a time.
         * NOTE: This is only used for time stamp calculation. */
        const float secondsPerSample = 1.0 / audio::MicroNetKwsMFCC::ms_defaultSamplingFreq;

        /* Set up pre and post-processing. */
        KWSPreProcess preprocess = KWSPreProcess(&model, numMfccFeatures, mfccFrameLength, mfccFrameStride);

        std::vector<ClassificationResult> singleInfResult;
        KWSPostProcess postprocess = KWSPostProcess(ctx.Get<KwsClassifier &>("classifier"), &model,
                                                    ctx.Get<std::vector<std::string>&>("labels"),
                                                    singleInfResult);

        UseCaseRunner runner = UseCaseRunner(&preprocess, &postprocess, &model);

        do {
            hal_lcd_clear(COLOR_BLACK);

            auto currentIndex = ctx.Get<uint32_t>("clipIndex");

            /* Creating a sliding window through the whole audio clip. */
            auto audioDataSlider = audio::SlidingWindow<const int16_t>(
                    get_audio_array(currentIndex),
                    get_audio_array_size(currentIndex),
                    preprocess.m_audioDataWindowSize, preprocess.m_audioDataStride);

            /* Declare a container to hold results from across the whole audio clip. */
            std::vector<kws::KwsResult> finalResults;

            /* Display message on the LCD - inference running. */
            std::string str_inf{"Running inference... "};
            hal_lcd_display_text(str_inf.c_str(), str_inf.size(),
                    dataPsnTxtInfStartX, dataPsnTxtInfStartY, 0);
            info("Running inference on audio clip %" PRIu32 " => %s\n", currentIndex,
                 get_filename(currentIndex));

            /* Start sliding through audio clip. */
            while (audioDataSlider.HasNext()) {
                const int16_t* inferenceWindow = audioDataSlider.Next();

                /* The first window does not have cache ready. */
                preprocess.m_audioWindowIndex = audioDataSlider.Index();

                info("Inference %zu/%zu\n", audioDataSlider.Index() + 1,
                     audioDataSlider.TotalStrides() + 1);

                /* Run the pre-processing, inference and post-processing. */
                if (!runner.PreProcess(inferenceWindow, audio::MicroNetKwsMFCC::ms_defaultSamplingFreq)) {
                    return false;
                }

                profiler.StartProfiling("Inference");
                if (!runner.RunInference()) {
                    return false;
                }
                profiler.StopProfiling();

                if (!runner.PostProcess()) {
                    return false;
                }

                /* Add results from this window to our final results vector. */
                finalResults.emplace_back(kws::KwsResult(singleInfResult,
                        audioDataSlider.Index() * secondsPerSample * preprocess.m_audioDataStride,
                        audioDataSlider.Index(), scoreThreshold));

#if VERIFY_TEST_OUTPUT
                TfLiteTensor* outputTensor = model.GetOutputTensor(0);
                arm::app::DumpTensor(outputTensor);
#endif /* VERIFY_TEST_OUTPUT */
            } /* while (audioDataSlider.HasNext()) */

            /* Erase. */
            str_inf = std::string(str_inf.size(), ' ');
            hal_lcd_display_text(str_inf.c_str(), str_inf.size(),
                    dataPsnTxtInfStartX, dataPsnTxtInfStartY, false);

            ctx.Set<std::vector<kws::KwsResult>>("results", finalResults);

            if (!PresentInferenceResult(finalResults)) {
                return false;
            }

            profiler.PrintProfilingResult();

            IncrementAppCtxIfmIdx(ctx,"clipIndex");

        } while (runAll && ctx.Get<uint32_t>("clipIndex") != initialClipIdx);

        return true;
    }

    static bool PresentInferenceResult(const std::vector<arm::app::kws::KwsResult>& results)
    {
        constexpr uint32_t dataPsnTxtStartX1 = 20;
        constexpr uint32_t dataPsnTxtStartY1 = 30;
        constexpr uint32_t dataPsnTxtYIncr   = 16;  /* Row index increment. */

        hal_lcd_set_text_color(COLOR_GREEN);
        info("Final results:\n");
        info("Total number of inferences: %zu\n", results.size());

        /* Display each result */
        uint32_t rowIdx1 = dataPsnTxtStartY1 + 2 * dataPsnTxtYIncr;

        for (const auto & result : results) {

            std::string topKeyword{"<none>"};
            float score = 0.f;
            if (!result.m_resultVec.empty()) {
                topKeyword = result.m_resultVec[0].m_label;
                score = result.m_resultVec[0].m_normalisedVal;
            }

            std::string resultStr =
                    std::string{"@"} + std::to_string(result.m_timeStamp) +
                    std::string{"s: "} + topKeyword + std::string{" ("} +
                    std::to_string(static_cast<int>(score * 100)) + std::string{"%)"};

            hal_lcd_display_text(resultStr.c_str(), resultStr.size(),
                    dataPsnTxtStartX1, rowIdx1, false);
            rowIdx1 += dataPsnTxtYIncr;

            if (result.m_resultVec.empty()) {
                info("For timestamp: %f (inference #: %" PRIu32
                             "); label: %s; threshold: %f\n",
                     result.m_timeStamp, result.m_inferenceNumber,
                     topKeyword.c_str(),
                     result.m_threshold);
            } else {
                for (uint32_t j = 0; j < result.m_resultVec.size(); ++j) {
                    info("For timestamp: %f (inference #: %" PRIu32
                                 "); label: %s, score: %f; threshold: %f\n",
                         result.m_timeStamp,
                         result.m_inferenceNumber,
                         result.m_resultVec[j].m_label.c_str(),
                         result.m_resultVec[j].m_normalisedVal,
                         result.m_threshold);
                }
            }
        }

        return true;
    }

} /* namespace app */
} /* namespace arm */
