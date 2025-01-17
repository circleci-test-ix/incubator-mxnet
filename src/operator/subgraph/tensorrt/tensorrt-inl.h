#ifndef MXNET_OPERATOR_SUBGRAPH_TENSORRT_TENSORRT_INL_H_
#define MXNET_OPERATOR_SUBGRAPH_TENSORRT_TENSORRT_INL_H_
/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * Copyright (c) 2019 by Contributors
 * \file tensorrt-inl.h
 * \brief TensorRT operation registration
 * \author Marek Kolodziej, Clement Fuji Tsang
*/

#if MXNET_USE_TENSORRT

#include <onnx-tensorrt/NvOnnxParser.h>

#include <utility>
#include <string>
#include <vector>

#include "../common.h"
#include "../subgraph_property.h"
#include "nnvm_to_onnx-inl.h"
#include "./onnx_to_tensorrt.h"

namespace mxnet {
namespace op {

using int64 = ::google::protobuf::int64;

struct TRTParam {
  std::unordered_map<std::string, uint32_t> inputs_to_idx;
  std::unordered_map<std::string, uint32_t> outputs_to_idx;
  std::unordered_map<std::string, NDArray> params_map;
};

struct TRTEngineParam {
  TRTEngineParam(nvinfer1::ICudaEngine* trt_engine,
                 nvonnxparser::IParser* _parser,
                 const std::unordered_map<std::string, uint32_t> input_map,
                 const std::unordered_map<std::string, uint32_t> output_map) {
    binding_order = std::make_shared<std::vector<std::pair<uint32_t, bool> > >();
    bindings = std::make_shared<std::vector<void*> >();
    binding_order->reserve(trt_engine->getNbBindings());
    bindings->resize(trt_engine->getNbBindings());
    for (int b = 0; b < trt_engine->getNbBindings(); ++b) {
      const std::string& binding_name = trt_engine->getBindingName(b);
      if (trt_engine->bindingIsInput(b)) {
        binding_order->emplace_back(input_map.at(binding_name), true);
      } else {
        binding_order->emplace_back(output_map.at(binding_name), false);
      }
    }
    trt_executor = trt_engine->createExecutionContext();
    trt_parser = _parser;
  }

  ~TRTEngineParam() {
    trt_parser->destroy();
    trt_executor->destroy();
  }
  nvinfer1::IExecutionContext* trt_executor;
  nvonnxparser::IParser* trt_parser;
  std::shared_ptr<std::vector<std::pair<uint32_t, bool> > > binding_order;
  std::shared_ptr<std::vector<void*> > bindings;
};

class TensorrtSelector : public SubgraphSelector {
 public:
  const std::unordered_set<std::string> unconditionalTRTops = {
    "BatchNorm",
    "clip",
    "Concat",
    "Convolution",
    "Deconvolution",
    "Dropout",
    "elemwise_add",
    "elemwise_sub",
    "elemwise_mul",
    "Flatten",
    "FullyConnected",
    "mean",
    "Pad",
    "relu",
    "rsqrt",
    "SoftmaxOutput"
  };

  const std::unordered_set<std::string> withWeightsOps = {
    "BatchNorm",
    "Convolution",
    "Deconvolution",
    "FullyConnected"
  };

  bool isTRTCompatible(const nnvm::Node &n) {
    const std::string op_name = n.op()->name;
    if (op_name == "Pooling") {
      return (n.attrs.dict.at("pool_type") == "avg" ||
          n.attrs.dict.at("pool_type") == "max");
    }

    if (unconditionalTRTops.count(op_name)) {
      return true;
    }

    if (op_name == "Activation") {
      return n.attrs.dict.at("act_type") == "relu" ||
        n.attrs.dict.at("act_type") == "tanh" ||
        n.attrs.dict.at("act_type") == "sigmoid";
    }

    return false;
  }

  bool Select(const nnvm::Node &n) override {
    return !n.is_variable() && isTRTCompatible(n);
  }

  bool SelectInput(const nnvm::Node &n, const nnvm::Node &new_node) override {
    if (new_node.is_variable()) {
      if (withWeightsOps.count(n.op()->name)) {
        return n.inputs[0].node->attrs.name != new_node.attrs.name;
      } else {
        return false;
      }
    }
    return isTRTCompatible(new_node);
  }

  bool SelectOutput(const nnvm::Node &n, const nnvm::Node &new_node) override {
    return isTRTCompatible(new_node);
  }

  std::vector<nnvm::Node*> Filter(const std::vector<nnvm::Node*>& candidates) override {
    bool found_one = false;
    // TensorRT is interesting with at least 2 operations
    for (auto& n : candidates) {
      if (!n->is_variable()) {
        if (found_one) {
          return candidates;
        } else {
          found_one = true;
        }
      }
    }
    return std::vector<nnvm::Node*>();
  }
};

class TensorrtProperty : public SubgraphProperty {
 public:
  static SubgraphPropertyPtr Create() {
    return std::make_shared<TensorrtProperty>();
  }

  nnvm::NodePtr CreateSubgraphNode(const nnvm::Symbol &sym,
                                   const int subgraph_id) const override {
    nnvm::NodePtr n = nnvm::Node::Create();
    nnvm::Symbol new_sym;
    std::unique_copy(sym.outputs.begin(), sym.outputs.end(),
        std::back_inserter(new_sym.outputs), [](
        nnvm::NodeEntry lhs, nnvm::NodeEntry rhs) {
          return lhs.index == rhs.index && lhs.node.get() == rhs.node.get();
        });
    n->attrs.name = "TensorRT" + std::to_string(subgraph_id);
    n->attrs.op = Op::Get("_TensorRT");
    CHECK(n->attrs.op);
    n->attrs.subgraphs.emplace_back(std::make_shared<nnvm::Symbol>(new_sym));
    std::ostringstream params_oss;
    for (auto &e : new_sym.ListInputNames(nnvm::Symbol::kAll)) {
      params_oss << e << ";";
    }
    auto tensorrt_params_names = params_oss.str();
    tensorrt_params_names.pop_back();
    n->attrs.dict["subgraph_params_names"] = tensorrt_params_names;
    TRTParam param;
    n->attrs.parsed = param;
    n->op()->attr_parser(&(n->attrs));
    return n;
  }

  SubgraphSelectorPtr CreateSubgraphSelector() const override {
    return std::make_shared<TensorrtSelector>();
  }

  void ConnectSubgraphOutputs(const nnvm::NodePtr subgraph_node, \
                              std::vector<nnvm::NodeEntry*>* output_entries) const override {
    std::vector<nnvm::NodeEntry>& outputs = subgraph_node->attrs.subgraphs[0]->outputs;
    TRTParam& _params = nnvm::get<TRTParam>(subgraph_node->attrs.parsed);
    for (size_t i = 0; i < outputs.size(); i++) {
      auto& o = outputs[i];
      for (auto& e : *output_entries) {
        if (o.index == e->index && o.node.get() == e->node.get()) {
          e->index = i;
          e->node = subgraph_node;
          // TODO(cfujitsang): For future support this would fail
          //                   if the node have multiple outputs
          _params.outputs_to_idx[o.node->attrs.name] = i;
        }
      }
    }
    subgraph_node->attrs.parsed = std::move(_params);
  }

  void ConnectSubgraphInputs(const nnvm::NodePtr subgraph_node,
                             std::vector<nnvm::NodeEntry*>* input_entries,
                             std::vector<nnvm::NodeEntry>* orig_input_entries) const override {
    TRTParam& _params = nnvm::get<TRTParam>(subgraph_node->attrs.parsed);
    subgraph_node->inputs.clear();
    subgraph_node->inputs.resize(orig_input_entries->size());
    for (size_t i = 0; i < orig_input_entries->size(); ++i) {
      subgraph_node->inputs[i] = orig_input_entries->at(i);
      _params.inputs_to_idx[input_entries->at(i)->node->attrs.name] = i;
    }
    subgraph_node->attrs.parsed = std::move(_params);
  }
};


}  // namespace op
}  // namespace mxnet

#endif  // MXNET_USE_TENSORRT

#endif  // MXNET_OPERATOR_SUBGRAPH_TENSORRT_TENSORRT_INL_H_
