# -----------------------------------------------------------------------------
#
# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#
# -----------------------------------------------------------------------------

import json
from typing import Dict, List, Optional

import onnx
import yaml
from onnx import helper

"""
    The network specilization file is generated by loading the onnx graph and fecthing the graph inputs and outputs.
"""


def generate_qnn_specialization(
    onnx_graph_path: str,
    specializations: Optional[List[Dict[str, int]]] = None,
    custom_io: Optional[Dict[str, str]] = None,
    file_path: str = "custom_io_config.yaml",
) -> None:
    """
    Generates network specialization config custom IO file for converter stage in QNN compilation.
    Reads onnx graph and creates a custom IO configuration file according to the passed parameters and
    save it as a yaml file provided in file_path argument.

    ``Mandatory`` Args:
        :onnx_graph_path (str): Generated ``ONNX`` Model Path.
        :batch_size (int): Batch size to compile the model for.
        :sequence_length (int): Sequence length for the model to compile.
        :context_length (int): Maximum context length to compile the model.

    ``Optional`` Args:
        :file_path (str): File path to save the generated custom IO config. ``Defaults to custom_io_config.yaml.``
        :full_batch_size (int): Set full batch size to enable continuous batching mode. ``Default to None``
        :kv_precision (str): Sets kv precision for compilation.  ``Defaults to float16.``
        :kv_cache_batch_size (int): kv_cache_batch_size for Prefix Caching. ``Defaults to None.``
    """
    print(specializations)

    # Load the ONNX model
    onnx_model = onnx.load(onnx_graph_path)

    input_nodes_info = []
    final_dict = {}
    output_nodes_info = []
    for node in onnx_model.graph.input:
        input_info = {}

        # Assigining data type value as per the onnx graph input.
        input_info["DataType"] = str(helper.tensor_dtype_to_np_dtype(node.type.tensor_type.elem_type))

        # Over riding the data type according to the custom_io provided.
        if node.name in custom_io:
            input_info["DataType"] = "uint8" if custom_io[node.name] == "mxint8" else custom_io[node.name]

        # Create Shapes List
        shapes = []
        for input_shape in node.type.tensor_type.shape.dim:
            if input_shape.HasField("dim_value"):
                shape = input_shape.dim_value
            elif input_shape.HasField("dim_param"):
                shape = input_shape.dim_param
            else:
                raise AttributeError(f"ERROR: {input_shape} Shape not Found")
            shapes.append(shape)
        print("Shapes: ", shapes)
        # Filling shape value for past_key / past_value nodes.
        if "past_key" in node.name or "past_value" in node.name:
            shape_list = []
            for input_shape in shapes:
                if isinstance(input_shape, str):
                    if input_shape in specializations[0]:
                        shape_list.append(specializations[0][input_shape])
                    else:
                        raise AttributeError(f"ERROR: {input_shape} is required in specializations")
                else:
                    shape_list.append(input_shape)
            input_info["Shape"] = str(shape_list).replace("[", "(").replace("]", ")")
        elif len(shapes) == 2:
            prefill_shape_list = []
            for input_shape in shapes:
                if isinstance(input_shape, str):
                    if input_shape in specializations[0]:
                        prefill_shape_list.append(specializations[0][input_shape])
                    else:
                        raise AttributeError(f"ERROR: {input_shape} is required in specializations")
                else:
                    prefill_shape_list.append(input_shape)
            decode_shape_list = []
            for input_shape in shapes:
                if isinstance(input_shape, str):
                    if input_shape in specializations[1]:
                        decode_shape_list.append(specializations[1][input_shape])
                    else:
                        raise AttributeError(f"ERROR: {input_shape} is required in specializations")
                else:
                    decode_shape_list.append(input_shape)

            input_info["Shape"] = (
                str(prefill_shape_list).replace("[", "(").replace("]", ")")
                + ", "
                + str(decode_shape_list).replace("[", "(").replace("]", ")")
            )
        else:
            raise AttributeError(f"ERROR: {shapes} Unknown Shape Dimension")

        input_nodes_info.append({"Name": node.name, "Desired Model Parameters": input_info})
        print(input_info)
    # Prepare output tensor configuration
    for output in onnx_model.graph.output:
        output_info = {}
        output_info["DataType"] = str(helper.tensor_dtype_to_np_dtype(output.type.tensor_type.elem_type))
        # Over riding the data type according to the custom_io provided.
        if output.name in custom_io:
            output_info["DataType"] = "uint8" if custom_io[output.name] == "mxint8" else custom_io[output.name]

        output_nodes_info.append({"Name": output.name, "Desired Model Parameters": output_info})

    # Combine input and output configurations
    final_dict = {"Input Tensor Configuration": input_nodes_info, "Output Tensor Configuration": output_nodes_info}

    print(final_dict)

    # Save the configuration to a YAML file
    try:
        with open(file_path, "w") as yaml_file:
            yaml.dump(final_dict, yaml_file, default_flow_style=False, sort_keys=False)
    except Exception as e:
        print(f"Failed to create YAML File for QNN Network Specialization Configuration{file_path}: {e}")


def generate_data_format_config(
    onnx_graph_path: str,
    *,
    data_format: Optional[str] = "QNN_TENSOR_DATA_FORMAT_MX",
    model_dlc_name: Optional[str] = "model",
    file_path: str = "qnn_data_format_config.json",
) -> None:
    """
    Generates data format config for context binary generation stage in QNN compilation path.
    It defines the tensor format for KV nodes when precision is set to mxint8.
    Reads onnx graph and creates a data format configuration file and save it as a json file provided in
    file_path argument.

    ``Mandatory`` Args:
        :onnx_graph_path (str): Generated ``ONNX`` Model Path.

    ``Optional`` Args:
        :data_format (str): Tensor format for KV nodes. ``Defaults to QNN_TENSOR_DATA_FORMAT_MX.``
        :model_dlc_name (str): DLC Name generated by the converter stage in QNN Compilation. ``Defaults to model.``
        :file_path (str): File path to save the generated data format config. ``Defaults to qnn_data_format_config.json.``
    """

    # Load the ONNX model
    onnx_model = onnx.load(onnx_graph_path)

    kv_nodes: list = []

    for input in onnx_model.graph.input:
        if "past_key" in input.name or "past_value" in input.name:
            kv_nodes.append((input.name).replace(".", "_"))
    for output in onnx_model.graph.output:
        if "past_key" in output.name or "past_value" in output.name:
            kv_nodes.append((output.name).replace(".", "_"))
            kv_overrides = {}

    kv_overrides["graphs"] = [
        {
            "graph_name": model_dlc_name + "_configuration_1",
            "tensors": [{"tensor_name": node, "dataFormat": data_format} for node in kv_nodes],
        },
        {
            "graph_name": model_dlc_name + "_configuration_2",
            "tensors": [{"tensor_name": node, "dataFormat": data_format} for node in kv_nodes],
        },
    ]

    try:
        with open(file_path, "w") as json_file:
            json.dump(kv_overrides, json_file, indent=4)
    except Exception as e:
        print(f"Failed to create JSON File for QNN Data Format Configuration{file_path}: {e}")
