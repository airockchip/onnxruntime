// Copyright 2020 rock-chips.com Inc.

#include <fstream>
#include <map>
#include <numeric>
#include <string>

#include "core/common/logging/logging.h"
#include "onnx_converter.h"
#include "node_attr_helper.h"

using std::string;
using std::vector;

namespace onnxruntime {

#define HAS(map, key) \
  (map.find(key) != map.end())

#define FORZ(var, end) \
  for (auto var = decltype(end){0}; var < end; var++)

#define ADD_SHAPE(name)                        \
  if (HAS(rk_tensors_, name)) {                \
    const auto& tensor = rk_tensors_.at(name); \
    shaper_.AddShape(name, tensor->GetDims()); \
  }

#define GET_ATTR(attr, name, type)                                                              \
  std::vector<type> attr;                                                                       \
  if (HAS(rk_tensors_, name)) {                                                                 \
    const auto& tensor = rk_tensors_.at(name);                                                  \
    const void* data = tensor->GetData();                                                       \
    rk::nn::PrecisionType fmt = tensor->GetPrecision();                                         \
    uint32_t dim = 1;                                                                           \
    if (tensor->GetDims().size() > 0)                                                           \
      dim = tensor->GetDims()[0];                                                               \
    for (uint32_t i = 0; i < dim; i++) {                                                        \
      if (fmt == rk::nn::PrecisionType::UINT8 || fmt == rk::nn::PrecisionType::INT8) {          \
        attr.push_back(((type*)data)[i]);                                                       \
      } else if (fmt == rk::nn::PrecisionType::INT32 || fmt == rk::nn::PrecisionType::UINT32) { \
        attr.push_back(((type*)data)[i]);                                                       \
      } else if (fmt == rk::nn::PrecisionType::INT64 || fmt == rk::nn::PrecisionType::UINT64) { \
        int64_t val = ((int64_t*)data)[i];                                                      \
        if (val > 0x7fffffff) val = 0x7fffffff;                                                 \
        attr.push_back((type)val);                                                              \
      } else if (fmt == rk::nn::PrecisionType::FLOAT32) {                                       \
        attr.push_back((type)((float*)data)[i]);                                                \
      } else {                                                                                  \
        LOGS_DEFAULT(FATAL) << "the format of " << name << " is not support!";                  \
      }                                                                                         \
    }                                                                                           \
  }

std::string OnnxConverter::m(const std::string& str) const {
  if (name_map_.find(str) != name_map_.end()) {
    return name_map_.at(str);
  }
  return str;
}

std::pair<std::pair<int, ONNX_NAMESPACE::NodeProto>, OnnxConverter::FuseCode>
OnnxConverter::FindActivation(const ONNX_NAMESPACE::ModelProto& model_proto,
                              const std::string& output_name) {
  std::pair<std::pair<int, ONNX_NAMESPACE::NodeProto>, FuseCode>
      activation{{}, FuseCode::FUSED_NONE};
  int i = 0;
  for (const auto& _node : model_proto.graph().node()) {
    if (!_node.input().empty() && output_name == _node.input(0) &&
        _node.op_type() == "Relu") {
      // If there are two branches after a conv/pool and both branches has
      // a relu on the top, we have to add two normal relu layers
      if (activation.second != FuseCode::FUSED_NONE) {
        return {{}, FuseCode::FUSED_NONE};
      }
      const auto node_pair = std::make_pair(i, _node);
      activation = std::make_pair(node_pair, FuseCode::FUSED_RELU);
    }
    i++;
  }
  if (activation.first.first != 0) {
    skipped_act_.push_back(activation.first.first);
    name_map_[activation.first.second.output(0)] = output_name;
  }
  return activation;
}

std::shared_ptr<rk::nn::Tensor>
OnnxConverter::CreateRknnTensor(const std::string name,
                                const std::vector<uint32_t> dims,
                                const void* data,
                                const rk::nn::TensorRole role,
                                const rk::nn::PrecisionType precision,
                                const rk::nn::DataLayoutType layout,
                                const rk::nn::QuantizationType qntType,
                                const uint8_t bits,
                                const float scale,
                                const uint32_t zero_point,
                                const int8_t fl) {
  auto attr = std::make_shared<rk::nn::TensorAttr>();
  attr->name = name;
  attr->dims = dims;
  attr->precision = precision;
  attr->layout = layout;
  attr->qntType = qntType;
  attr->role = role;
  attr->qntBits = bits;
  attr->qntParamDFP.fl.push_back(fl);
  attr->qntParamAffineAsymmetric.zero_point.push_back(zero_point);
  attr->qntParamAffineAsymmetric.scale.push_back(scale);
  attr->qntParamSymmetric.scale.push_back(scale);
  return graph_->CreateTensor(attr, (void*)data);
}

void OnnxConverter::HandleInitializer() {
  for (const auto& tensor : model_proto_.graph().initializer()) {
    const std::string name = tensor.name();
    std::vector<uint32_t> dims;
    for (auto dim : tensor.dims()) {
      dims.push_back(static_cast<uint32_t>(dim));
    }
    if (tensor.data_type() == ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
      const char* ptr = tensor.float_data().empty()
                            ? tensor.raw_data().data()
                            : reinterpret_cast<const char*>(
                                  tensor.float_data().data());
      rk_tensors_[name] = CreateRknnTensor(name, dims, ptr,
                                           rk::nn::TensorRole::CONST, rk::nn::PrecisionType::FLOAT32);
    } else if (tensor.data_type() ==
               ONNX_NAMESPACE::TensorProto_DataType_UINT8) {
      const char* ptr = tensor.int32_data().empty()
                            ? tensor.raw_data().data()
                            : reinterpret_cast<const char*>(
                                  tensor.int32_data().data());
      rk_tensors_[name] = CreateRknnTensor(name, dims, ptr,
                                           rk::nn::TensorRole::CONST, rk::nn::PrecisionType::UINT8);
    } else if (tensor.data_type() ==
               ONNX_NAMESPACE::TensorProto_DataType_INT32) {
      const char* ptr = tensor.int32_data().empty()
                            ? tensor.raw_data().data()
                            : reinterpret_cast<const char*>(
                                  tensor.int32_data().data());
      rk_tensors_[name] = CreateRknnTensor(name, dims, ptr,
                                           rk::nn::TensorRole::CONST, rk::nn::PrecisionType::INT32);
    } else if (tensor.data_type() ==
               ONNX_NAMESPACE::TensorProto_DataType_INT64) {
      const char* ptr = tensor.int64_data().empty()
                            ? tensor.raw_data().data()
                            : reinterpret_cast<const char*>(
                                  tensor.int64_data().data());
      rk_tensors_[name] = CreateRknnTensor(name, dims, ptr,
                                           rk::nn::TensorRole::CONST, rk::nn::PrecisionType::INT64);
    } else {
      LOGS_DEFAULT(FATAL) << "tensor name = " << tensor.name() << ", data_type = "
                          << tensor.data_type() << " is not support!";
      assert(0);
    }
    operands_.push_back(name);
  }
}

std::vector<std::shared_ptr<rk::nn::Tensor>> OnnxConverter::GetInputOfOnnxModel(
    std::vector<const void*> input_bufs, std::unordered_map<std::string, int> input_maps) {
  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs;

  for (const auto& input : model_proto_.graph().input()) {
    if (std::find(operands_.begin(), operands_.end(), input.name()) !=
        operands_.end()) {
      continue;
    }

    Shaper::Shape shape;
    for (const auto& dim : input.type().tensor_type().shape().dim()) {
      if (dim.value_case() ==
          ONNX_NAMESPACE::TensorShapeProto_Dimension::kDimValue) {
        shape.push_back(static_cast<uint32_t>(dim.dim_value()));
      } else {
        throw std::invalid_argument(
            "The input of graph doesn't have dim_value");
      }
    }
    shaper_.AddShape(input.name(), shape);

    const void* ptr = NULL;
    auto iter = input_maps.find(input.name());
    if (iter != input_maps.end()) {
      if (iter->second < (int)input_bufs.size())
        ptr = input_bufs[iter->second];
    }

    rk::nn::PrecisionType type = rk::nn::PrecisionType::FLOAT32;
    if (input.type().tensor_type().has_elem_type()) {
      switch (input.type().tensor_type().elem_type()) {
        case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
          type = rk::nn::PrecisionType::FLOAT32;
          break;
        case ONNX_NAMESPACE::TensorProto_DataType_UINT8:
          type = rk::nn::PrecisionType::UINT8;
          break;
        case ONNX_NAMESPACE::TensorProto_DataType_INT32:
          type = rk::nn::PrecisionType::INT32;
          break;
        case ONNX_NAMESPACE::TensorProto_DataType_INT64:
          type = rk::nn::PrecisionType::INT64;
          break;
        default:
          // TODO
          throw std::invalid_argument(
              "The input of graph doesn't have valid type");
      }
    }

    auto rk_input = CreateRknnTensor(input.name(), shape, ptr, rk::nn::TensorRole::DATA, type);

    inputs.push_back(rk_input);

    rk_tensors_[input.name()] = rk_input;
  }
  if (inputs.empty())
    throw std::runtime_error("GetInputOfOnnxModel fail!");
  return inputs;
}

std::vector<std::shared_ptr<rk::nn::Tensor>> OnnxConverter::GetOutputOfOnnxModel() {
  std::vector<std::shared_ptr<rk::nn::Tensor>> outputs;
  for (const auto& output : model_proto_.graph().output()) {
    auto name = m(output.name());
    if (HAS(rk_tensors_, name)) {
      outputs.push_back(rk_tensors_[name]);
    }
  }
  if (outputs.empty())
    throw std::runtime_error("GetOutputOfOnnxModel fail!");
  return outputs;
}

Shaper::Shape GetShape(const ONNX_NAMESPACE::ModelProto& model_proto,
                       const std::map<std::string, std::vector<uint32_t>>& tensor_dims, const std::string& name) {
  Shaper::Shape shape;
  for (const auto& value_info : model_proto.graph().value_info()) {
    if (value_info.name() == name) {
      if (!value_info.has_type()) {
        break;
      } else if (!value_info.type().has_tensor_type()) {
        break;
      } else if (!value_info.type().tensor_type().has_shape()) {
        break;
      } else if (value_info.type().tensor_type().shape().dim_size() == 0) {
        break;
      }

      for (const auto& dim : value_info.type().tensor_type().shape().dim()) {
        if (dim.has_dim_value()) {
          shape.push_back(dim.dim_value());
        } else {
          break;
        }
      }

      return shape;
    }
  }

  if (HAS(tensor_dims, name)) {
    const auto& dims = tensor_dims.at(name);
    if (dims.size() > 0)
      shape = dims;
  }

  return shape;
}

bool TypeSupport(const ONNX_NAMESPACE::ModelProto& model_proto, const std::string& name) {
  for (const auto& value_info : model_proto.graph().value_info()) {
    if (value_info.name() == name) {
      if (!value_info.has_type()) {
        break;
      } else if (!value_info.type().has_tensor_type()) {
        break;
      } else if (!value_info.type().tensor_type().has_elem_type()) {
        break;
      }

      switch (value_info.type().tensor_type().elem_type()) {
        case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
          return true;
        case ONNX_NAMESPACE::TensorProto_DataType_UINT8:
          return true;
        case ONNX_NAMESPACE::TensorProto_DataType_INT32:
          return true;
        case ONNX_NAMESPACE::TensorProto_DataType_INT64:
          return false;
        default:
          break;
      }

      return false;
    }
  }

  // if can't find, it's considered support.
  return true;
}

std::pair<bool, std::string> OnnxConverter::IsNodeSupported(
    const ONNX_NAMESPACE::ModelProto& model_proto,
    const ONNX_NAMESPACE::NodeProto& node) const {
  NodeAttrHelper helper(node);
  const auto& op = node.op_type();
  const std::vector<std::string> supported_types{
      "Conv", "Relu", "Clip", "LeakyRelu",
      "MaxPool", "AveragePool", "GlobalAveragePool",
      "Concat", "Softmax", "BatchNormalization", "Gemm",
      "Add", "Mul", "Sub",
      "Reshape", "Squeeze", "Unsqueeze",
      "Flatten", "Transpose", /*"Gather", "Slice",*/
      "QLinearConv", /*"QuantizeLinear",*/ "DequantizeLinear"};
  if (std::find(supported_types.begin(), supported_types.end(), op) ==
      supported_types.end()) {
    return {false, "Unsupported operator"};
  }

  if (!TypeSupport(model_proto, node.input(0))) {
    return {false, "Type of input(" + node.input(0) + ") is unsupported"};
  }

  if (op == "Conv") {
    const auto strides = helper.get("strides", vector<int>{1, 1});
    const auto pads = helper.get("pads", vector<int>{0, 0, 0, 0});
    const auto dilations = helper.get("dilations", vector<int>{1, 1});
    const auto group = helper.get("group", 1);
    if (dilations != vector<int>{1, 1} && strides != vector<int>{1, 1}) {
      return {false, "Both dilations and strides > 1 is not supported for now"};
    }
    const auto weight_name = m(node.input(1));
    if (HAS(tensor_dims_, weight_name)) {
      const auto& dims = tensor_dims_.at(weight_name);
      if (group != 1 && dims[1] != 1) {
        return {false, "group != 1 is not supported"};
      }
      if (dims.size() != 4) {
        return {false, "Only conv 2d is supported."};
      }
    } else {
      return {false, "The weight of convolution must be known"};
    }
  } else if (op == "AveragePool" || op == "MaxPool") {
    const auto count_include_pad = helper.get("count_include_pad", 0);
    if (count_include_pad == 1) {
      return {false, "count_include_pad == 1 is not supported"};
    }
    const auto storage_order = helper.get("storage_order", 0);
    if (storage_order == 1) {
      return {false, "storage_order == 1 is not supported"};
    }
    if (helper.get("auto_pad", "NOTSET") != "NOTSET") {
      return {false, "auto_pad is not supported"};
    }
    if (helper.get("kernel_shape", std::vector<int>{1, 1}).size() != 2) {
      return {false, "Only pooling 2d is supported"};
    }
    if (helper.get("ceil_mode", 0) == 1) {
      return {false, "ceil_mode == 1 is not supported for pooling"};
    }
    if (helper.get("dilations", std::vector<int>{1, 1}) !=
        std::vector<int>{1, 1}) {
      return {false, "Dilations of pooling is not supported"};
    }
    if (node.output_size() != 1) {
      return {false, "Argmax in maxpooling is not supported"};
    }
  } else if (op == "GlobalAveragePool" || op == "GlobalMaxPool") {
    const auto& input_shape = GetShape(model_proto, tensor_dims_, node.input(0));
    if (input_shape.size() == 0 || input_shape.size() != 4) {
      return {false, "Only rank-4 tensor is supported"};
    }
  } else if (op == "PRelu") {
    const auto slope_name = m(node.input(1));
    if (HAS(tensor_dims_, slope_name)) {
      if (tensor_dims_.at(slope_name) != Shaper::Shape{1}) {
        // TODO: support it
        return {false, "PRelu only support one element slope."};
      }
    } else {
      return {false, "PRelu slope must be known"};
    }
  } else if (op == "Gemm") {
    const auto transA = helper.get("transA", 0);
    const auto transB = helper.get("transB", 0);
    const auto alpha = helper.get("alpha", 1.0f);
    const auto beta = helper.get("beta", 1.0f);
    if (!(transA == 0 && transB == 1 && alpha == 1.f && beta == 1.f)) {
      return {false,
              "Only transA == 0, transB == 1, alpha == 1.0 and beta == "
              "1.0 is supported."};
    }
  } else if (op == "BatchNormalization") {
    if (node.output_size() != 1) {
      return {false,
              "Your onnx model may be in training mode, please export "
              "it in test mode."};
    }
    const auto scale_name = m(node.input(1));
    const auto b_name = m(node.input(2));
    const auto mean_name = m(node.input(3));
    const auto var_name = m(node.input(4));
    if (!HAS(tensor_dims_, scale_name)) {
      return {false, "Scale of BN must be known"};
    }
    if (!HAS(tensor_dims_, b_name)) {
      return {false, "B of BN must be known"};
    }
    if (!HAS(tensor_dims_, mean_name)) {
      return {false, "Mean of BN must be known"};
    }
    if (!HAS(tensor_dims_, var_name)) {
      return {false, "Var of BN must be known"};
    }
  } else if (op == "LRN") {
    const auto size = helper.get("size", 1);
    if (size % 2 == 0) {
      return {false, "NNAPI only support odd size for LRN"};
    }
  } else if (op == "Reshape") {
    const auto output_name = node.output(0);
    for (const auto another_node : model_proto_.graph().node()) {
      for (const auto input_name : another_node.input()) {
        if (input_name == output_name &&
            another_node.op_type() != "Gemm") {
          return {false,
                  "Reshape can only be the last layer or precede a "
                  "gemm layer for now"};
        }
      }
    }
    const auto& shape = GetShape(model_proto, tensor_dims_, node.input(1));
    int rank = shape.size();
    if (shape.size() != 1) {
      return {false, "Wrong shape rank"};
    }
    if (shape[0] <= 1) {  // rknpu doesn't support dims of shape equal 1,
                          // but simulator support, why? (reproduce on "Reshape_699" of ssd.onnx)
      return {false, "Only shape dims > 1 is supported"};
    }
  } else if (op == "Softmax") {
    const auto axis = helper.get("axis", 1);
    if (axis != 1) {
      return {false, "Only axis == 1 is supported"};
    }
  } else if (op == "Flatten") {
    const auto axis = helper.get("axis", 1);
    const auto& input_shape = GetShape(model_proto, tensor_dims_, node.input(0));
    int rank = input_shape.size();
    if (rank != 0) {
      if (axis < 0 || axis > (int64_t)rank) {
        return {false, "Only axis <= rank of input is supported"};
      }
    }
  } else if (op == "Squeeze") {
    const auto& input_shape = GetShape(model_proto, tensor_dims_, node.input(0));
    const auto axes = helper.get("axes", std::vector<int>{});
    int rank = input_shape.size();
    if (rank != 0) {
      for (auto axis : axes) {
        if (axis >= rank || axis < -1) {
          return {false, "Only axes <= rank of input is supported"};
        } else {
          axis = (axis < 0) ? (axis + rank) : (axis);
          if (input_shape[axis] != 1) {
            return {false, "the input_shape[axis] must equal one"};
          }
        }
      }
    }
  } else if (op == "Unsqueeze") {
    const auto& input_shape = GetShape(model_proto, tensor_dims_, node.input(0));
    const auto axes = helper.get("axes", std::vector<int>{});
    int rank = input_shape.size();
    if (rank != 0) {
      for (auto axis : axes) {
        if (axis < 0) {
          return {false, "Only axes >= 0 is supported"};
        } else if (axis >= (int)(rank + axes.size())) {
          return {false, "Only axes <= rank of (input + axes) is supported"};
        }
      }
    }
  } else if (op == "Gather") {
    const auto& input_shape = GetShape(model_proto, tensor_dims_, node.input(0));
    const auto axis = helper.get("axis", 1);
    int rank = input_shape.size();
    if (rank != 0 && (axis >= rank || axis < -rank)) {
      return {false, "Only axis <= rank of input is supported"};
    }
  } else if (op == "Concat") {
    const auto& input_shape = GetShape(model_proto, tensor_dims_, node.input(0));
    const auto axis = helper.get("axis", 1);
    int rank = input_shape.size();
    if (axis >= 4) {
      return {false, "Only axis <= 4 of input is supported"};
    }
    if (rank != 0 && rank > 4 && (axis >= rank || axis < -rank)) {
      if (rank > 4)
        return {false, "Only rank <= 4 of input is supported"};
      else
        return {false, "Only axis <= rank of input is supported"};
    }
  } else if (op == "Add") {
    if (!TypeSupport(model_proto, node.input(1))) {
      return {false, "Type of input1(" + node.input(1) + ") is unsupported"};
    }
  } else if (op == "Mul") {
    if (!TypeSupport(model_proto, node.input(1))) {
      return {false, "Type of input1(" + node.input(1) + ") is unsupported"};
    }
  }

  return {true, ""};
}

std::vector<std::vector<int>> OnnxConverter::GetSupportedNodes(
    ONNX_NAMESPACE::ModelProto model_proto) {
  for (const auto& tensor : model_proto.graph().initializer()) {
    const std::string name = tensor.name();
    std::vector<uint32_t> dims;
    for (auto dim : tensor.dims()) {
      dims.push_back(static_cast<uint32_t>(dim));
    }
    tensor_dims_[name] = dims;
  }

  std::vector<std::vector<int>> supported_node_vecs;
  std::vector<int> supported_node_vec;
  bool force_unsupport = false;
  for (int i = 0; i < model_proto.graph().node_size(); i++) {
    bool supported;
    std::string error_msg;
    if (force_unsupport) {
      const auto& op = model_proto.graph().node(i).op_type();
      LOGS_DEFAULT(INFO) << op << ": Force unsupported operator";
      continue;
    }
    std::tie(supported, error_msg) =
        IsNodeSupported(model_proto, model_proto.graph().node(i));
    if (supported) {
      supported_node_vec.push_back(i);
    } else {
      const auto& op = model_proto.graph().node(i).op_type();
      LOGS_DEFAULT(INFO) << op << ": " << error_msg;
      if (!supported_node_vec.empty()) {
        // if (supported_node_vec.size() >= 2)
          supported_node_vecs.push_back(supported_node_vec);
        supported_node_vec.clear();
      }
      // if (op == "NonMaxSuppression" || op == "TopK")
      //   force_unsupport = true;
    }
  }
  if (!supported_node_vec.empty()) {
    // if (supported_node_vec.size() >= 2)
      supported_node_vecs.push_back(supported_node_vec);
  }

  for (auto it = tensor_dims_.begin(); it != tensor_dims_.end(); ++it) {
    it->second.clear();
    tensor_dims_.erase(it);
  }
  return supported_node_vecs;
}

void OnnxConverter::Convert(const ONNX_NAMESPACE::ModelProto& model_proto, rk::nn::Graph* graph,
                            std::vector<const void*> input_bufs, std::unordered_map<std::string, int> input_maps) {
  model_proto_ = model_proto;
  graph_ = graph;

  HandleInitializer();

  const auto inputs = GetInputOfOnnxModel(input_bufs, input_maps);

  for (int i = 0; i < model_proto_.graph().node_size(); i++) {
    const auto& node = model_proto_.graph().node(i);
    NodeAttrHelper helper(node);
    const auto& op = node.op_type();
    LOGS_DEFAULT(VERBOSE) << "----------- Node=" << node.name() << ", op=" << op << "-----------";
    if (std::find(skipped_act_.begin(), skipped_act_.end(), i) !=
        skipped_act_.end()) {
      LOGS_DEFAULT(INFO) << "Skip layer " << node.name() << ", op=" << op;
      continue;
    }

    if (op == "Conv") {
      const auto strides = helper.get("strides", vector<int>{1, 1});
      const auto pads = helper.get("pads", vector<int>{0, 0, 0, 0});
      const auto dilations = helper.get("dilations", vector<int>{1, 1});
      const auto group = helper.get("group", 1);
      std::string bias_name;
      if (node.input_size() >= 3) {
        bias_name = m(node.input(2));
      }
      const auto auto_pad = helper.get("auto_pad", "NOTSET");

      const auto ori_weight_name = m(node.input(1));
      AddConv(m(node.input(0)), strides, pads, dilations, group,
              ori_weight_name, bias_name, auto_pad, m(node.output(0)));
    } else if (op == "QLinearConv") {
      const auto strides = helper.get("strides", vector<int>{1, 1});
      const auto pads = helper.get("pads", vector<int>{0, 0, 0, 0});
      const auto dilations = helper.get("dilations", vector<int>{1, 1});
      const auto group = helper.get("group", 1);
      const auto auto_pad = helper.get("auto_pad", "NOTSET");
      std::string bias_name;
      if (node.input_size() >= 9) {
        bias_name = m(node.input(8));
      }
      AddQLinearConv(m(node.input(0)), m(node.input(1)), m(node.input(2)),
                     strides, pads, dilations, group, auto_pad,
                     m(node.input(3)), m(node.input(4)), m(node.input(5)),
                     bias_name, m(node.output(0)), m(node.input(6)), m(node.input(7)));
    } else if (op == "AveragePool" || op == "MaxPool" ||
               op == "GlobalAveragePool" || op == "GlobalMaxPool") {
      const auto input_name = m(node.input(0));
      const auto output_name = m(node.output(0));
      vector<int> strides, pads, kernel_shape;
      int ceil_mode;
      if (op == "AveragePool" || op == "MaxPool") {
        strides = helper.get("strides", vector<int>{1, 1});
        pads = helper.get("pads", vector<int>{0, 0, 0, 0});
        kernel_shape = helper.get("kernel_shape", vector<int>{0, 0});
        ceil_mode = helper.get("ceil_mode", 0);
        const auto count_include_pad =
            helper.get("count_include_pad", 0);
        if (count_include_pad == 1) {
          throw std::invalid_argument(
              "count_include_pad == 1 is not supported");
        }
        const auto storage_order = helper.get("storage_order", 0);
        if (storage_order == 1) {
          throw std::invalid_argument(
              "storage_order == 1 is not supported");
        }
        if (helper.get("auto_pad", "NOTSET") != "NOTSET") {
          throw std::invalid_argument("auto_pad is not supported");
        }
      } else {
        strides = {0, 0};
        pads = {0, 0, 0, 0};
        kernel_shape = {-1, -1};  // -1 for global
        ceil_mode = 0;
      }
      AddLayerPool(op, input_name, kernel_shape, pads, strides, ceil_mode,
                   output_name);
    } else if (op == "Relu") {
      const auto input_name = m(node.input(0));
      const auto output_name = m(node.output(0));
      AddLayerReLU(input_name, output_name);
    } else if (op == "PRelu") {
    } else if (op == "Add") {
      const auto input1_name = m(node.input(0));
      const auto input2_name = m(node.input(1));
      const auto output_name = m(node.output(0));
      AddLayerAdd(input1_name, input2_name, output_name);
    } else if (op == "Sub") {
      const auto input1_name = m(node.input(0));
      const auto input2_name = m(node.input(1));
      const auto output_name = m(node.output(0));
      AddLayerSub(input1_name, input2_name, output_name);
    } else if (op == "Mul") {
      const auto input1_name = m(node.input(0));
      const auto input2_name = m(node.input(1));
      const auto output_name = m(node.output(0));
      AddLayerMul(input1_name, input2_name, output_name);
    } else if (op == "Gemm") {
      const auto input_name = m(node.input(0));
      const auto weight_name = m(node.input(1));
      const auto output_name = m(node.output(0));
      string bias_name;
      if (node.input_size() >= 3) {
        bias_name = m(node.input(2));
      }
      const auto transA = helper.get("transA", 0);
      const auto transB = helper.get("transB", 0);
      const auto alpha = helper.get("alpha", 1.0f);
      const auto beta = helper.get("beta", 1.0f);
      if (transA == 0 && transB == 1 && alpha == 1.f && beta == 1.f) {
        AddLayerFC(input_name, weight_name, bias_name, output_name);
      } else {
        throw std::invalid_argument(
            "Only transA == 0, transB == 1, alpha == 1.0 and beta == "
            "1.0 is "
            "supported.");
      }
    } else if (op == "Softmax") {
      const auto input_name = m(node.input(0));
      const auto output_name = m(node.output(0));
      AddLayerSoftmax(input_name, output_name);
    } else if (op == "Concat") {
      vector<std::string> concat_inputs_str;
      for (const auto& onnx_input : node.input()) {
        concat_inputs_str.push_back(m(onnx_input));
      }
      const auto axis = helper.get("axis", 1);
      const auto output_name = m(node.output(0));
      AddLayerConcat(concat_inputs_str, axis, output_name);
    } else if (op == "Dropout") {
    } else if (op == "BatchNormalization") {
      const auto input_name = m(node.input(0));
      const auto scale_name = m(node.input(1));
      const auto bias_name = m(node.input(2));
      const auto mean_name = m(node.input(3));
      const auto var_name = m(node.input(4));
      const auto eps = helper.get("epsilon", 1e-5f);
      const auto output_name = m(node.output(0));
      AddLayerBatchNorm(input_name, scale_name, bias_name, mean_name,
                        var_name, eps, output_name);
    } else if (op == "Reshape") {
      const auto input_name = m(node.input(0));
      const auto shape_name = m(node.input(1));
      const auto output_name = m(node.output(0));
      AddLayerReshape(input_name, shape_name, output_name);
    } else if (op == "LRN") {
    } else if (op == "Tanh") {
    } else if (op == "Floor") {
    } else if (op == "Sigmoid") {
    } else if (op == "Flatten") {
      const auto input_name = m(node.input(0));
      const auto axis = helper.get("axis", 1);
      const auto output_name = m(node.output(0));
      AddLayerFlatten(input_name, axis, output_name);
    } else if (op == "Transpose") {
      const auto input_name = m(node.input(0));
      const auto perm = helper.get("perm", vector<int>{0, 1, 2});
      const auto output_name = m(node.output(0));
      AddLayerTranspose(input_name, perm, output_name);
    } else if (op == "Slice") {
      const auto input_name = m(node.input(0));
      const auto starts_name = m(node.input(1));
      const auto ends_name = m(node.input(2));
      const auto axes_name = m(node.input(3));
      const auto steps_name = m(node.input(4));
      const auto output_name = m(node.output(0));
      AddLayerSlice(input_name, starts_name, ends_name, axes_name,
                    steps_name, output_name);
    } else if (op == "Squeeze") {
      const auto input_name = m(node.input(0));
      const auto axes = helper.get("axes", std::vector<int>{});
      const auto output_name = m(node.output(0));
      AddLayerSqueeze(input_name, axes, output_name);
    } else if (op == "Unsqueeze") {
      const auto input_name = m(node.input(0));
      const auto axes = helper.get("axes", std::vector<int>{});
      const auto output_name = m(node.output(0));
      AddLayerUnsqueeze(input_name, axes, output_name);
    } else if (op == "Gather") {
      const auto input_name = m(node.input(0));
      const auto indices_name = m(node.input(1));
      const auto axis = helper.get("axis", 0);
      const auto output_name = m(node.output(0));
      AddLayerGather(input_name, indices_name, axis, output_name);
    } else if (op == "LeakyRelu") {
      const auto input_name = m(node.input(0));
      const auto alpha = helper.get("alpha", (float)0.0);
      const auto output_name = m(node.output(0));
      AddLayerLeakyRelu(input_name, alpha, output_name);
    } else if (op == "Clip") {
      const auto input_name = m(node.input(0));
      const auto min = helper.get("axis", 0);
      const auto max = helper.get("axis", 6);
      const auto output_name = m(node.output(0));
      AddLayerClip(input_name, min, max, output_name);
    } else if (op == "QuantizeLinear") {
      const auto input_name = m(node.input(0));
      const auto output_scale = m(node.input(1));
      const auto output_zp = m(node.input(2));
      const auto output_name = m(node.output(0));
      AddLayerQuantizeLinear(input_name, output_scale, output_zp, output_name);
    } else if (op == "DequantizeLinear") {
      const auto input_name = m(node.input(0));
      const auto input_scale = m(node.input(1));
      const auto input_zp = m(node.input(2));
      const auto output_name = m(node.output(0));
      AddLayerDequantizeLinear(input_name, input_scale, input_zp, output_name);
    } else {
      throw std::invalid_argument("Unsupported operator " + op);
    }
  }

  const auto outputs = GetOutputOfOnnxModel();

  graph_->SetInputsOutputs(inputs, outputs);
}

void OnnxConverter::Clear() {
  skipped_act_.clear();
  operands_.clear();
  name_map_.clear();
  rk_tensors_.clear();
  shaper_.Clear();

  for (auto p : free_list_) {
    if (p) free(p);
  }
  free_list_.clear();
}

void OnnxConverter::AddConv(const string& input_name,
                            const std::vector<int>& strides,
                            const std::vector<int>& pads,
                            const std::vector<int>& dilations, int group,
                            const string& ori_weight_name,
                            const string& bias_name,
                            const string& auto_pad,
                            const string& output_name) {
  if (dilations != vector<int>{1, 1}) {
    // TODO
    throw std::invalid_argument("dilations != 1 is not supported yet");
  }

  if (!HAS(rk_tensors_, ori_weight_name)) {
    throw std::invalid_argument("The weight of convolution must be known");
  }
  const auto& weight = rk_tensors_.at(ori_weight_name);
  if (group == 1) {
    LOGS_DEFAULT(VERBOSE) << "Vanilla conv";
    AddLayerConvImpl(input_name, ori_weight_name, bias_name, pads, strides, 1, auto_pad,
                     output_name);
  } else if (weight->GetDims()[1] == 1) {  // depthwise
    LOGS_DEFAULT(VERBOSE) << "Depthwise conv";
    AddLayerDepthwiseConvImpl(input_name, ori_weight_name, bias_name, pads,
                              strides, weight->GetDims()[0] / group, group,
                              output_name);
  } else {
    LOGS_DEFAULT(VERBOSE) << "Group conv";
    AddLayerConvImpl(input_name, ori_weight_name, bias_name, pads, strides, group, auto_pad,
                     output_name);
  }
}

void OnnxConverter::AddQLinearConv(const string& input_name,
                                   const string& input_scale_name,
                                   const string& input_zp_name,
                                   const std::vector<int>& strides,
                                   const std::vector<int>& pads,
                                   const std::vector<int>& dilations, int group,
                                   const string& auto_pad,
                                   const string& weight_name,
                                   const string& weight_scale_name,
                                   const string& weight_zp_name,
                                   const string& bias_name,
                                   const string& output_name,
                                   const string& output_scale_name,
                                   const string& output_zp_name) {
  if (dilations != vector<int>{1, 1}) {
    return;
  }

  if (!HAS(rk_tensors_, weight_name)) {
    throw std::invalid_argument("The weight of convolution must be known");
  }
  const auto& weight = rk_tensors_.at(weight_name);
  if (group == 1) {
    LOGS_DEFAULT(VERBOSE) << "Vanilla QLinearConv";
    AddLayerQLinearConvImpl(input_name, input_scale_name, input_zp_name,
                            weight_name, weight_scale_name, weight_zp_name,
                            bias_name, pads, strides, 1, auto_pad,
                            output_name, output_scale_name, output_zp_name);
  } else if (weight->GetDims()[1] == 1) {  // depthwise
    LOGS_DEFAULT(VERBOSE) << "Depthwise QLinearConv";
    // TODO
    throw std::invalid_argument("Depthwise QLinearConv is not supported yet");
  } else {
    LOGS_DEFAULT(VERBOSE) << "Group QLinearConv";
    AddLayerQLinearConvImpl(input_name, input_scale_name, input_zp_name,
                            weight_name, weight_scale_name, weight_zp_name,
                            bias_name, pads, strides, group, auto_pad,
                            output_name, output_scale_name, output_zp_name);
  }
}

void OnnxConverter::AddLayerPool(const std::string& op, const std::string& input_name,
                                 const std::vector<int>& kernel_shape,
                                 const std::vector<int>& pads,
                                 const std::vector<int>& strides,
                                 const int32_t ceil_mode,
                                 const std::string& output_name) {
  if (op == "AveragePool" || op == "GlobalAveragePool") {
    AddLayerAvePoolImpl(input_name, kernel_shape, pads, strides, ceil_mode, output_name);
  } else {
    AddLayerMaxPoolImpl(input_name, kernel_shape, pads, strides, ceil_mode, output_name);
  }
}

void OnnxConverter::AddLayerConvImpl(const std::string& input,
                                     const std::string& weight,
                                     const std::string& bias,
                                     const std::vector<int32_t>& pads,
                                     const std::vector<int32_t>& strides,
                                     const int32_t group,
                                     const std::string& auto_pad,
                                     const std::string& output) {
  const auto activation = FindActivation(model_proto_, output);

  ADD_SHAPE(input);
  ADD_SHAPE(weight);
  if (bias != "") {
    ADD_SHAPE(bias);
  }

  shaper_.Conv(m(input), m(weight), pads, strides, auto_pad, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  std::vector<uint32_t> weight_dims;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, weight)) {
    weight_dims = rk_tensors_.at(weight)->GetDims();
    inputs.push_back(rk_tensors_.at(weight));
  }
  if (bias != "") {
    if (HAS(rk_tensors_, bias)) {
      inputs.push_back(rk_tensors_.at(bias));
    }
  } else {
    uint32_t dim = shaper_[weight][0];
    void* ptr = (void*)malloc(sizeof(float) * dim);
    memset(ptr, 0, sizeof(float) * dim);
    free_list_.push_back(ptr);

    std::vector<uint32_t> dims = {dim};
    auto rk_bias = CreateRknnTensor(bias, dims, ptr, rk::nn::TensorRole::CONST);
    inputs.push_back(rk_bias);
  }

  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::PadType pad_type = rk::nn::PadType::AUTO;
  if (auto_pad == "NOTSET") {
    pad_type = rk::nn::PadType::AUTO;
  } else if (auto_pad == "SAME_UPPER" || auto_pad == "SAME_LOWER") {
    pad_type = rk::nn::PadType::SAME;
  } else if (auto_pad == "VALID") {
    pad_type = rk::nn::PadType::VALID;
  }

  rk::nn::Conv2DAttr attr;
  attr.ksize[0] = weight_dims[3];
  attr.ksize[1] = weight_dims[2];
  attr.stride[0] = strides[1];
  attr.stride[1] = strides[0];
  attr.pad[0] = pads[1];
  attr.pad[1] = pads[3];
  attr.pad[2] = pads[0];
  attr.pad[3] = pads[2];
  attr.group = group;
  attr.weights = weight_dims[0];  // TODO
  attr.dilation[0] = 1;
  attr.dilation[1] = 1;
  attr.pad_type = pad_type;
  attr.multiplier = 0;  // TODO
  attr.has_relu = (activation.second == FuseCode::FUSED_RELU) ? true : false;

  graph_->AddOperator(rk::nn::OperatorType::CONV2D, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerQLinearConvImpl(const string& input,
                                            const string& input_scale,
                                            const string& input_zp,
                                            const string& weight,
                                            const string& weight_scale,
                                            const string& weight_zp,
                                            const string& bias,
                                            const std::vector<int>& pads,
                                            const std::vector<int>& strides,
                                            int group,
                                            const string& auto_pad,
                                            const string& output,
                                            const string& output_scale,
                                            const string& output_zp) {
  const auto activation = FindActivation(model_proto_, output);

  ADD_SHAPE(input);
  ADD_SHAPE(weight);
  if (bias != "") {
    ADD_SHAPE(bias);
  }

  shaper_.Conv(m(input), m(weight), pads, strides, auto_pad, output);

  GET_ATTR(in_s, input_scale, float);
  GET_ATTR(in_zp, input_zp, uint8_t);
  GET_ATTR(w_s, weight_scale, float);
  GET_ATTR(w_zp, weight_zp, uint8_t);
  GET_ATTR(out_s, output_scale, float);
  GET_ATTR(out_zp, output_zp, uint8_t);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  std::vector<uint32_t> weight_dims;
  if (HAS(rk_tensors_, input)) {
    rk::nn::QuantizationParamAffineAsymmetric param;
    param.scale.push_back(in_s[0]);
    param.zero_point.push_back(in_zp[0]);
    auto tensor = rk_tensors_.at(input);
    tensor->SetQntParam(rk::nn::QuantizationType::AFFINE_ASYMMETRIC, 8, param);
    inputs.push_back(tensor);
  }
  if (HAS(rk_tensors_, weight)) {
    rk::nn::QuantizationParamAffineAsymmetric param;
    param.scale.push_back(w_s[0]);
    param.zero_point.push_back(w_zp[0]);
    weight_dims = rk_tensors_.at(weight)->GetDims();
    auto tensor = rk_tensors_.at(weight);
    tensor->SetQntParam(rk::nn::QuantizationType::AFFINE_ASYMMETRIC, 8, param);
    inputs.push_back(tensor);
  }
  if (bias != "") {
    if (HAS(rk_tensors_, bias)) {
      rk::nn::QuantizationParamSymmetric param;
      param.scale.push_back(in_s[0] * w_s[0]);
      auto tensor = rk_tensors_.at(bias);
      tensor->SetQntParam(rk::nn::QuantizationType::SYMMETRIC, 32, param);
      inputs.push_back(tensor);
    }
  } else {
    uint32_t dim = shaper_[weight][0];
    void* ptr = (void*)malloc(sizeof(int32_t) * dim);
    memset(ptr, 0, sizeof(int32_t) * dim);
    free_list_.push_back(ptr);

    std::vector<uint32_t> dims = {dim};
    auto rk_bias = CreateRknnTensor(bias, dims, ptr, rk::nn::TensorRole::CONST, rk::nn::PrecisionType::INT32,
                                    rk::nn::DataLayoutType::NCHW, rk::nn::QuantizationType::SYMMETRIC, 32, in_s[0] * w_s[0]);
    inputs.push_back(rk_bias);
  }

  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output], NULL, rk::nn::TensorRole::VAR,
                                      inputs[0]->GetPrecision(), rk::nn::DataLayoutType::NCHW,
                                      rk::nn::QuantizationType::AFFINE_ASYMMETRIC, 8, out_s[0], out_zp[0]);
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::PadType pad_type = rk::nn::PadType::AUTO;
  if (auto_pad == "NOTSET") {
    pad_type = rk::nn::PadType::AUTO;
  } else if (auto_pad == "SAME_UPPER" || auto_pad == "SAME_LOWER") {
    pad_type = rk::nn::PadType::SAME;
  } else if (auto_pad == "VALID") {
    pad_type = rk::nn::PadType::VALID;
  }

  rk::nn::Conv2DAttr attr;
  attr.ksize[0] = weight_dims[3];
  attr.ksize[1] = weight_dims[2];
  attr.stride[0] = strides[1];
  attr.stride[1] = strides[0];
  attr.pad[0] = pads[1];
  attr.pad[1] = pads[3];
  attr.pad[2] = pads[0];
  attr.pad[3] = pads[2];
  attr.group = group;
  attr.weights = weight_dims[0];  // TODO
  attr.dilation[0] = 1;
  attr.dilation[1] = 1;
  attr.pad_type = pad_type;
  attr.multiplier = 0;  // TODO
  attr.has_relu = (activation.second == FuseCode::FUSED_RELU) ? true : false;

  graph_->AddOperator(rk::nn::OperatorType::CONV2D, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerDepthwiseConvImpl(
    const std::string& input, const std::string& weight,
    const std::string& bias, const std::vector<int32_t>& pads,
    const std::vector<int32_t>& strides, int32_t depth_multiplier, int32_t group,
    const std::string& output) {
  const auto activation = FindActivation(model_proto_, output);

  ADD_SHAPE(input);
  ADD_SHAPE(weight);
  if (bias != "") {
    ADD_SHAPE(bias);
  }
  shaper_.DepthwiseConv(m(input), m(weight), pads, strides, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  std::vector<uint32_t> weight_dims;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, weight)) {
    weight_dims = rk_tensors_.at(weight)->GetDims();
    inputs.push_back(rk_tensors_.at(weight));
  }
  if (bias != "") {
    if (HAS(rk_tensors_, bias)) {
      inputs.push_back(rk_tensors_.at(bias));
    }
  } else {
    uint32_t dim = shaper_[weight][0];
    void* ptr = (void*)malloc(sizeof(float) * dim);
    memset(ptr, 0, sizeof(float) * dim);
    free_list_.push_back(ptr);

    std::vector<uint32_t> dims = {dim};
    auto rk_bias = CreateRknnTensor(bias, dims, ptr, rk::nn::TensorRole::CONST);
    inputs.push_back(rk_bias);
  }

  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::Conv2DAttr attr;
  attr.ksize[0] = weight_dims[3];
  attr.ksize[1] = weight_dims[2];
  attr.stride[0] = strides[1];
  attr.stride[1] = strides[0];
  attr.pad[0] = pads[1];
  attr.pad[1] = pads[3];
  attr.pad[2] = pads[0];
  attr.pad[3] = pads[2];
  attr.group = group;
  attr.weights = weight_dims[0];  // TODO
  attr.dilation[0] = 1;
  attr.dilation[1] = 1;
  attr.pad_type = rk::nn::PadType::AUTO;
  attr.multiplier = depth_multiplier;  // TODO
  attr.has_relu = (activation.second == FuseCode::FUSED_RELU) ? true : false;

  graph_->AddOperator(rk::nn::OperatorType::CONV2D, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerConcat(const std::vector<std::string>& input,
                                   int32_t axis, const std::string& output) {
  for (const auto& name : input) {
    ADD_SHAPE(name);
  }
  shaper_.Concat(input, axis, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  for (const auto& in : input) {
    if (HAS(rk_tensors_, in)) {
      inputs.push_back(rk_tensors_.at(in));
    }
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::ConcatAttr attr;
  attr.axis = axis;
  graph_->AddOperator(rk::nn::OperatorType::CONCAT, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerAvePoolImpl(
    const std::string& input, const std::vector<int32_t>& kernel_shape,
    const std::vector<int32_t>& pads, const std::vector<int32_t>& strides,
    const int32_t ceil_mode, const std::string& output) {
  ADD_SHAPE(input);
  shaper_.Pool(m(input), kernel_shape, pads, strides, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::PoolAttr attr;
  attr.ksize[0] = kernel_shape[0];
  attr.ksize[1] = kernel_shape[1];
  attr.stride[0] = strides[0];
  attr.stride[1] = strides[1];
  attr.pad[0] = pads[0];
  attr.pad[1] = pads[2];
  attr.pad[2] = pads[1];
  attr.pad[3] = pads[3];
  attr.pad_type = rk::nn::PadType::AUTO;
  attr.pool_type = rk::nn::PoolType::POOLING_AVG;
  attr.round_type = (ceil_mode == 1) ? rk::nn::RoundType::ROUND_CEIL : rk::nn::RoundType::ROUND_FLOOR;
  attr.global_pooling = (kernel_shape[0] == -1 && kernel_shape[1] == -1);

  graph_->AddOperator(rk::nn::OperatorType::POOL, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerMaxPoolImpl(
    const std::string& input, const std::vector<int32_t>& kernel_shape,
    const std::vector<int32_t>& pads, const std::vector<int32_t>& strides,
    const int32_t ceil_mode, const std::string& output) {
  ADD_SHAPE(input);
  shaper_.Pool(m(input), kernel_shape, pads, strides, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::PoolAttr attr;
  attr.ksize[0] = kernel_shape[0];
  attr.ksize[1] = kernel_shape[1];
  attr.stride[0] = strides[0];
  attr.stride[1] = strides[1];
  attr.pad[0] = pads[0];
  attr.pad[1] = pads[2];
  attr.pad[2] = pads[1];
  attr.pad[3] = pads[3];
  attr.pad_type = rk::nn::PadType::AUTO;
  attr.pool_type = rk::nn::PoolType::POOLING_MAX;
  attr.round_type = (ceil_mode == 1) ? rk::nn::RoundType::ROUND_CEIL : rk::nn::RoundType::ROUND_FLOOR;
  attr.global_pooling = (kernel_shape[0] == -1 && kernel_shape[1] == -1);

  graph_->AddOperator(rk::nn::OperatorType::POOL, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerReLU(const std::string& input,
                                 const std::string& output) {
  ADD_SHAPE(input);
  shaper_.Relu(m(input), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  graph_->AddOperator(rk::nn::OperatorType::RELU, inputs, outputs, nullptr);
}

void OnnxConverter::AddLayerSoftmax(const std::string& input,
                                    const std::string& output) {
  ADD_SHAPE(input);
  shaper_.Softmax(m(input), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::SoftmaxAttr attr;
  attr.axis = 1;
  attr.beta = 1.0;

  graph_->AddOperator(rk::nn::OperatorType::SOFTMAX, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerFC(const std::string& input, const std::string& weight,
                               const std::string& bias,
                               const std::string& output) {
  const auto activation = FindActivation(model_proto_, output);

  ADD_SHAPE(input);
  ADD_SHAPE(weight);
  if (bias != "") {
    ADD_SHAPE(bias);
  }

  shaper_.FC(m(input), m(weight), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  std::vector<uint32_t> weight_dims;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, weight)) {
    weight_dims = rk_tensors_.at(weight)->GetDims();
    inputs.push_back(rk_tensors_.at(weight));
  }
  if (bias != "") {
    if (HAS(rk_tensors_, bias)) {
      inputs.push_back(rk_tensors_.at(bias));
    }
  } else {
    uint32_t dim = shaper_[weight][0];
    void* ptr = (void*)malloc(sizeof(float) * dim);
    memset(ptr, 0, sizeof(float) * dim);
    free_list_.push_back(ptr);

    std::vector<uint32_t> dims = {dim};
    auto rk_bias = CreateRknnTensor(bias, dims, ptr, rk::nn::TensorRole::CONST);
    inputs.push_back(rk_bias);
  }

  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::FCAttr attr;
  attr.weights = weight_dims[1];  //todo
  attr.has_relu = (activation.second == FuseCode::FUSED_RELU) ? true : false;

  graph_->AddOperator(rk::nn::OperatorType::FULLCONNECT, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerAdd(const std::string& input1, const std::string& input2,
                                const std::string& output) {
  ADD_SHAPE(input1);
  ADD_SHAPE(input2);
  shaper_.Eltwise(m(input1), m(input2), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input1)) {
    inputs.push_back(rk_tensors_.at(input1));
  }
  if (HAS(rk_tensors_, input2)) {
    inputs.push_back(rk_tensors_.at(input2));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  graph_->AddOperator(rk::nn::OperatorType::ADD, inputs, outputs, nullptr);
}

void OnnxConverter::AddLayerSub(const std::string& input1, const std::string& input2,
                                const std::string& output) {
  ADD_SHAPE(input1);
  ADD_SHAPE(input2);
  shaper_.Eltwise(m(input1), m(input2), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input1)) {
    inputs.push_back(rk_tensors_.at(input1));
  }
  if (HAS(rk_tensors_, input2)) {
    inputs.push_back(rk_tensors_.at(input2));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  graph_->AddOperator(rk::nn::OperatorType::SUBTRACT, inputs, outputs, nullptr);
}

void OnnxConverter::AddLayerMul(const std::string& input1, const std::string& input2,
                                const std::string& output) {
  ADD_SHAPE(input1);
  ADD_SHAPE(input2);
  shaper_.Eltwise(m(input1), m(input2), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input1)) {
    inputs.push_back(rk_tensors_.at(input1));
  }
  if (HAS(rk_tensors_, input2)) {
    inputs.push_back(rk_tensors_.at(input2));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  graph_->AddOperator(rk::nn::OperatorType::MULTIPLY, inputs, outputs, nullptr);
}

void OnnxConverter::AddLayerBatchNorm(const string& input,
                                      const string& scale_name,
                                      const string& bias_name,
                                      const string& mean_name,
                                      const string& var_name,
                                      const float eps,
                                      const string& output) {
  ADD_SHAPE(input);
  shaper_.BatchNorm(m(input), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, mean_name)) {
    inputs.push_back(rk_tensors_.at(mean_name));
  }
  if (HAS(rk_tensors_, var_name)) {
    inputs.push_back(rk_tensors_.at(var_name));
  }
  if (HAS(rk_tensors_, scale_name)) {
    inputs.push_back(rk_tensors_.at(scale_name));
  }
  if (HAS(rk_tensors_, bias_name)) {
    inputs.push_back(rk_tensors_.at(bias_name));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::BatchNormAttr attrs;
  attrs.eps = eps;

  graph_->AddOperator(rk::nn::OperatorType::BATCH_NORM, inputs, outputs, &attrs);
}

void OnnxConverter::AddLayerReshape(const string& input, const string& shape_name,
                                    const string& output) {
  ADD_SHAPE(input);

  GET_ATTR(shape, shape_name, int32_t);

  shaper_.Reshape(m(input), shape, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    // the "TFNodes/yolo_evaluation_layer_1/Reshape_5:0" of yolov3.onnx will wrong
    // when role is VAR, modify it to DATA for workaround.
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::DATA, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::ReshapeAttr attr;
  for (auto dim : shaper_[output]) {
    attr.shapes.push_back(static_cast<uint32_t>(dim));
  }
  graph_->AddOperator(rk::nn::OperatorType::RESHAPE, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerFlatten(const string& input, const int32_t axis, const string& output) {
  ADD_SHAPE(input);

  std::vector<int32_t> shape = {1, -1};  // axis = 0
  if (axis > 0) {
    auto in_shape = shaper_[m(input)];
    shape[0] = (int32_t)std::accumulate(in_shape.begin(),
                                        in_shape.begin() + axis, 1, std::multiplies<uint32_t>());
  }

  shaper_.Reshape(m(input), shape, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::ReshapeAttr attr;
  for (auto dim : shaper_[output]) {
    attr.shapes.push_back(static_cast<uint32_t>(dim));
  }
  graph_->AddOperator(rk::nn::OperatorType::RESHAPE, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerTranspose(const string& input, const std::vector<int32_t>& perm,
                                      const string& output) {
  ADD_SHAPE(input);

  shaper_.Transpose(m(input), perm, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::PermuteAttr attr;
  for (auto val : perm) {
    attr.perm.push_back(static_cast<uint32_t>(val));
  }
  graph_->AddOperator(rk::nn::OperatorType::PERMUTE, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerSlice(const string& input, const string& starts_name,
                                  const string& ends_name, const string& axes_name,
                                  const string& steps_name, const string& output) {
  ADD_SHAPE(input);

  GET_ATTR(starts, starts_name, int32_t);
  GET_ATTR(ends, ends_name, int32_t);
  GET_ATTR(axes, axes_name, int32_t);
  GET_ATTR(steps, steps_name, int32_t);

  for (auto step : steps) {
    if (step != 1) {
      LOGS_DEFAULT(FATAL) << "the steps of Slice must be 1!";
      return;
    }
  }

  shaper_.Slice(m(input), starts, ends, axes, steps, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  auto input_dims = shaper_[input];
  auto output_dims = shaper_[output];

  rk::nn::SliceAttr attr;
  for (size_t i = 0; i < output_dims.size(); i++) {
    attr.start.push_back(0);
    attr.length.push_back(static_cast<uint32_t>(output_dims[i]));
  }
  for (size_t i = 0; i < axes.size(); i++) {
    int32_t dim = input_dims[axes[i]];
    if (dim > 0) {
      int32_t start = starts[i] < 0 ? (starts[i] + dim) : starts[i];
      attr.start[axes[i]] = std::max(start, 0);
    }
  }

  graph_->AddOperator(rk::nn::OperatorType::SLICE, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerSqueeze(const string& input, const std::vector<int32_t>& axes,
                                    const string& output) {
  ADD_SHAPE(input);

  shaper_.Squeeze(m(input), axes, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::ReshapeAttr attr;
  for (auto dim : shaper_[output]) {
    attr.shapes.push_back(static_cast<uint32_t>(dim));
  }
  graph_->AddOperator(rk::nn::OperatorType::RESHAPE, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerUnsqueeze(const string& input, const std::vector<int32_t>& axes,
                                      const string& output) {
  ADD_SHAPE(input);

  shaper_.Unsqueeze(m(input), axes, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::ReshapeAttr attr;
  for (auto dim : shaper_[output]) {
    attr.shapes.push_back(static_cast<uint32_t>(dim));
  }
  graph_->AddOperator(rk::nn::OperatorType::RESHAPE, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerGather(const string& input, const string& indices_name,
                                   const int32_t axis, const string& output) {
  ADD_SHAPE(input);
  ADD_SHAPE(indices_name);

  shaper_.Gather(m(input), indices_name, (int32_t)axis, output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::GatherAttr attr;
  attr.axis = axis;
  graph_->AddOperator(rk::nn::OperatorType::GATHER, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerLeakyRelu(const std::string& input, const float alpha,
                                      const std::string& output) {
  ADD_SHAPE(input);
  shaper_.Relu(m(input), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  rk::nn::LeakyReluAttr attr;
  attr.alpha = alpha;
  graph_->AddOperator(rk::nn::OperatorType::LEAKY_RELU, inputs, outputs, (void*)&attr);
}

void OnnxConverter::AddLayerClip(const std::string& input,
                                 const int32_t min,
                                 const int32_t max,
                                 const std::string& output) {
  ADD_SHAPE(input);
  shaper_.Relu(m(input), output);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, inputs[0]->GetPrecision());
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  if (min == 0 && max == 6) {
    graph_->AddOperator(rk::nn::OperatorType::RELU6, inputs, outputs, nullptr);
  } else {
    rk::nn::ClipAttr attr;
    attr.min = min;
    attr.max = max;
    graph_->AddOperator(rk::nn::OperatorType::CLIP, inputs, outputs, (void*)&attr);
  }
}

void OnnxConverter::AddLayerQuantizeLinear(const string& input,
                                           const string& output_scale,
                                           const string& output_zp,
                                           const string& output) {
  ADD_SHAPE(input);
  shaper_.Identity(m(input), output);

  GET_ATTR(out_s, output_scale, float);
  GET_ATTR(out_zp, output_zp, uint8_t);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    inputs.push_back(rk_tensors_.at(input));
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output], NULL, rk::nn::TensorRole::VAR,
                                      rk::nn::PrecisionType::UINT8, rk::nn::DataLayoutType::NCHW,
                                      rk::nn::QuantizationType::AFFINE_ASYMMETRIC, 8, out_s[0], out_zp[0]);
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  graph_->AddOperator(rk::nn::OperatorType::DATACONVERT, inputs, outputs, nullptr);
}

void OnnxConverter::AddLayerDequantizeLinear(const string& input,
                                             const string& input_scale,
                                             const string& input_zp,
                                             const string& output) {
  ADD_SHAPE(input);
  shaper_.Identity(m(input), output);

  GET_ATTR(in_s, input_scale, float);
  GET_ATTR(in_zp, input_zp, uint8_t);

  std::vector<std::shared_ptr<rk::nn::Tensor>> inputs, outputs;
  if (HAS(rk_tensors_, input)) {
    rk::nn::QuantizationParamAffineAsymmetric param;
    param.scale.push_back(in_s[0]);
    param.zero_point.push_back(in_zp[0]);
    auto tensor = rk_tensors_.at(input);
    tensor->SetQntParam(rk::nn::QuantizationType::AFFINE_ASYMMETRIC, 8, param);
    inputs.push_back(tensor);
  }
  if (HAS(rk_tensors_, output)) {
    outputs.push_back(rk_tensors_.at(output));
  } else {
    auto rk_output = CreateRknnTensor(output, shaper_[output],
                                      NULL, rk::nn::TensorRole::VAR, rk::nn::PrecisionType::FLOAT32);
    outputs.push_back(rk_output);
    rk_tensors_[output] = rk_output;
  }

  graph_->AddOperator(rk::nn::OperatorType::DATACONVERT, inputs, outputs, nullptr);
}

}  // namespace onnxruntime