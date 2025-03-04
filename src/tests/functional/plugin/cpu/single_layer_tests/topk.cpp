// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <common_test_utils/ov_tensor_utils.hpp>
#include "test_utils/cpu_test_utils.hpp"
#include "shared_test_classes/base/ov_subgraph.hpp"
#include "ngraph_functions/builders.hpp"

using namespace InferenceEngine;
using namespace CPUTestUtils;
using namespace ov::test;

namespace CPULayerTestsDefinitions {

typedef std::tuple<
        int64_t,                        // keepK
        int64_t,                        // axis
        ngraph::opset4::TopK::Mode,     // mode
        ngraph::opset4::TopK::SortType, // sort
        ElementType,                    // Net precision
        ElementType,                    // Input precision
        ElementType,                    // Output precision
        InputShape                      // inputShape
> basicTopKParams;

typedef std::tuple<
        basicTopKParams,
        CPUSpecificParams,
        std::map<std::string, std::string>> TopKLayerCPUTestParamsSet;

class TopKLayerCPUTest : public testing::WithParamInterface<TopKLayerCPUTestParamsSet>,
                         virtual public SubgraphBaseTest, public CPUTestsBase {
public:
    static std::string getTestCaseName(testing::TestParamInfo<TopKLayerCPUTestParamsSet> obj) {
        basicTopKParams basicParamsSet;
        CPUSpecificParams cpuParams;
        std::map<std::string, std::string> additionalConfig;
        std::tie(basicParamsSet, cpuParams, additionalConfig) = obj.param;

        int64_t keepK, axis;
        ngraph::opset4::TopK::Mode mode;
        ngraph::opset4::TopK::SortType sort;
        ElementType netPrecision, inPrc, outPrc;
        InputShape inputShape;
        std::tie(keepK, axis, mode, sort, netPrecision, inPrc, outPrc, inputShape) = basicParamsSet;

        std::ostringstream result;
        bool staticShape = inputShape.first.rank() == 0;
        if (staticShape)
            result << "k=" << keepK << "_";
        result << "axis=" << axis << "_";
        result << "mode=" << mode << "_";
        result << "sort=" << sort << "_";
        result << "netPRC=" << netPrecision << "_";
        result << "inPRC=" << inPrc << "_";
        result << "outPRC=" << outPrc << "_";
        result << "IS=" << CommonTestUtils::partialShape2str({inputShape.first}) << "_" << "TS=(";
        for (const auto& shape : inputShape.second) {
            result << CommonTestUtils::vec2str(shape) << "_";
        }

        result << CPUTestsBase::getTestCaseName(cpuParams);

        if (!additionalConfig.empty()) {
            result << "_PluginConf";
            for (auto& item : additionalConfig) {
                if (item.second == PluginConfigParams::YES)
                    result << "_" << item.first << "=" << item.second;
            }
        }

        return result.str();
    }

protected:
    void SetUp() override {
        targetDevice = CommonTestUtils::DEVICE_CPU;

        basicTopKParams basicParamsSet;
        CPUSpecificParams cpuParams;
        std::map<std::string, std::string> additionalConfig;
        std::tie(basicParamsSet, cpuParams, additionalConfig) = this->GetParam();

        std::tie(inFmts, outFmts, priority, selectedType) = cpuParams;

        int64_t keepK;
        ngraph::opset4::TopK::Mode mode;
        ngraph::opset4::TopK::SortType sort;
        ElementType inPrc, outPrc;
        InputShape inputShape;
        std::tie(keepK, axis, mode, sort, netPrecision, inPrc, outPrc, inputShape) = basicParamsSet;

        if (additionalConfig[PluginConfigParams::KEY_ENFORCE_BF16] == PluginConfigParams::YES)
            inPrc = outPrc = netPrecision = ElementType::bf16;
        else
            inPrc = outPrc = netPrecision;
        configuration.insert(additionalConfig.begin(), additionalConfig.end());

        selectedType = getPrimitiveType() + "_" + InferenceEngine::details::convertPrecision(netPrecision).name();

        staticShape = inputShape.first.rank() == 0;
        if (staticShape) {
            init_input_shapes({inputShape});
        } else {
            inputDynamicShapes = {inputShape.first, {}};
            for (size_t i = 0; i < inputShape.second.size(); ++i) {
                targetStaticShapes.push_back({inputShape.second[i], {}});
            }
        }

        auto params = ngraph::builder::makeDynamicParams(netPrecision, {inputDynamicShapes[0]});

        // static shape need specific const k to test different sorting algorithms, dynamic shape tests random param k
        std::shared_ptr<ngraph::opset4::TopK> topk;
        if (staticShape) {
            auto k = std::make_shared<ngraph::opset3::Constant>(ngraph::element::Type_t::i64, ngraph::Shape{}, &keepK);
            topk = std::dynamic_pointer_cast<ngraph::opset4::TopK>(
                    std::make_shared<ngraph::opset4::TopK>(params[0], k, axis, mode, sort));
        } else {
            auto k = std::make_shared<ngraph::opset3::Parameter>(ngraph::element::Type_t::i64, inputDynamicShapes[1]);
            params.push_back(k);
            topk = std::dynamic_pointer_cast<ngraph::opset4::TopK>(
                    std::make_shared<ngraph::opset4::TopK>(params[0], k, axis, mode, sort));
        }

        topk->get_rt_info() = getCPUInfo();

        ngraph::ResultVector results;
        for (size_t i = 0; i < topk->get_output_size(); i++) {
            results.push_back(std::make_shared<ngraph::opset4::Result>(topk->output(i)));
        }

        function = std::make_shared<ngraph::Function>(results, params, "TopK");
    }

    void generate_inputs(const std::vector<ngraph::Shape>& targetInputStaticShapes) override {
        inputs.clear();
        const auto& funcInputs = function->inputs();

        // Spec TopK_3.md allows to use unstable sorting, thus generate unreapeated input data to avoid a. and b.
        // a. Skip comparing of index results, because an element in actual index tensor can be different with
        //    its counterpart in expected index tensor
        // b. If SortType is SORT_INDICES or NONE, the test program still needs to apply std::sort for all pairs
        //    of 1xk value vectors in expected and actual output tensor before comparing them
        auto shape = targetInputStaticShapes.front();
        ov::Tensor tensor;
        tensor = ov::test::utils::create_and_fill_tensor(funcInputs[0].get_element_type(), shape);
        size_t size = tensor.get_size();

        if (netPrecision == ElementType::f32) {
            std::vector<int> data(size);
            int start = - static_cast<int>(size / 2);
            std::iota(data.begin(), data.end(), start);
            std::mt19937 gen(0);
            std::shuffle(data.begin(), data.end(), gen);

            auto *rawBlobDataPtr = static_cast<float *>(tensor.data());
            for (size_t i = 0; i < size; ++i) {
                rawBlobDataPtr[i] = static_cast<float>(data[i]);
            }
        } else if (netPrecision == ElementType::bf16) {
            size_t O = 1, A = 1, I = 1;
            A = shape[axis];
            for (size_t i = 0; i < axis; i++)
                O *= shape[i];
            for (size_t i = axis + 1; i < shape.size(); i++)
                I *= shape[i];
            if (O * A * I != size)
                FAIL() << "Incorrect blob shape " << shape;

            auto *rawBlobDataPtr = static_cast<ngraph::bfloat16 *>(tensor.data());
            for (size_t o = 0; o < O; o++) {
                for (size_t i = 0; i < I; i++) {
                    std::vector<int> data(A);
                    int start = - static_cast<int>(A / 2);
                    std::iota(data.begin(), data.end(), start);
                    const size_t seed = (o + 1) * (i + 1);
                    std::mt19937 gen(seed);
                    std::shuffle(data.begin(), data.end(), gen);
                    for (size_t a = 0; a < A; a++) {
                        rawBlobDataPtr[o * A * I + a * I + i] = static_cast<ngraph::bfloat16>(data[a]);
                    }
                }
            }
        } else {
            FAIL() << "generate_inputs for " << netPrecision << " precision isn't supported";
        }
        inputs.insert({funcInputs[0].get_node_shared_ptr(), tensor});

        if (!staticShape) {
            const auto& kPrecision = funcInputs[1].get_element_type();
            const auto& kShape = targetInputStaticShapes[1];

            const size_t startFrom = 1;
            const size_t range = targetInputStaticShapes[0][axis] - 1;
            const size_t seed = inferRequestNum++;
            const auto kTensor = ov::test::utils::create_and_fill_tensor(kPrecision, kShape, range, startFrom, 1, seed);

            inputs.insert({funcInputs[1].get_node_shared_ptr(), kTensor});
        }
    }

private:
    int64_t axis;
    size_t inferRequestNum = 0;
    ElementType netPrecision;
    bool staticShape;
};

TEST_P(TopKLayerCPUTest, CompareWithRefs) {
    SKIP_IF_CURRENT_TEST_IS_DISABLED()

    run();
    CheckPluginRelatedResults(compiledModel, "TopK");
}

namespace {

const std::vector<ElementType> netPrecisions = {
    ElementType::f32,
};

std::vector<std::map<std::string, std::string>> additionalConfig = {
    {{PluginConfigParams::KEY_ENFORCE_BF16, PluginConfigParams::NO}},
    {{PluginConfigParams::KEY_ENFORCE_BF16, PluginConfigParams::YES}}
};

const std::vector<int64_t> axes = {0, 1, 2, 3};
const std::vector<int64_t> k = {1, 5, 7, 18, 21};

const std::vector<ngraph::opset4::TopK::Mode> modes = {
    ngraph::opset4::TopK::Mode::MIN,
    ngraph::opset4::TopK::Mode::MAX
};

const std::vector<ngraph::opset4::TopK::SortType> sortTypes = {
    ngraph::opset4::TopK::SortType::SORT_VALUES,
    ngraph::opset4::TopK::SortType::SORT_INDICES,
};

std::vector<ov::test::InputShape> inputShapes = {
    {{}, {{21, 21, 21, 21}}},
};

std::vector<ov::test::InputShape> inputShapesDynamic = {
    {{21, {20, 25}, 21, {20, 25}}, {{21, 21, 21, 21}, {21, 22, 21, 23}}}
};

std::vector<CPUSpecificParams> cpuParams = {
    CPUSpecificParams({nChw16c, x}, {nChw16c, nChw16c}, {}, {}),
    CPUSpecificParams({nchw, x}, {nchw, nchw}, {}, {}),
    CPUSpecificParams({nhwc, x}, {nhwc, nhwc}, {}, {})
};

INSTANTIATE_TEST_CASE_P(smoke_TopK, TopKLayerCPUTest,
    ::testing::Combine(
        ::testing::Combine(
            ::testing::ValuesIn(k),
            ::testing::ValuesIn(axes),
            ::testing::ValuesIn(modes),
            ::testing::ValuesIn(sortTypes),
            ::testing::ValuesIn(netPrecisions),
            ::testing::Values(ElementType::undefined),
            ::testing::Values(ElementType::undefined),
            ::testing::ValuesIn(inputShapes)),
        ::testing::ValuesIn(filterCPUSpecificParams(cpuParams)),
        ::testing::ValuesIn(additionalConfig)),
    TopKLayerCPUTest::getTestCaseName);

INSTANTIATE_TEST_CASE_P(smoke_TopK_dynamic, TopKLayerCPUTest,
    ::testing::Combine(
        ::testing::Combine(
            ::testing::Values(1),
            ::testing::ValuesIn(axes),
            ::testing::ValuesIn(modes),
            ::testing::ValuesIn(sortTypes),
            ::testing::ValuesIn(netPrecisions),
            ::testing::Values(ElementType::undefined),
            ::testing::Values(ElementType::undefined),
            ::testing::ValuesIn(inputShapesDynamic)),
        ::testing::ValuesIn(filterCPUSpecificParams(cpuParams)),
        ::testing::ValuesIn(additionalConfig)),
    TopKLayerCPUTest::getTestCaseName);

std::vector<ov::test::InputShape> inputShapes_bubble_BLK_on_channel_horiz = {
    {{}, {{2, 2, 2, 2}}},
};

std::vector<ov::test::InputShape> inputShapesDynamic_bubble_BLK_on_channel_horiz = {
    {{2, {2, 3}, 2, 2}, {{2, 2, 2, 2}, {2, 3, 2, 2}}}
};

INSTANTIATE_TEST_CASE_P(smoke_TopK_bubble_BLK_on_channel_horiz, TopKLayerCPUTest,
    ::testing::Combine(
        ::testing::Combine(
            ::testing::Values(1),
            ::testing::Values(1),
            ::testing::ValuesIn(modes),
            ::testing::ValuesIn(sortTypes),
            ::testing::ValuesIn(netPrecisions),
            ::testing::Values(ElementType::undefined),
            ::testing::Values(ElementType::undefined),
            ::testing::ValuesIn(inputShapes_bubble_BLK_on_channel_horiz)),
        ::testing::Values(CPUSpecificParams({nChw16c, x}, {nChw16c, nChw16c}, {}, {})),
        ::testing::ValuesIn(additionalConfig)),
    TopKLayerCPUTest::getTestCaseName);

INSTANTIATE_TEST_CASE_P(smoke_TopK_bubble_BLK_on_channel_horiz_dynamic, TopKLayerCPUTest,
    ::testing::Combine(
        ::testing::Combine(
            ::testing::Values(1),
            ::testing::Values(1),
            ::testing::ValuesIn(modes),
            ::testing::ValuesIn(sortTypes),
            ::testing::ValuesIn(netPrecisions),
            ::testing::Values(ElementType::undefined),
            ::testing::Values(ElementType::undefined),
            ::testing::ValuesIn(inputShapesDynamic_bubble_BLK_on_channel_horiz)),
        ::testing::Values(CPUSpecificParams({nChw16c, x}, {nChw16c, nChw16c}, {}, {})),
        ::testing::ValuesIn(additionalConfig)),
    TopKLayerCPUTest::getTestCaseName);

} // namespace

} // namespace CPULayerTestsDefinitions
