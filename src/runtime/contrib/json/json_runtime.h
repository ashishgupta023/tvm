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
 * \file src/runtime/contrib/json/json_runtime.h
 * \brief Utilities for json runtime.
 */

#ifndef TVM_RUNTIME_CONTRIB_JSON_JSON_RUNTIME_H_
#define TVM_RUNTIME_CONTRIB_JSON_JSON_RUNTIME_H_

#include <tvm/runtime/container.h>
#include <tvm/runtime/module.h>
#include <tvm/runtime/ndarray.h>

#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json_node.h"

namespace tvm {
namespace runtime {
namespace json {

/*!
 * \brief A json runtime that executes the serialized JSON format. This runtime
 * can be extended by user defined runtime for execution.
 */
class JSONRuntimeBase : public ModuleNode {
 public:
  JSONRuntimeBase(const std::string& symbol_name, const std::string& graph_json,
                  const Array<String> const_names)
      : symbol_name_(symbol_name), graph_json_(graph_json), const_names_(const_names) {
    LoadGraph(graph_json_);
  }

  const char* type_key() const { return "json"; }

  /*! \brief Initialize a specific json runtime. */
  virtual void Init(const Array<NDArray>& consts) = 0;

  /*! \brief Invoke the execution engine to inteprete a specific json runtime. */
  virtual void Run() = 0;

  /*!
   * \brief Get a packed function.
   * \param name The name/symbol of the function.
   * \param sptr_to_self The pointer to the module node.
   * \return The packed function.
   */
  virtual PackedFunc GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) {
    if (name == "get_symbol") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->symbol_name_; });
    } else if (name == "get_const_vars") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->const_names_; });
    } else if (this->symbol_name_ == name) {
      return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
        CHECK(this->initialized_) << "The module has not been initialized";

        // Bind argument tensors to data entries.
        this->SetInputOutputBuffers(args);
        // Execute the subgraph.
        this->Run();
      });
    } else if ("__init_" + this->symbol_name_ == name) {
      // The function to initialize constant tensors.
      return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
        CHECK_EQ(args.size(), 1U);
        this->Init(args[0]);
        this->initialized_ = true;
        *rv = 0;
      });
    } else {
      return PackedFunc(nullptr);
    }
  }

  virtual void SaveToBinary(dmlc::Stream* stream) {
    // Save the symbol
    stream->Write(symbol_name_);
    // Save the graph
    stream->Write(graph_json_);
    // Save the required const names
    std::vector<std::string> consts;
    for (const auto& it : const_names_) {
      consts.push_back(it);
    }
    stream->Write(consts);
  }

  template <typename T,
            typename = typename std::enable_if<std::is_base_of<JSONRuntimeBase, T>::value>::type>
  static Module LoadFromBinary(void* strm) {
    dmlc::Stream* stream = static_cast<dmlc::Stream*>(strm);
    std::string symbol;
    std::string graph_json;
    std::vector<std::string> consts;
    // Load the symbol
    CHECK(stream->Read(&symbol)) << "Loading symbol name failed";
    CHECK(stream->Read(&graph_json)) << "Loading graph json failed";
    CHECK(stream->Read(&consts)) << "Loading the const name list failed";
    Array<String> const_names;
    for (const auto& it : consts) {
      const_names.push_back(it);
    }
    auto n = make_object<T>(symbol, graph_json, const_names);
    return Module(n);
  }

 protected:
  /*!
   * \brief Set up the input and output buffers by binding their DLTensor pointers to the
   * corresponding data entry.
   *
   * \param args The packed args.
   */
  void SetInputOutputBuffers(const TVMArgs& args) {
    CHECK_EQ(args.size(), input_var_idx_.size() + outputs_.size())
        << "Found mismatch in the number of provided data entryies and required.";

    for (size_t i = 0; i < static_cast<size_t>(args.size()); i++) {
      auto eid = i < input_var_idx_.size() ? EntryID(input_var_idx_[i], 0)
                                           : EntryID(outputs_[i - input_var_idx_.size()]);
      CHECK(args[i].type_code() == kTVMNDArrayHandle || args[i].type_code() == kTVMDLTensorHandle)
          << "Expect NDArray or DLTensor as inputs";

      const DLTensor* arg;
      if (args[i].IsObjectRef<NDArray>()) {
        NDArray arr = args[i];
        arg = arr.operator->();
      } else {
        arg = args[i].operator DLTensor*();
      }

      // Assign input/output the NDArray pointers to data entry so that we can directly
      // read/write host buffers.
      data_entry_[eid] = arg;
    }
  }

  /*!
   * \brief Load the graph and record the entries for inputs and constants.
   *
   * \param graph_json The graph in the json format.
   */
  void LoadGraph(const std::string& graph_json) {
    std::istringstream is(graph_json);
    dmlc::JSONReader reader(&is);
    this->Load(&reader);
    std::vector<std::string> consts;
    for (size_t i = 0; i < input_nodes_.size(); i++) {
      uint32_t nid = input_nodes_[i];
      std::string name = nodes_[nid].name_;
      if (nodes_[nid].op_type_ == "input") {
        input_var_idx_.push_back(nid);
      } else {
        CHECK_EQ(nodes_[nid].op_type_, "const");
        auto pos = std::find(std::begin(const_names_), std::end(const_names_), name);
        CHECK(pos != std::end(const_names_)) << "Found non-existent constant: " << name;
        const_idx_.push_back(nid);
        consts.push_back(name);
      }
    }
    CHECK_EQ(consts.size(), const_names_.size())
        << "Found mismatch for the number of constants in the graph and required.";

    for (size_t i = 0; i < consts.size(); i++) {
      CHECK_EQ(consts[i], const_names_[i])
          << "The position of constant in the graph must be the same as the required.";
    }

    // Reserve data entries.
    data_entry_.resize(NumEntries());
  }

  /*!
   * \brief Set up the constants/weights for inference by binding their DLTensor pointer to
   * the corresponding data entry.
   *
   * \param consts A list of constant NDArray to be used.
   */
  void SetupConstants(const Array<NDArray>& consts) {
    for (size_t i = 0; i < consts.size(); ++i) {
      data_entry_[EntryID(const_idx_[i], 0)] = consts[i].operator->();
    }
  }

  // Load the graph.
  void Load(dmlc::JSONReader* reader) {
    reader->BeginObject();
    std::string key;
    while (reader->NextObjectItem(&key)) {
      if (key == "nodes") {
        reader->Read(&nodes_);
      } else if (key == "arg_nodes") {
        reader->Read(&input_nodes_);
      } else if (key == "node_row_ptr") {
        reader->Read(&node_row_ptr_);
      } else if (key == "heads") {
        reader->Read(&outputs_);
      } else {
        LOG(FATAL) << "Unknown key: " << key;
      }
    }
  }

  // Get the node entry index.
  uint32_t EntryID(uint32_t nid, uint32_t index) const { return node_row_ptr_[nid] + index; }

  // Get the node entry index.
  uint32_t EntryID(const JSONGraphNodeEntry& e) const { return EntryID(e.id_, e.index_); }

  // Number of node entries.
  uint32_t NumEntries() const { return node_row_ptr_.back(); }

 protected:
  /*! \brief The only subgraph name for this module. */
  std::string symbol_name_;
  /*! \brief The graph. */
  std::string graph_json_;
  /*! \brief The required constant names. */
  Array<String> const_names_;
  /*! \brief The json graph nodes. */
  std::vector<JSONGraphNode> nodes_;
  /*! \brief The input nodes, including variables and constants. */
  std::vector<uint32_t> input_nodes_;
  /*! \brief Used for quick entry indexing. */
  std::vector<uint32_t> node_row_ptr_;
  /*! \brief Output entries. */
  std::vector<JSONGraphNodeEntry> outputs_;
  /*! \brief Data of that entry. */
  std::vector<const DLTensor*> data_entry_;
  /*! \brief Map the input name to node index. */
  std::vector<uint32_t> input_var_idx_;
  /*! \brief input const node index. */
  std::vector<uint32_t> const_idx_;
  /*! \brief Indicate if the engine has been initialized. */
  bool initialized_{false};
};

}  // namespace json
}  // namespace runtime
}  // namespace tvm
#endif  // TVM_RUNTIME_CONTRIB_JSON_JSON_RUNTIME_H_
