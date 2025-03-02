// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/optimizer/gather_to_split_fusion.h"

#include "core/graph/graph_utils.h"
#include "core/optimizer/initializer.h"
#include "core/optimizer/utils.h"

namespace onnxruntime {

bool GatherToSplitFusion::IsSupportedGather(const Graph& graph, const Node& node, int64_t& index, int64_t& axis) const {
  if (!graph_utils::IsSupportedOptypeVersionAndDomain(node, "Gather", {1, 11, 13}) ||
      !graph_utils::IsSupportedProvider(node, GetCompatibleExecutionProviders())) {
    return false;
  }

  const NodeArg& input_arg = *(node.InputDefs()[1]);
  if (!optimizer_utils::IsScalar(input_arg)) return false;
  const ONNX_NAMESPACE::TensorProto* tensor_proto = graph_utils::GetConstantInitializer(graph, input_arg.Name());
  if (!tensor_proto) return false;
  Initializer init_const{*tensor_proto, graph.ModelPath()};
  if (tensor_proto->data_type() != ONNX_NAMESPACE::TensorProto_DataType_INT64) return false;
  index = *(init_const.data<int64_t>());
  axis = 0;  // Default value.
  auto& attrs = node.GetAttributes();
  if (attrs.find("axis") != attrs.end()) {
    auto& axis_attr = attrs.at("axis");
    if (utils::HasInt(axis_attr)) axis = axis_attr.i();
  }
  return true;
}

/*
GatherToSplitFusion is to fuse:
Node -> Gather(index=0, axis=axis)
    |-> Gather(index=1, axis=axis)
    |-> Gather(index=2, axis=axis)
    |...

To

Node -> Split -> Squeeze(axis=axis)
             |-> Squeeze(axis=axis)
             |-> Squeeze(axis=axis)
             |...

So that we can use one kernel to finish the job.
*/
Status GatherToSplitFusion::ApplyImpl(Graph& graph, bool& modified, int graph_level,
                                      const logging::Logger& logger) const {
  GraphViewer graph_viewer(graph);
  const auto& node_topology_list = graph_viewer.GetNodesInTopologicalOrder();

  for (auto node_index : node_topology_list) {
    auto* p_node = graph.GetNode(node_index);
    if (p_node == nullptr) continue;  // we removed the node as part of an earlier fusion
    Node& node = *p_node;

    ORT_RETURN_IF_ERROR(Recurse(node, modified, graph_level, logger));

    // Gather following Shape is a common case but not the target case to fuse here as its compute is normally very
    // quick.
    if (node.OpType() == "Shape") continue;

    // Ideally it's possible that the node has multiple outputs and the Gather nodes can consume one of them.
    // To make the fusion simple, here we require the node has only one output, if in the future we observe
    // new pattern we can optimize the fusion here.
    if (node.MutableOutputDefs().size() > 1) continue;

    // No need to fuse if there is only one output or no output.
    size_t output_count = node.GetOutputEdgesCount();
    if (output_count <= 1) continue;

    auto shape = node.MutableOutputDefs()[0]->Shape();
    if (!shape) continue;
    int64_t rank = static_cast<int64_t>(shape->dim_size());

    bool can_fuse = true;
    bool first_edge = true;
    int64_t split_axis = 0;
    InlinedVector<NodeArg*> gather_outputs(output_count, nullptr);
    InlinedVector<std::reference_wrapper<Node>> nodes_to_fuse;
    for (auto it = node.OutputNodesBegin(); it != node.OutputNodesEnd(); ++it) {
      int64_t index, axis;
      if (!IsSupportedGather(graph, *it, index, axis)) {
        can_fuse = false;
        break;
      }
      if (axis < 0) axis += rank;
      if (first_edge) {
        auto dim = shape->dim(static_cast<int>(axis));
        if (!utils::HasDimValue(dim) || dim.dim_value() != static_cast<int64_t>(output_count)) {
          can_fuse = false;
          break;
        }
        split_axis = axis;
        first_edge = false;
      } else if (axis != split_axis) {
        can_fuse = false;
        break;
      }
      if (index < 0) index += static_cast<int64_t>(output_count);
      if (index < 0 || index >= static_cast<int64_t>(output_count) || gather_outputs[static_cast<size_t>(index)]) {
        can_fuse = false;
        break;
      }
      Node& gather_node = *graph.GetNode(it->Index());
      nodes_to_fuse.emplace_back(gather_node);
      gather_outputs[static_cast<size_t>(index)] = gather_node.MutableOutputDefs()[0];
    }

    if (!can_fuse) continue;

    ONNX_NAMESPACE::TypeProto split_output_type;
    const ONNX_NAMESPACE::TensorProto_DataType element_type = static_cast<ONNX_NAMESPACE::TensorProto_DataType>(
        node.MutableOutputDefs()[0]->TypeAsProto()->tensor_type().elem_type());
    split_output_type.mutable_tensor_type()->set_elem_type(element_type);
    for (int64_t i = 0; i < rank; ++i) {
      if (i == split_axis) {
        split_output_type.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1LL);
      } else {
        *(split_output_type.mutable_tensor_type()->mutable_shape()->add_dim()) = shape->dim(static_cast<int>(i));
      }
    }

    InlinedVector<NodeArg*> split_outputs;
    for (size_t i = 0; i < output_count; ++i) {
      split_outputs.emplace_back(
          &graph.GetOrCreateNodeArg(graph.GenerateNodeArgName("split" + std::to_string(i)), &split_output_type));
    }

    Node& split_node = graph.AddNode(graph.GenerateNodeName("Split"), "Split", "Split for Fused Gather nodes",
                                     {node.MutableOutputDefs()[0]}, split_outputs);
    split_node.AddAttribute("axis", split_axis);
    split_node.SetExecutionProviderType(node.GetExecutionProviderType());

    // Squeeze before and after OpSet-13 have different schemas.
    int onnx_opset_version = -1;
    if (graph.DomainToVersionMap().find(kOnnxDomain) != graph.DomainToVersionMap().end()) {
      onnx_opset_version = graph.DomainToVersionMap().at(kOnnxDomain);
    }

    if (onnx_opset_version < 13) {
      for (size_t i = 0; i < output_count; ++i) {
        Node& squeeze_node = graph.AddNode(graph.GenerateNodeName("Squeeze" + std::to_string(i)), "Squeeze",
                                           "Squeeze for Fused Gather nodes", {split_outputs[i]}, {gather_outputs[i]});
        squeeze_node.AddAttribute("axes", std::vector<int64_t>{split_axis});
        squeeze_node.SetExecutionProviderType(node.GetExecutionProviderType());
      }
    } else {
      ONNX_NAMESPACE::TensorProto axes_initializer_proto;
      axes_initializer_proto.set_name(graph.GenerateNodeName("SqueezeAxesInitializer"));
      axes_initializer_proto.add_dims(static_cast<int64_t>(1));
      axes_initializer_proto.set_data_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
      InlinedVector<int64_t> axes_value{split_axis};
      axes_initializer_proto.set_raw_data(axes_value.data(), axes_value.size() * sizeof(int64_t));
      NodeArg* axes_arg = &graph_utils::AddInitializer(graph, axes_initializer_proto);

      for (size_t i = 0; i < output_count; ++i) {
        Node& squeeze_node =
            graph.AddNode(graph.GenerateNodeName("Squeeze" + std::to_string(i)), "Squeeze",
                          "Squeeze for Fused Gather nodes", {split_outputs[i], axes_arg}, {gather_outputs[i]});
        squeeze_node.SetExecutionProviderType(node.GetExecutionProviderType());
      }
    }

    for (Node& n : nodes_to_fuse) {
      graph_utils::RemoveNodeOutputEdges(graph, n);
      graph.RemoveNode(n.Index());
    }

    modified = true;
  }

  return Status::OK();
}
}  // namespace onnxruntime
