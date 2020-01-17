/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GeneratedTestHarness.h"

#include <android-base/logging.h>
#include <android/hardware/neuralnetworks/1.0/IDevice.h>
#include <android/hardware/neuralnetworks/1.0/IExecutionCallback.h>
#include <android/hardware/neuralnetworks/1.0/IPreparedModel.h>
#include <android/hardware/neuralnetworks/1.0/IPreparedModelCallback.h>
#include <android/hardware/neuralnetworks/1.0/types.h>
#include <android/hardware/neuralnetworks/1.1/IDevice.h>
#include <android/hardware/neuralnetworks/1.2/IDevice.h>
#include <android/hardware/neuralnetworks/1.2/IExecutionCallback.h>
#include <android/hardware/neuralnetworks/1.2/IPreparedModel.h>
#include <android/hardware/neuralnetworks/1.2/IPreparedModelCallback.h>
#include <android/hardware/neuralnetworks/1.2/types.h>
#include <android/hardware/neuralnetworks/1.3/IDevice.h>
#include <android/hardware/neuralnetworks/1.3/IPreparedModel.h>
#include <android/hardware/neuralnetworks/1.3/IPreparedModelCallback.h>
#include <android/hardware/neuralnetworks/1.3/types.h>
#include <android/hidl/allocator/1.0/IAllocator.h>
#include <android/hidl/memory/1.0/IMemory.h>
#include <gtest/gtest.h>
#include <hidlmemory/mapping.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>

#include "1.0/Utils.h"
#include "1.2/Callbacks.h"
#include "1.3/Callbacks.h"
#include "ExecutionBurstController.h"
#include "MemoryUtils.h"
#include "TestHarness.h"
#include "Utils.h"
#include "VtsHalNeuralnetworks.h"

namespace android::hardware::neuralnetworks::V1_3::vts::functional {

using namespace test_helper;
using hidl::memory::V1_0::IMemory;
using implementation::PreparedModelCallback;
using V1_0::DataLocation;
using V1_0::ErrorStatus;
using V1_0::OperandLifeTime;
using V1_0::Request;
using V1_1::ExecutionPreference;
using V1_2::Constant;
using V1_2::MeasureTiming;
using V1_2::OutputShape;
using V1_2::SymmPerChannelQuantParams;
using V1_2::Timing;
using V1_2::implementation::ExecutionCallback;
using HidlToken = hidl_array<uint8_t, static_cast<uint32_t>(Constant::BYTE_SIZE_OF_CACHE_TOKEN)>;

namespace {

enum class Executor { ASYNC, SYNC, BURST };

enum class OutputType { FULLY_SPECIFIED, UNSPECIFIED, INSUFFICIENT };

struct TestConfig {
    Executor executor;
    MeasureTiming measureTiming;
    OutputType outputType;
    // `reportSkipping` indicates if a test should print an info message in case
    // it is skipped. The field is set to true by default and is set to false in
    // quantization coupling tests to suppress skipping a test
    bool reportSkipping;
    TestConfig(Executor executor, MeasureTiming measureTiming, OutputType outputType)
        : executor(executor),
          measureTiming(measureTiming),
          outputType(outputType),
          reportSkipping(true) {}
    TestConfig(Executor executor, MeasureTiming measureTiming, OutputType outputType,
               bool reportSkipping)
        : executor(executor),
          measureTiming(measureTiming),
          outputType(outputType),
          reportSkipping(reportSkipping) {}
};

}  // namespace

Model createModel(const TestModel& testModel) {
    // Model operands.
    hidl_vec<Operand> operands(testModel.operands.size());
    size_t constCopySize = 0, constRefSize = 0;
    for (uint32_t i = 0; i < testModel.operands.size(); i++) {
        const auto& op = testModel.operands[i];

        DataLocation loc = {};
        if (op.lifetime == TestOperandLifeTime::CONSTANT_COPY) {
            loc = {.poolIndex = 0,
                   .offset = static_cast<uint32_t>(constCopySize),
                   .length = static_cast<uint32_t>(op.data.size())};
            constCopySize += op.data.alignedSize();
        } else if (op.lifetime == TestOperandLifeTime::CONSTANT_REFERENCE) {
            loc = {.poolIndex = 0,
                   .offset = static_cast<uint32_t>(constRefSize),
                   .length = static_cast<uint32_t>(op.data.size())};
            constRefSize += op.data.alignedSize();
        }

        Operand::ExtraParams extraParams;
        if (op.type == TestOperandType::TENSOR_QUANT8_SYMM_PER_CHANNEL) {
            extraParams.channelQuant(SymmPerChannelQuantParams{
                    .scales = op.channelQuant.scales, .channelDim = op.channelQuant.channelDim});
        }

        operands[i] = {.type = static_cast<OperandType>(op.type),
                       .dimensions = op.dimensions,
                       .numberOfConsumers = op.numberOfConsumers,
                       .scale = op.scale,
                       .zeroPoint = op.zeroPoint,
                       .lifetime = static_cast<OperandLifeTime>(op.lifetime),
                       .location = loc,
                       .extraParams = std::move(extraParams)};
    }

    // Model operations.
    hidl_vec<Operation> operations(testModel.operations.size());
    std::transform(testModel.operations.begin(), testModel.operations.end(), operations.begin(),
                   [](const TestOperation& op) -> Operation {
                       return {.type = static_cast<OperationType>(op.type),
                               .inputs = op.inputs,
                               .outputs = op.outputs};
                   });

    // Constant copies.
    hidl_vec<uint8_t> operandValues(constCopySize);
    for (uint32_t i = 0; i < testModel.operands.size(); i++) {
        const auto& op = testModel.operands[i];
        if (op.lifetime == TestOperandLifeTime::CONSTANT_COPY) {
            const uint8_t* begin = op.data.get<uint8_t>();
            const uint8_t* end = begin + op.data.size();
            std::copy(begin, end, operandValues.data() + operands[i].location.offset);
        }
    }

    // Shared memory.
    hidl_vec<hidl_memory> pools = {};
    if (constRefSize > 0) {
        hidl_vec_push_back(&pools, nn::allocateSharedMemory(constRefSize));
        CHECK_NE(pools[0].size(), 0u);

        // load data
        sp<IMemory> mappedMemory = mapMemory(pools[0]);
        CHECK(mappedMemory.get() != nullptr);
        uint8_t* mappedPtr =
                reinterpret_cast<uint8_t*>(static_cast<void*>(mappedMemory->getPointer()));
        CHECK(mappedPtr != nullptr);

        for (uint32_t i = 0; i < testModel.operands.size(); i++) {
            const auto& op = testModel.operands[i];
            if (op.lifetime == TestOperandLifeTime::CONSTANT_REFERENCE) {
                const uint8_t* begin = op.data.get<uint8_t>();
                const uint8_t* end = begin + op.data.size();
                std::copy(begin, end, mappedPtr + operands[i].location.offset);
            }
        }
    }

    return {.operands = std::move(operands),
            .operations = std::move(operations),
            .inputIndexes = testModel.inputIndexes,
            .outputIndexes = testModel.outputIndexes,
            .operandValues = std::move(operandValues),
            .pools = std::move(pools),
            .relaxComputationFloat32toFloat16 = testModel.isRelaxed};
}

static bool isOutputSizeGreaterThanOne(const TestModel& testModel, uint32_t index) {
    const auto byteSize = testModel.operands[testModel.outputIndexes[index]].data.size();
    return byteSize > 1u;
}

static void makeOutputInsufficientSize(uint32_t outputIndex, Request* request) {
    auto& length = request->outputs[outputIndex].location.length;
    ASSERT_GT(length, 1u);
    length -= 1u;
}

static void makeOutputDimensionsUnspecified(Model* model) {
    for (auto i : model->outputIndexes) {
        auto& dims = model->operands[i].dimensions;
        std::fill(dims.begin(), dims.end(), 0);
    }
}

static Return<ErrorStatus> ExecutePreparedModel(const sp<IPreparedModel>& preparedModel,
                                                const Request& request, MeasureTiming measure,
                                                sp<ExecutionCallback>& callback) {
    return preparedModel->execute_1_3(request, measure, callback);
}
static Return<ErrorStatus> ExecutePreparedModel(const sp<IPreparedModel>& preparedModel,
                                                const Request& request, MeasureTiming measure,
                                                hidl_vec<OutputShape>* outputShapes,
                                                Timing* timing) {
    ErrorStatus result;
    Return<void> ret = preparedModel->executeSynchronously_1_3(
            request, measure,
            [&result, outputShapes, timing](ErrorStatus error, const hidl_vec<OutputShape>& shapes,
                                            const Timing& time) {
                result = error;
                *outputShapes = shapes;
                *timing = time;
            });
    if (!ret.isOk()) {
        return ErrorStatus::GENERAL_FAILURE;
    }
    return result;
}
static std::shared_ptr<::android::nn::ExecutionBurstController> CreateBurst(
        const sp<IPreparedModel>& preparedModel) {
    return android::nn::ExecutionBurstController::create(preparedModel,
                                                         std::chrono::microseconds{0});
}

void EvaluatePreparedModel(const sp<IPreparedModel>& preparedModel, const TestModel& testModel,
                           const TestConfig& testConfig, bool* skipped = nullptr) {
    if (skipped != nullptr) {
        *skipped = false;
    }
    // If output0 does not have size larger than one byte, we can not test with insufficient buffer.
    if (testConfig.outputType == OutputType::INSUFFICIENT &&
        !isOutputSizeGreaterThanOne(testModel, 0)) {
        return;
    }

    Request request = createRequest(testModel);
    if (testConfig.outputType == OutputType::INSUFFICIENT) {
        makeOutputInsufficientSize(/*outputIndex=*/0, &request);
    }

    ErrorStatus executionStatus;
    hidl_vec<OutputShape> outputShapes;
    Timing timing;
    switch (testConfig.executor) {
        case Executor::ASYNC: {
            SCOPED_TRACE("asynchronous");

            // launch execution
            sp<ExecutionCallback> executionCallback = new ExecutionCallback();
            Return<ErrorStatus> executionLaunchStatus = ExecutePreparedModel(
                    preparedModel, request, testConfig.measureTiming, executionCallback);
            ASSERT_TRUE(executionLaunchStatus.isOk());
            EXPECT_EQ(ErrorStatus::NONE, static_cast<ErrorStatus>(executionLaunchStatus));

            // retrieve execution status
            executionCallback->wait();
            executionStatus = executionCallback->getStatus();
            outputShapes = executionCallback->getOutputShapes();
            timing = executionCallback->getTiming();

            break;
        }
        case Executor::SYNC: {
            SCOPED_TRACE("synchronous");

            // execute
            Return<ErrorStatus> executionReturnStatus = ExecutePreparedModel(
                    preparedModel, request, testConfig.measureTiming, &outputShapes, &timing);
            ASSERT_TRUE(executionReturnStatus.isOk());
            executionStatus = static_cast<ErrorStatus>(executionReturnStatus);

            break;
        }
        case Executor::BURST: {
            SCOPED_TRACE("burst");

            // create burst
            const std::shared_ptr<::android::nn::ExecutionBurstController> controller =
                    CreateBurst(preparedModel);
            ASSERT_NE(nullptr, controller.get());

            // create memory keys
            std::vector<intptr_t> keys(request.pools.size());
            for (size_t i = 0; i < keys.size(); ++i) {
                keys[i] = reinterpret_cast<intptr_t>(&request.pools[i]);
            }

            // execute burst
            int n;
            std::tie(n, outputShapes, timing, std::ignore) =
                    controller->compute(request, testConfig.measureTiming, keys);
            executionStatus = nn::convertResultCodeToErrorStatus(n);

            break;
        }
    }

    if (testConfig.outputType != OutputType::FULLY_SPECIFIED &&
        executionStatus == ErrorStatus::GENERAL_FAILURE) {
        if (skipped != nullptr) {
            *skipped = true;
        }
        if (!testConfig.reportSkipping) {
            return;
        }
        LOG(INFO) << "NN VTS: Early termination of test because vendor service cannot "
                     "execute model that it does not support.";
        std::cout << "[          ]   Early termination of test because vendor service cannot "
                     "execute model that it does not support."
                  << std::endl;
        GTEST_SKIP();
    }
    if (testConfig.measureTiming == MeasureTiming::NO) {
        EXPECT_EQ(UINT64_MAX, timing.timeOnDevice);
        EXPECT_EQ(UINT64_MAX, timing.timeInDriver);
    } else {
        if (timing.timeOnDevice != UINT64_MAX && timing.timeInDriver != UINT64_MAX) {
            EXPECT_LE(timing.timeOnDevice, timing.timeInDriver);
        }
    }

    switch (testConfig.outputType) {
        case OutputType::FULLY_SPECIFIED:
            // If the model output operands are fully specified, outputShapes must be either
            // either empty, or have the same number of elements as the number of outputs.
            ASSERT_EQ(ErrorStatus::NONE, executionStatus);
            ASSERT_TRUE(outputShapes.size() == 0 ||
                        outputShapes.size() == testModel.outputIndexes.size());
            break;
        case OutputType::UNSPECIFIED:
            // If the model output operands are not fully specified, outputShapes must have
            // the same number of elements as the number of outputs.
            ASSERT_EQ(ErrorStatus::NONE, executionStatus);
            ASSERT_EQ(outputShapes.size(), testModel.outputIndexes.size());
            break;
        case OutputType::INSUFFICIENT:
            ASSERT_EQ(ErrorStatus::OUTPUT_INSUFFICIENT_SIZE, executionStatus);
            ASSERT_EQ(outputShapes.size(), testModel.outputIndexes.size());
            ASSERT_FALSE(outputShapes[0].isSufficient);
            return;
    }

    // Go through all outputs, check returned output shapes.
    for (uint32_t i = 0; i < outputShapes.size(); i++) {
        EXPECT_TRUE(outputShapes[i].isSufficient);
        const auto& expect = testModel.operands[testModel.outputIndexes[i]].dimensions;
        const std::vector<uint32_t> actual = outputShapes[i].dimensions;
        EXPECT_EQ(expect, actual);
    }

    // Retrieve execution results.
    const std::vector<TestBuffer> outputs = getOutputBuffers(request);

    // We want "close-enough" results.
    checkResults(testModel, outputs);
}

void EvaluatePreparedModel(const sp<IPreparedModel>& preparedModel, const TestModel& testModel,
                           TestKind testKind) {
    std::vector<OutputType> outputTypesList;
    std::vector<MeasureTiming> measureTimingList;
    std::vector<Executor> executorList;

    switch (testKind) {
        case TestKind::GENERAL: {
            outputTypesList = {OutputType::FULLY_SPECIFIED};
            measureTimingList = {MeasureTiming::NO, MeasureTiming::YES};
            executorList = {Executor::ASYNC, Executor::SYNC, Executor::BURST};
        } break;
        case TestKind::DYNAMIC_SHAPE: {
            outputTypesList = {OutputType::UNSPECIFIED, OutputType::INSUFFICIENT};
            measureTimingList = {MeasureTiming::NO, MeasureTiming::YES};
            executorList = {Executor::ASYNC, Executor::SYNC, Executor::BURST};
        } break;
        case TestKind::QUANTIZATION_COUPLING: {
            LOG(FATAL) << "Wrong TestKind for EvaluatePreparedModel";
            return;
        } break;
    }

    for (const OutputType outputType : outputTypesList) {
        for (const MeasureTiming measureTiming : measureTimingList) {
            for (const Executor executor : executorList) {
                const TestConfig testConfig(executor, measureTiming, outputType);
                EvaluatePreparedModel(preparedModel, testModel, testConfig);
            }
        }
    }
}

void EvaluatePreparedCoupledModels(const sp<IPreparedModel>& preparedModel,
                                   const TestModel& testModel,
                                   const sp<IPreparedModel>& preparedCoupledModel,
                                   const TestModel& coupledModel) {
    const std::vector<OutputType> outputTypesList = {OutputType::FULLY_SPECIFIED};
    const std::vector<MeasureTiming> measureTimingList = {MeasureTiming::NO, MeasureTiming::YES};
    const std::vector<Executor> executorList = {Executor::ASYNC, Executor::SYNC, Executor::BURST};

    for (const OutputType outputType : outputTypesList) {
        for (const MeasureTiming measureTiming : measureTimingList) {
            for (const Executor executor : executorList) {
                const TestConfig testConfig(executor, measureTiming, outputType,
                                            /*reportSkipping=*/false);
                bool baseSkipped = false;
                EvaluatePreparedModel(preparedModel, testModel, testConfig, &baseSkipped);
                bool coupledSkipped = false;
                EvaluatePreparedModel(preparedCoupledModel, coupledModel, testConfig,
                                      &coupledSkipped);
                ASSERT_EQ(baseSkipped, coupledSkipped);
                if (baseSkipped) {
                    LOG(INFO) << "NN VTS: Early termination of test because vendor service cannot "
                                 "execute model that it does not support.";
                    std::cout << "[          ]   Early termination of test because vendor service "
                                 "cannot "
                                 "execute model that it does not support."
                              << std::endl;
                    GTEST_SKIP();
                }
            }
        }
    }
}

void Execute(const sp<IDevice>& device, const TestModel& testModel, TestKind testKind) {
    Model model = createModel(testModel);
    if (testKind == TestKind::DYNAMIC_SHAPE) {
        makeOutputDimensionsUnspecified(&model);
    }

    sp<IPreparedModel> preparedModel;
    switch (testKind) {
        case TestKind::GENERAL: {
            createPreparedModel(device, model, &preparedModel);
            if (preparedModel == nullptr) return;
            EvaluatePreparedModel(preparedModel, testModel, TestKind::GENERAL);
        } break;
        case TestKind::DYNAMIC_SHAPE: {
            createPreparedModel(device, model, &preparedModel);
            if (preparedModel == nullptr) return;
            EvaluatePreparedModel(preparedModel, testModel, TestKind::DYNAMIC_SHAPE);
        } break;
        case TestKind::QUANTIZATION_COUPLING: {
            ASSERT_TRUE(testModel.hasQuant8CoupledOperands());
            createPreparedModel(device, model, &preparedModel, /*reportSkipping*/ false);
            TestModel signedQuantizedModel = convertQuant8AsymmOperandsToSigned(testModel);
            sp<IPreparedModel> preparedCoupledModel;
            createPreparedModel(device, createModel(signedQuantizedModel), &preparedCoupledModel,
                                /*reportSkipping*/ false);
            // If we couldn't prepare a model with unsigned quantization, we must
            // fail to prepare a model with signed quantization as well.
            if (preparedModel == nullptr) {
                ASSERT_EQ(preparedCoupledModel, nullptr);
                // If we failed to prepare both of the models, we can safely skip
                // the test.
                LOG(INFO) << "NN VTS: Early termination of test because vendor service cannot "
                             "prepare model that it does not support.";
                std::cout
                        << "[          ]   Early termination of test because vendor service cannot "
                           "prepare model that it does not support."
                        << std::endl;
                GTEST_SKIP();
            }
            ASSERT_NE(preparedCoupledModel, nullptr);
            EvaluatePreparedCoupledModels(preparedModel, testModel, preparedCoupledModel,
                                          signedQuantizedModel);
        } break;
    }
}

void GeneratedTestBase::SetUp() {
    testing::TestWithParam<GeneratedTestParam>::SetUp();
    ASSERT_NE(kDevice, nullptr);
}

std::vector<NamedModel> getNamedModels(const FilterFn& filter) {
    return TestModelManager::get().getTestModels(filter);
}

std::string printGeneratedTest(const testing::TestParamInfo<GeneratedTestParam>& info) {
    const auto& [namedDevice, namedModel] = info.param;
    return gtestCompliantName(getName(namedDevice) + "_" + getName(namedModel));
}

// Tag for the generated tests
class GeneratedTest : public GeneratedTestBase {};

// Tag for the dynamic output shape tests
class DynamicOutputShapeTest : public GeneratedTest {};

// Tag for the dynamic output shape tests
class QuantizationCouplingTest : public GeneratedTest {};

TEST_P(GeneratedTest, Test) {
    Execute(kDevice, kTestModel, /*testKind=*/TestKind::GENERAL);
}

TEST_P(DynamicOutputShapeTest, Test) {
    Execute(kDevice, kTestModel, /*testKind=*/TestKind::DYNAMIC_SHAPE);
}

TEST_P(QuantizationCouplingTest, Test) {
    Execute(kDevice, kTestModel, /*testKind=*/TestKind::QUANTIZATION_COUPLING);
}

INSTANTIATE_GENERATED_TEST(GeneratedTest,
                           [](const TestModel& testModel) { return !testModel.expectFailure; });

INSTANTIATE_GENERATED_TEST(DynamicOutputShapeTest,
                           [](const TestModel& testModel) { return !testModel.expectFailure; });

INSTANTIATE_GENERATED_TEST(QuantizationCouplingTest, [](const TestModel& testModel) {
    return testModel.hasQuant8CoupledOperands() && testModel.operations.size() == 1;
});

}  // namespace android::hardware::neuralnetworks::V1_3::vts::functional