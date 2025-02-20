/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/jit/get_compiler_ir.h"

#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "tensorflow/compiler/jit/compilability_check_util.h"
#include "tensorflow/compiler/jit/device_compiler.h"
#include "tensorflow/compiler/jit/xla_launch_util.h"
#include "tensorflow/compiler/jit/xla_platform_info.h"
#include "tensorflow/compiler/xla/client/executable_build_options.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/service/hlo_graph_dumper.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/common_runtime/eager/tensor_handle.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/op_def.pb.h"
#include "tensorflow/core/framework/resource_handle.pb.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/tsl/platform/errors.h"

namespace tensorflow {

static StatusOr<std::unique_ptr<xla::LocalExecutable>> BuildExecutable(
    xla::LocalClient* local_client,
    const XlaCompiler::CompilationResult& result,
    const XlaCompiler::Options& options,
    const bool xla_embed_ir_in_executable = false) {
  std::vector<const xla::Shape*> argument_layouts(
      result.xla_input_shapes.size());
  for (int i = 0, end = result.xla_input_shapes.size(); i < end; ++i) {
    argument_layouts[i] = &result.xla_input_shapes[i];
  }
  xla::ExecutableBuildOptions build_options;
  if (result.collective_info) {
    build_options.set_num_replicas(result.collective_info->group_size);
  }
  build_options.set_device_ordinal(
      options.device_ordinal != -1 ? options.device_ordinal
                                   : local_client->default_device_ordinal());
  build_options.set_result_layout(result.xla_output_shape);
  build_options.set_device_allocator(options.device_allocator.get());
  build_options.set_alias_passthrough_params(options.alias_passthrough_params);
  build_options.mutable_debug_options()->set_xla_detailed_logging_and_dumping(
      options.detailed_logging);
  // If the embed_ir_in_executable is set, hlo_proto will be dumped in
  // executable. The hlo_proto contains HLO modules and buffer assignment.
  build_options.mutable_debug_options()->set_xla_embed_ir_in_executable(
      xla_embed_ir_in_executable);
  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<xla::LocalExecutable>> executables,
      local_client->Compile(*result.computation, argument_layouts,
                            build_options));
  TF_RET_CHECK(executables.size() == 1);
  return std::move(executables[0]);
}

static StatusOr<std::string> BuildHLOString(
    IrExportStage stage, const XlaCompiler::CompilationResult& result,
    xla::LocalClient* local_client, const XlaCompiler::Options& options) {
  switch (stage) {
    case IrExportStage::HLO:
    case IrExportStage::HLO_NO_METADATA:
    case IrExportStage::HLO_SERIALIZED: {
      TF_ASSIGN_OR_RETURN(xla::ProgramShape program_shape,
                          result.computation->GetProgramShape());
      xla::HloModuleConfig config(program_shape);
      TF_ASSIGN_OR_RETURN(
          std::unique_ptr<xla::HloModule> new_module,
          xla::HloModule::CreateFromProto(result.computation->proto(), config));

      xla::HloPrintOptions opts;
      if (stage == IrExportStage::HLO_NO_METADATA) {
        opts.set_print_metadata(false);
      }

      if (stage == IrExportStage::HLO_SERIALIZED) {
        return new_module->ToProto().SerializeAsString();
      } else {
        return new_module->ToString(opts);
      }
    }
    case IrExportStage::OPTIMIZED_HLO:
    case IrExportStage::OPTIMIZED_HLO_SERIALIZED: {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<xla::LocalExecutable> executable,
                          BuildExecutable(local_client, result, options));
      xla::Executable* new_executable = executable->executable();
      if (stage == IrExportStage::OPTIMIZED_HLO_SERIALIZED) {
        return new_executable->module().ToProto().SerializeAsString();
      } else {
        return new_executable->module().ToString();
      }
    }
    case IrExportStage::OPTIMIZED_HLO_PROTO_SERIALIZED: {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<xla::LocalExecutable> executable,
                          BuildExecutable(local_client, result, options,
                                          /*xla_embed_ir_in_executable=*/true));
      return executable->executable()->hlo_proto()->SerializeAsString();
    }
    case IrExportStage::OPTIMIZED_HLO_DOT: {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<xla::LocalExecutable> executable,
                          BuildExecutable(local_client, result, options));
      StatusOr<std::string> graph = xla::RenderGraph(
          *executable->executable()->module().entry_computation(),
          "Visualization",
          /*debug_options=*/{}, xla::RenderedGraphFormat::kDot,
          /*hlo_render_options=*/{});
      TF_RETURN_IF_ERROR(graph.status());
      return *graph;
    }
  }
}

static StatusOr<std::vector<XlaCompiler::Argument>>
BuildXlaCompilerArgumentFromFuncBody(const FunctionBody* fbody) {
  TF_RET_CHECK(fbody != nullptr);
  auto& input_args = fbody->fdef.signature().input_arg();
  int input_arg_size = input_args.size();

  // Shape info is not in input_arg. parse it from arg_attrs.
  auto& arg_attrs = fbody->fdef.arg_attr();
  if (arg_attrs.size() != input_arg_size) {
    return errors::InvalidArgument(
        "The function to be lowered uses some tf.Variable defined outside_"
        "_the_function body.  This is not supported with using_tensor_spec."
        "Please modify the function with pure functional style.");
  }
  std::vector<TensorShape> shapes;
  shapes.reserve(input_arg_size);
  for (const auto& attr : arg_attrs) {
    const unsigned int& idx = attr.first;
    bool has_function_input_shape = false;
    for (const auto& attr_value : attr.second.attr()) {
      if (attr_value.first == "_output_shapes") {
        TF_RETURN_IF_ERROR(TensorShape::BuildTensorShape(
            attr_value.second.list().shape()[0], &shapes[idx]));
        has_function_input_shape = true;
      }
    }
    TF_RET_CHECK(has_function_input_shape);
  }

  // Build Xla Compiler Arguments
  std::vector<XlaCompiler::Argument> args;
  args.resize(input_arg_size);
  for (int64_t input_num = 0; input_num < input_arg_size; ++input_num) {
    XlaCompiler::Argument& arg = args[input_num];
    arg.kind = XlaCompiler::Argument::kParameter;
    arg.type = input_args[input_num].type();
    arg.shape = shapes[input_num];
    arg.name = input_args[input_num].name();
  }
  return args;
}

StatusOr<std::string> GetCompilerIr(
    IrExportStage stage, ProcessFunctionLibraryRuntime* pflr,
    absl::string_view func_name, Device* dev, EagerContext* context,
    absl::Span<const TensorHandle* const> inputs_handles) {
  // input_handles vector is empty for using_tensor_spec case
  bool using_tensor_spec = inputs_handles.empty() ? true : false;
  using XlaDeviceCompiler =
      DeviceCompiler<xla::LocalExecutable, xla::LocalClient>;

  auto is_tfrt_tpu_supported_stage = [](IrExportStage stage) {
    return stage == IrExportStage::HLO ||
           stage == IrExportStage::HLO_NO_METADATA ||
           stage == IrExportStage::HLO_SERIALIZED;
  };
  // TODO(b/238830423): support GetCompilerIr on TFRT TPU device for stages
  // that requires compilation from HLO to executable.
  if (dev->device_type() != DEVICE_CPU &&
      dev->tensorflow_accelerator_device_info()->stream == nullptr &&
      !is_tfrt_tpu_supported_stage(stage)) {
    return errors::Internal(
        "GetCompilerIr with requested stage is not supported on this device.");
  }
  NameAttrList function;
  function.set_name(std::string{func_name});

  FunctionLibraryRuntime* flr = pflr->GetFLR(dev->name());
  ResourceMgr* rmgr = dev->resource_manager();

  const FunctionBody* fbody = nullptr;
  std::vector<int> constant_arg_indices;
  std::vector<int> resource_arg_indices;
  TF_RETURN_IF_ERROR(GetBodyAndConstantsAndResources(
      flr, function, &fbody, &constant_arg_indices, &resource_arg_indices));

  std::vector<const Tensor*> inputs;
  std::deque<Tensor> inputs_storage;
  inputs.reserve(inputs_handles.size());
  std::vector<VariableInfo> variable_infos;
  if (!using_tensor_spec) {
    MemoryTypeVector input_memory_types =
        GetInputMemoryTypes(fbody, constant_arg_indices, resource_arg_indices);
    MemoryTypeVector output_memory_types = GetOutputMemoryTypes(fbody);
    for (int i = 0; i < inputs_handles.size(); i++) {
      const TensorHandle* th = inputs_handles[i];
      const Tensor* t;
      // Handle owns the tensor.
      TF_RETURN_IF_ERROR(th->Tensor(&t));
      if (absl::c_binary_search(constant_arg_indices, i)) {
        // Need to make sure it's on the host.
        inputs_storage.emplace_back(t->dtype(), t->shape());
        TF_RETURN_IF_ERROR(
            th->CopyToDevice(*context, /*d=*/nullptr, &inputs_storage.back()));
        inputs.push_back(&inputs_storage.back());
      } else {
        inputs.push_back(t);
      }
    }

    TF_RETURN_IF_ERROR(GetVariableInfosFromInputs(
        rmgr, dev, inputs, resource_arg_indices, &variable_infos));
    TF_RETURN_IF_ERROR(LockVariables(absl::MakeSpan(variable_infos)));
  }

  XlaPlatformInfo platform_info = XlaPlatformInfoFromDevice(dev);

  XlaDeviceCompiler* xla_device_compiler;
  TF_RETURN_IF_ERROR(rmgr->LookupOrCreate<XlaDeviceCompiler>(
      rmgr->default_container(), "xla_device_compiler", &xla_device_compiler,
      [&](XlaDeviceCompiler** xla_device_compiler) {
        return BuildXlaDeviceCompiler(dev, flr, platform_info,
                                      xla_device_compiler);
      }));
  core::ScopedUnref xla_device_compiler_ref(xla_device_compiler);

  se::Stream* stream = nullptr;
  if (const DeviceBase::AcceleratorDeviceInfo* accelerator_device_info =
          dev->tensorflow_accelerator_device_info()) {
    stream = accelerator_device_info->stream;
  }

  XlaCompiler::Options options;
  if (platform_info.device_type() == DEVICE_TPU && stream == nullptr) {
    options = GenerateTfrtTpuCompilerOptions(*xla_device_compiler, *flr);
  } else {
    options = GenerateCompilerOptions(*xla_device_compiler, *flr, dev, stream,
                                      platform_info,
                                      /*has_ref_vars=*/false);
  }

  XlaCompiler::CompileOptions compile_options;
  compile_options.always_return_tuple = false;
  compile_options.alias_resource_update = true;

  XlaCompiler compiler(options);

  StatusOr<std::vector<XlaCompiler::Argument>> args;
  if (using_tensor_spec) {
    args = BuildXlaCompilerArgumentFromFuncBody(fbody);
  } else {
    args = XlaComputationLaunchContext::BuildXlaCompilerArguments(
        constant_arg_indices, inputs, variable_infos, dev);
  }
  TF_RETURN_IF_ERROR(args.status());

  xla::LocalClient* local_client = xla_device_compiler->client();
  XlaCompiler::CompilationResult result;
  TF_RETURN_IF_ERROR(
      compiler.CompileFunction(compile_options, function, *args, &result));

  return BuildHLOString(stage, result, local_client, options);
}

}  // namespace tensorflow
