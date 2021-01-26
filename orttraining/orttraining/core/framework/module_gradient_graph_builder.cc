// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/graph/model.h"
#include "core/graph/graph_utils.h"
#include "core/providers/cpu/cpu_execution_provider.h"
#include "orttraining/core/framework/module_gradient_graph_builder.h"
#include "orttraining/core/framework/gradient_graph_builder.h"
#include "orttraining/core/session/training_session.h"
#include "orttraining/core/optimizer/graph_transformer_utils.h"

namespace onnxruntime {
namespace training {

using namespace onnxruntime::common;

void GetInputAndOutputNames(const Node& node, std::unordered_set<std::string>& input_names,
                            std::unordered_set<std::string>& output_names) {
  std::for_each(node.InputDefs().begin(), node.InputDefs().end(),
                [&input_names](const NodeArg* node_arg) { input_names.insert(node_arg->Name()); });
  std::for_each(node.OutputDefs().begin(), node.OutputDefs().end(),
                [&output_names](const NodeArg* node_arg) { output_names.insert(node_arg->Name()); });
}

void RemoveNodes(Graph& graph, const std::vector<Node*>& nodes_to_remove) {
  for (Node* node_to_remove : nodes_to_remove) {
    graph_utils::RemoveNodeOutputEdges(graph, *node_to_remove);
    graph.RemoveNode(node_to_remove->Index());
  }
}

void FilterInitializers(Graph& graph, const std::unordered_set<std::string>& input_names) {
  const auto& initializers = graph.GetAllInitializedTensors();
  std::unordered_set<std::string> initializer_names_to_remove;
  for (const auto& initializer : initializers) {
    if (input_names.find(initializer.first) == input_names.end()) {
      initializer_names_to_remove.insert(initializer.first);
    }
  }

  for (const auto& initializer_name : initializer_names_to_remove) {
    graph.RemoveInitializedTensor(initializer_name);
  }
}

Status ModuleGradientGraphBuilder::Initialize(std::istream& model_istream,
                                              const ModuleGradientGraphBuilderConfiguration& config) {
  // We need to apply the pre-training transformers before the gradient graph builder so we can build
  // an optimized gradient graph. The constant folding transformer depends on concrete shapes, without
  // constant folding with concrete shapes, shapes of some intermediate tensors will fail to infer.
  // This means we need to "apply transformers -> build gradient graph -> split" each time we have different
  // concrete input shapes. So this init func is just to save the original graph and config.
  ONNX_NAMESPACE::ModelProto model_proto;
  ORT_RETURN_IF_ERROR(Model::Load(model_istream, &model_proto));
  ORT_RETURN_IF_ERROR(Model::Load(model_proto, model_, nullptr, *logger_));

  // Handle original model inputs, outputs and trainable initializers.
  Graph& graph = model_->MainGraph();
  const std::vector<const NodeArg*>& graph_inputs = graph.GetInputsIncludingInitializers();
  for (auto& node_arg : graph_inputs) {
    split_graphs_info_.user_input_names.emplace_back(node_arg->Name());
  }

  const std::vector<const NodeArg*>& graph_outputs = graph.GetOutputs();
  for (auto& node_arg : graph_outputs) {
    split_graphs_info_.user_output_names.emplace_back(node_arg->Name());
  }

  split_graphs_info_.initializer_names_to_train.assign(config.initializer_names_to_train.begin(),
                                                       config.initializer_names_to_train.end());

  // Remove the training initializers from the graph and move them to input to save memory.
  std::vector<const NodeArg*> input_args;
  for (const auto& input_name : split_graphs_info_.user_input_names) {
    input_args.emplace_back(graph.GetNodeArg(input_name));
  }

  for (const auto& initializer_name : split_graphs_info_.initializer_names_to_train) {
    input_args.emplace_back(graph.GetNodeArg(initializer_name));
    graph.RemoveInitializedTensor(initializer_name);
  }

  graph.SetInputs(input_args);

  config_ = config;
  return Status::OK();
}

Status ModuleGradientGraphBuilder::BuildAndSplit(const std::vector<std::vector<int64_t>>& input_shapes) {
  // Make a copy of the original model.
  auto model_proto = model_->ToProto();
  std::shared_ptr<onnxruntime::Model> model_copied;
  ORT_RETURN_IF_ERROR(Model::Load(model_proto, model_copied, nullptr, *logger_));
  Graph& graph = model_copied->MainGraph();

  // Replace the input shapes.
  std::vector<const NodeArg*> input_args;
  size_t input_index = 0;
  for (const auto& input_name : split_graphs_info_.user_input_names) {
    NodeArg* input_node_arg = graph.GetNodeArg(input_name);
    ONNX_NAMESPACE::TensorShapeProto new_shape;
    for (size_t i = 0; i < input_shapes[input_index].size(); i++) {
      new_shape.add_dim()->set_dim_value(input_shapes[input_index][i]);
    }

    input_node_arg->SetShape(new_shape);
    input_args.emplace_back(input_node_arg);
    input_index++;
  }

  // Move over all training initializer inputs. They already have the concrete shapes.
  const std::vector<const NodeArg*>& graph_inputs = graph.GetInputsIncludingInitializers();
  for (; input_index < graph_inputs.size(); input_index++) {
    input_args.emplace_back(graph_inputs[input_index]);
  }

  graph.SetInputs(input_args);
  ORT_RETURN_IF_ERROR(graph.Resolve());

  // Register and apply transformers for pre-training.
  const TrainingSession::TrainingConfiguration::GraphTransformerConfiguration graph_transformer_config{};
  GraphTransformerManager graph_transformation_mgr{2};
  std::unique_ptr<CPUExecutionProvider> cpu_execution_provider =
      onnxruntime::make_unique<CPUExecutionProvider>(CPUExecutionProviderInfo());

  std::unordered_set<std::string> x_node_arg_names;
  std::set_union(config_.initializer_names_to_train.begin(), config_.initializer_names_to_train.end(),
                 config_.input_names_require_grad.begin(), config_.input_names_require_grad.end(),
                 std::inserter(x_node_arg_names, x_node_arg_names.begin()));
  auto add_transformers = [&](TransformerLevel level) {
    std::unordered_map<std::string, std::string> updated_weight_names{};
    auto transformers_to_register = transformer_utils::GeneratePreTrainingTransformers(
        level, x_node_arg_names, graph_transformer_config, *cpu_execution_provider, updated_weight_names, {});
    for (auto& entry : transformers_to_register) {
      graph_transformation_mgr.Register(std::move(entry), level);
    }
  };

  for (int i = static_cast<int>(TransformerLevel::Level1); i <= static_cast<int>(TransformerLevel::MaxLevel); i++) {
    TransformerLevel level = static_cast<TransformerLevel>(i);
    if (TransformerLevel::MaxLevel >= level) {
      add_transformers(level);
    }
  }

  for (int i = static_cast<int>(TransformerLevel::Level1); i <= static_cast<int>(TransformerLevel::MaxLevel); i++) {
    ORT_RETURN_IF_ERROR(graph_transformation_mgr.ApplyTransformers(graph, static_cast<TransformerLevel>(i), *logger_));
  }

  // Build gradient graph.
  GradientGraphConfiguration gradient_graph_config{};
  gradient_graph_config.use_invertible_layernorm_grad = config_.use_invertible_layernorm_grad;
  gradient_graph_config.set_gradients_as_graph_outputs = true;
  std::unordered_set<std::string> y_node_arg_names(split_graphs_info_.user_output_names.begin(),
                                                   split_graphs_info_.user_output_names.end());
  GradientGraphBuilder grad_graph_builder(&graph, y_node_arg_names, x_node_arg_names, "", gradient_graph_config,
                                          *logger_);
  ORT_RETURN_IF_ERROR(grad_graph_builder.Build());

  // Fix inputs/outputs related to gradients.
  GraphViewer graph_viewer(graph);
  const auto& node_topology_list = graph_viewer.GetNodesInTopologicalOrder();
  std::unordered_set<std::string> input_names;
  std::unordered_set<std::string> output_names;
  for (auto node_index : node_topology_list) {
    auto& node = *graph.GetNode(node_index);
    GetInputAndOutputNames(node, input_names, output_names);
  }

  input_args.clear();
  for (const NodeArg* input_node_arg : graph.GetInputsIncludingInitializers()) {
    input_args.emplace_back(input_node_arg);
  }

  // Add the entry points of gradients (normally loss_gard) to the graph inputs. Using the order of graph outputs.
  split_graphs_info_.user_output_grad_names.clear();
  split_graphs_info_.backward_output_grad_names.clear();
  for (const auto& output_name : split_graphs_info_.user_output_names) {
    std::string output_gradient_name = output_name + "_grad";
    if (input_names.find(output_gradient_name) != input_names.end()) {
      split_graphs_info_.user_output_grad_names.emplace_back(output_gradient_name);
      // Only add to graph input when it's not an output of a node.
      if (output_names.find(output_gradient_name) == output_names.end()) {
        split_graphs_info_.backward_output_grad_names.emplace_back(output_gradient_name);
        NodeArg* output_gradient_node_arg = graph.GetNodeArg(output_gradient_name);
        output_gradient_node_arg->UpdateTypeAndShape(*graph.GetNodeArg(output_name), true, true, *logger_);
        input_args.emplace_back(output_gradient_node_arg);
      }
    }
  }

  graph.SetInputs(input_args);

  std::vector<const NodeArg*> output_args;
  for (auto& output_name : split_graphs_info_.user_output_names) {
    output_args.emplace_back(graph.GetNodeArg(output_name));
  }

  // Add initializer gradients to graph outputs.
  split_graphs_info_.initializer_grad_names_to_train.clear();
  for (const auto& initializer_name : split_graphs_info_.initializer_names_to_train) {
    std::string initializer_gradient_name = initializer_name + "_grad";
    if (output_names.find(initializer_gradient_name) != output_names.end()) {
      split_graphs_info_.initializer_grad_names_to_train.emplace_back(initializer_gradient_name);
      output_args.emplace_back(graph.GetNodeArg(initializer_gradient_name));
    }
  }

  // Add input gradients to graph outputs if it's required.
  for (const auto& input_name : config_.input_names_require_grad) {
    std::string input_gradient_name = input_name + "_grad";
    if (output_names.find(input_gradient_name) != output_names.end()) {
      output_args.emplace_back(graph.GetNodeArg(input_gradient_name));
    }
  }

  graph.SetOutputs(output_args);
  graph.Resolve();

  // Run the transformers again mainly for backward part, e.g., constant fold from those Shape nodes in backward graph.
  for (int i = static_cast<int>(TransformerLevel::Level1); i <= static_cast<int>(TransformerLevel::MaxLevel); i++) {
    ORT_RETURN_IF_ERROR(graph_transformation_mgr.ApplyTransformers(graph, static_cast<TransformerLevel>(i), *logger_));
  }

  // Create two copies of gradient model for forward and backward models respectively.
  auto gradient_model_proto = model_copied->ToProto();
  ORT_RETURN_IF_ERROR(Model::Load(gradient_model_proto, forward_model_, nullptr, *logger_));
  ORT_RETURN_IF_ERROR(Model::Load(gradient_model_proto, backward_model_, nullptr, *logger_));

  // Split the graph in the copies of gradient model.
  ORT_RETURN_IF_ERROR(Split());

  return Status::OK();
}

std::string SerializeModel(const std::shared_ptr<onnxruntime::Model>& model, const std::string& tag) {
  std::string model_str;
  if (!model->ToProto().SerializeToString(&model_str)) {
    ORT_THROW("Fail to serialize", tag, "model to string.");
  }

  return model_str;
}

Status ModuleGradientGraphBuilder::Build() {
  // Make a copy of the original model.
  auto model_proto = model_->ToProto();
  ORT_RETURN_IF_ERROR(Model::Load(model_proto, gradient_model_, nullptr, *logger_));

  // Build the gradient graph.
  ORT_RETURN_IF_ERROR(BuildGradientGraph());

  // Add Yield Op.
  AddYieldOp();

  // Reorder outputs.
  ReorderOutputs();

  PathString path_str("bert_gradient.onnx");
  std::remove(ToMBString(path_str).c_str());
  Model::Save(*gradient_model_, path_str);

  return Status::OK();
}

Status ModuleGradientGraphBuilder::BuildGradientGraph() {
  // Resolve forward graph, register and apply transformers for pre-training.
  Graph& gradient_graph = gradient_model_->MainGraph();
  ORT_RETURN_IF_ERROR(gradient_graph.Resolve());

  const TrainingSession::TrainingConfiguration::GraphTransformerConfiguration graph_transformer_config{};
  GraphTransformerManager graph_transformation_mgr{2};
  std::unique_ptr<CPUExecutionProvider> cpu_execution_provider =
      onnxruntime::make_unique<CPUExecutionProvider>(CPUExecutionProviderInfo());

  std::unordered_set<std::string> x_node_arg_names;
  std::set_union(config_.initializer_names_to_train.begin(), config_.initializer_names_to_train.end(),
                 config_.input_names_require_grad.begin(), config_.input_names_require_grad.end(),
                 std::inserter(x_node_arg_names, x_node_arg_names.begin()));
  auto add_transformers = [&](TransformerLevel level) {
    std::unordered_map<std::string, std::string> updated_weight_names{};
    auto transformers_to_register = transformer_utils::GeneratePreTrainingTransformers(
        level, x_node_arg_names, graph_transformer_config, *cpu_execution_provider, updated_weight_names, {});
    for (auto& entry : transformers_to_register) {
      graph_transformation_mgr.Register(std::move(entry), level);
    }
  };

  for (int i = static_cast<int>(TransformerLevel::Level1); i <= static_cast<int>(TransformerLevel::MaxLevel); i++) {
    TransformerLevel level = static_cast<TransformerLevel>(i);
    if (TransformerLevel::MaxLevel >= level) {
      add_transformers(level);
    }
  }

  for (int i = static_cast<int>(TransformerLevel::Level1); i <= static_cast<int>(TransformerLevel::MaxLevel); i++) {
    ORT_RETURN_IF_ERROR(
        graph_transformation_mgr.ApplyTransformers(gradient_graph, static_cast<TransformerLevel>(i), *logger_));
  }

  // Build gradient graph to backward graph.
  GradientGraphConfiguration gradient_graph_config{};
  gradient_graph_config.use_invertible_layernorm_grad = config_.use_invertible_layernorm_grad;
  gradient_graph_config.set_gradients_as_graph_outputs = false;
  std::unordered_set<std::string> y_node_arg_names(split_graphs_info_.user_output_names.begin(),
                                                   split_graphs_info_.user_output_names.end());
  GradientGraphBuilder grad_graph_builder(&gradient_graph, y_node_arg_names, x_node_arg_names, "",
                                          gradient_graph_config, *logger_);

  ORT_RETURN_IF_ERROR(grad_graph_builder.Build());

  // Apply transformers to backward graph.
  for (int i = static_cast<int>(TransformerLevel::Level1); i <= static_cast<int>(TransformerLevel::MaxLevel); i++) {
    ORT_RETURN_IF_ERROR(
        graph_transformation_mgr.ApplyTransformers(gradient_graph, static_cast<TransformerLevel>(i), *logger_));
  }

  return Status::OK();
}

void ModuleGradientGraphBuilder::AddYieldOp() {
  Graph& gradient_graph = gradient_model_->MainGraph();
  GraphViewer gradient_graph_viewer(gradient_graph);
  const auto& gradient_node_topology_list = gradient_graph_viewer.GetNodesInTopologicalOrder();
  std::vector<Node*> forward_nodes_to_remove;
  split_graphs_info_.user_output_grad_names.clear();
  for (const auto& name : split_graphs_info_.user_output_names) {
    split_graphs_info_.user_output_grad_names.emplace_back(name + "_grad");
  }

  std::unordered_set<std::string> user_output_grad_names{split_graphs_info_.user_output_grad_names.begin(),
                                                         split_graphs_info_.user_output_grad_names.end()};

  std::unordered_set<std::string> non_backward_user_output_grad_names;
  for (auto node_index : gradient_node_topology_list) {
    auto& node = *gradient_graph.GetNode(node_index);
    std::cout << "Node: " << node.Name() << "\n";
    for (const auto& node_arg : node.OutputDefs()) {
      if (user_output_grad_names.find(node_arg->Name()) != user_output_grad_names.end()) {
        non_backward_user_output_grad_names.insert(node_arg->Name());
        std::cout<<"Grad NodeArg:"<<node_arg->Name() <<"\n";
      }
    }
  }

  std::vector<std::string> yield_input_names;
  split_graphs_info_.backward_output_grad_names.clear();
  for (const auto& name : split_graphs_info_.user_output_names) {
    std::string grad_name = name + "_grad";
    if (non_backward_user_output_grad_names.find(grad_name) == non_backward_user_output_grad_names.end()) {
      yield_input_names.emplace_back(name);
      split_graphs_info_.backward_output_grad_names.emplace_back(grad_name);
    }
  }

  for (const auto& name : split_graphs_info_.user_output_names) {
    if (non_backward_user_output_grad_names.find(name + "_grad") != non_backward_user_output_grad_names.end()) {
      yield_input_names.emplace_back(name);
    }
  }

  std::vector<NodeArg*> yield_input_node_args;
  std::vector<NodeArg*> yield_output_node_args;
  for (const auto& name : yield_input_names) {
    yield_input_node_args.emplace_back(gradient_graph.GetNodeArg(name));
  }

  for (const auto& name : split_graphs_info_.backward_output_grad_names) {
    yield_output_node_args.emplace_back(gradient_graph.GetNodeArg(name));
  }

  gradient_graph.AddNode("YieldOp_fw_op", "Yield", "Yield Op", yield_input_node_args, yield_output_node_args, {}, kMSDomain);

  // Add Yield ops after each grad output is ready
  std::cout<<"Adding Grad Yield ops\n";
  // Get initializer gradients
  std::unordered_map<std::string, std::string> grad_name_map{};
  split_graphs_info_.initializer_grad_names_to_train.clear();
  for (const auto& initializer_name : split_graphs_info_.initializer_names_to_train) {
    std::string initializer_gradient_name = initializer_name + "_grad";
    split_graphs_info_.initializer_grad_names_to_train.emplace_back(initializer_gradient_name);
    grad_name_map[initializer_gradient_name] = initializer_name;
  }

  auto& grad_names = split_graphs_info_.initializer_grad_names_to_train;
  for (auto node_index : gradient_node_topology_list) {
    auto& node = *gradient_graph.GetNode(node_index);
    // std::cout << "Node: " << node.Name() << "\n";
    for (const auto& node_arg : node.OutputDefs()) {
      if (std::find(grad_names.begin(), grad_names.end(), node_arg->Name()) != grad_names.end()) {
        std::vector<NodeArg*> yield_input_node_arg;
        std::vector<NodeArg*> yield_output_node_arg;
        yield_input_node_arg.emplace_back(gradient_graph.GetNodeArg(node_arg->Name()));
        Node& yield_node = gradient_graph.AddNode("YieldOp_" + node_arg->Name(), "Yield", "Yield Op", yield_input_node_arg, yield_output_node_arg, {}, kMSDomain);
        yield_node.AddAttribute("push_input", static_cast<int64_t>(1));
        split_graphs_info_.ordered_initializer_names.emplace_back(grad_name_map[node_arg->Name()]);
        std::cout<<"Yield for Grad:"<< node_arg->Name() <<"\n";
      }
    }
  }
  //reverse the order to correctly get forward order
  std::reverse(split_graphs_info_.ordered_initializer_names.begin(), split_graphs_info_.ordered_initializer_names.end());
}

void ModuleGradientGraphBuilder::ReorderOutputs() {
  // Adjust gradient graph outputs by the following order:
  // 1. user outputs,
  // 2. user input grads if required, with same order of user inputs,
  // 3. trainable initailizer grads, with same order of trainable initializers.
  Graph& gradient_graph = gradient_model_->MainGraph();
  const std::vector<const NodeArg*>& gradient_graph_outputs = gradient_graph.GetOutputs();
  std::unordered_map<std::string, const NodeArg*> gradient_output_arg_map;
  for (auto& node_arg : gradient_graph_outputs) {
    gradient_output_arg_map[node_arg->Name()] = node_arg;
  }

  std::vector<const NodeArg*> new_output_args;
  for (const auto& user_output_name : split_graphs_info_.user_output_names) {
    new_output_args.emplace_back(gradient_graph.GetNodeArg(user_output_name));
  }

  std::unordered_set<std::string> user_input_require_grad_set(config_.input_names_require_grad.begin(),
                                                              config_.input_names_require_grad.end());

  split_graphs_info_.user_input_grad_names.clear();
  for (const auto& input_name : split_graphs_info_.user_input_names) {
    if (user_input_require_grad_set.find(input_name) != user_input_require_grad_set.end()) {
      std::string input_gradient_name = input_name + "_grad";
      ORT_ENFORCE(gradient_output_arg_map.find(input_gradient_name) != gradient_output_arg_map.end(),
                  "Required user input grad is not found on gradient graph.");
      split_graphs_info_.user_input_grad_names[input_name] = input_gradient_name;
      new_output_args.emplace_back(gradient_output_arg_map[input_gradient_name]);
    }
  }

  // Add initializer gradients to graph outputs.
  // split_graphs_info_.initializer_grad_names_to_train.clear();
  // for (const auto& initializer_name : split_graphs_info_.initializer_names_to_train) {
  //   std::string initializer_gradient_name = initializer_name + "_grad";
  //   ORT_ENFORCE(gradient_output_arg_map.find(initializer_gradient_name) != gradient_output_arg_map.end(),
  //               "Trainable initializer grad is not found on gradient graph.");
  //   split_graphs_info_.initializer_grad_names_to_train.emplace_back(initializer_gradient_name);
  //   // new_output_args.emplace_back(gradient_output_arg_map[initializer_gradient_name]);
  // }

  gradient_graph.SetOutputs(new_output_args);
}

std::string ModuleGradientGraphBuilder::GetForwardModel() const { return SerializeModel(forward_model_, "forward"); }

std::string ModuleGradientGraphBuilder::GetBackwardModel() const { return SerializeModel(backward_model_, "backward"); }

std::string ModuleGradientGraphBuilder::GetGradientModel() const { return SerializeModel(gradient_model_, "gradient"); }

Status ModuleGradientGraphBuilder::Split() {
  // Get forward model, also collect some information for backward model generation.
  Graph& forward_graph = forward_model_->MainGraph();
  GraphViewer forward_graph_viewer(forward_graph);
  const auto& forward_node_topology_list = forward_graph_viewer.GetNodesInTopologicalOrder();
  std::vector<Node*> forward_nodes_to_remove;
  std::unordered_set<std::string> forward_input_names;
  std::unordered_set<std::string> forward_output_names;
  std::unordered_set<std::string> backward_input_names;
  std::unordered_set<std::string> backward_output_names;
  for (auto node_index : forward_node_topology_list) {
    auto& node = *forward_graph.GetNode(node_index);
    // Currently we are using node description to distinguish the forward and backward nodes.
    if (node.Description() == "Backward pass") {
      forward_nodes_to_remove.emplace_back(&node);
      GetInputAndOutputNames(node, backward_input_names, backward_output_names);
    } else {
      GetInputAndOutputNames(node, forward_input_names, forward_output_names);
    }
  }

  std::unordered_set<std::string> intermediate_arg_names;
  for (const auto& forward_output_name : forward_output_names) {
    if (backward_input_names.find(forward_output_name) != backward_input_names.end()) {
      intermediate_arg_names.insert(forward_output_name);
    }
  }

  RemoveNodes(forward_graph, forward_nodes_to_remove);
  FilterInitializers(forward_graph, forward_input_names);

  // All user inputs should be also part of the forward graph inputs.
  std::vector<const NodeArg*> forward_input_args;
  for (const auto& input_name : split_graphs_info_.user_input_names) {
    forward_input_args.emplace_back(forward_graph.GetNodeArg(input_name));
  }

  // Add initializers to forward graph inputs.
  for (const auto& initializer_name : split_graphs_info_.initializer_names_to_train) {
    forward_input_args.emplace_back(forward_graph.GetNodeArg(initializer_name));
  }

  forward_graph.SetInputs(forward_input_args);

  // All user outputs should be also part of the forward graph outputs.
  std::vector<const NodeArg*> forward_output_args;
  for (const auto& output_name : split_graphs_info_.user_output_names) {
    forward_output_args.emplace_back(forward_graph.GetNodeArg(output_name));
  }

  // Add intermediate args to forward graph outputs.
  split_graphs_info_.intermediate_tensor_names.clear();
  for (const auto& intermediate_arg_name : intermediate_arg_names) {
    // Ignore the user outputs.
    if (std::find(split_graphs_info_.user_output_names.begin(), split_graphs_info_.user_output_names.end(),
                  intermediate_arg_name) == split_graphs_info_.user_output_names.end()) {
      split_graphs_info_.intermediate_tensor_names.emplace_back(intermediate_arg_name);
      forward_output_args.emplace_back(forward_graph.GetNodeArg(intermediate_arg_name));
    }
  }

  forward_graph.SetOutputs(forward_output_args);
  forward_graph.Resolve();

  // Get backward graph.
  Graph& backward_graph = backward_model_->MainGraph();
  GraphViewer backward_graph_viewer(backward_graph);
  const auto& backward_node_topology_list = backward_graph_viewer.GetNodesInTopologicalOrder();
  std::vector<Node*> backward_nodes_to_remove;
  for (auto node_index : backward_node_topology_list) {
    auto& node = *backward_graph.GetNode(node_index);
    if (node.Description() != "Backward pass") {
      backward_nodes_to_remove.emplace_back(&node);
    }
  }

  RemoveNodes(backward_graph, backward_nodes_to_remove);
  FilterInitializers(backward_graph, backward_input_names);

  // User inputs to backward graph inputs.
  split_graphs_info_.backward_user_input_names.clear();
  std::vector<const NodeArg*> backward_input_args;
  for (const auto& input_name : split_graphs_info_.user_input_names) {
    // Only takes those in the backward inputs.
    if (backward_input_names.find(input_name) != backward_input_names.end()) {
      split_graphs_info_.backward_user_input_names.emplace_back(input_name);
      backward_input_args.emplace_back(backward_graph.GetNodeArg(input_name));
    }
  }

  // Add initializer args to backward graph inputs if any node uses them.
  split_graphs_info_.backward_intializer_names_as_input.clear();
  for (const auto& initializer_name : split_graphs_info_.initializer_names_to_train) {
    // Some initializers will be inputs for backward graph.
    if (backward_input_names.find(initializer_name) != backward_input_names.end()) {
      split_graphs_info_.backward_intializer_names_as_input.emplace_back(initializer_name);
      backward_input_args.emplace_back(backward_graph.GetNodeArg(initializer_name));
      backward_graph.RemoveInitializedTensor(initializer_name);
    }
  }

  // Add intermediate args to backward graph inputs.
  for (const auto& intermediate_arg_name : split_graphs_info_.intermediate_tensor_names) {
    NodeArg* intermediate_node_arg = backward_graph.GetNodeArg(intermediate_arg_name);
    intermediate_node_arg->UpdateTypeAndShape(*forward_graph.GetNodeArg(intermediate_arg_name), true, true, *logger_);
    backward_input_args.emplace_back(intermediate_node_arg);
  }

  // Grad of user outputs to backward graph inputs.
  for (const auto& output_grad_name : split_graphs_info_.backward_output_grad_names) {
    backward_input_args.emplace_back(backward_graph.GetNodeArg(output_grad_name));
  }

  backward_graph.SetInputs(backward_input_args);

  // Exclude user outputs from the backward graph.
  const std::vector<const NodeArg*>& backward_graph_outputs = backward_graph.GetOutputs();
  std::vector<const NodeArg*> backward_output_args;
  for (auto& node_arg : backward_graph_outputs) {
    if (backward_output_names.find(node_arg->Name()) != backward_output_names.end()) {
      backward_output_args.emplace_back(node_arg);
    }
  }

  backward_graph.SetOutputs(backward_output_args);
  backward_graph.Resolve();
  return Status::OK();
}

}  // namespace training
}  // namespace onnxruntime
