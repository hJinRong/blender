/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup fn
 *
 * The signature of a multi-function contains the functions name and expected parameters. New
 * signatures should be build using the #MFSignatureBuilder class.
 */

#include "FN_multi_function_param_type.hh"

#include "BLI_vector.hh"

namespace blender::fn {

struct MFSignature {
  struct ParamInfo {
    MFParamType type;
    const char *name;
  };

  /**
   * The name should be statically allocated so that it lives longer than this signature. This is
   * used instead of an #std::string because of the overhead when many functions are created.
   * If the name of the function has to be more dynamic for debugging purposes, override
   * #MultiFunction::debug_name() instead. Then the dynamic name will only be computed when it is
   * actually needed.
   */
  const char *function_name;
  Vector<ParamInfo> params;
};

class MFSignatureBuilder {
 private:
  MFSignature &signature_;

 public:
  MFSignatureBuilder(const char *function_name, MFSignature &signature_to_build)
      : signature_(signature_to_build)
  {
    signature_.function_name = function_name;
  }

  /* Input Parameter Types */

  template<typename T> void single_input(const char *name)
  {
    this->single_input(name, CPPType::get<T>());
  }
  void single_input(const char *name, const CPPType &type)
  {
    this->input(name, MFDataType::ForSingle(type));
  }
  template<typename T> void vector_input(const char *name)
  {
    this->vector_input(name, CPPType::get<T>());
  }
  void vector_input(const char *name, const CPPType &base_type)
  {
    this->input(name, MFDataType::ForVector(base_type));
  }
  void input(const char *name, MFDataType data_type)
  {
    signature_.params.append({MFParamType(MFParamType::Input, data_type), name});
  }

  /* Output Parameter Types */

  template<typename T> void single_output(const char *name)
  {
    this->single_output(name, CPPType::get<T>());
  }
  void single_output(const char *name, const CPPType &type)
  {
    this->output(name, MFDataType::ForSingle(type));
  }
  template<typename T> void vector_output(const char *name)
  {
    this->vector_output(name, CPPType::get<T>());
  }
  void vector_output(const char *name, const CPPType &base_type)
  {
    this->output(name, MFDataType::ForVector(base_type));
  }
  void output(const char *name, MFDataType data_type)
  {
    signature_.params.append({MFParamType(MFParamType::Output, data_type), name});
  }

  /* Mutable Parameter Types */

  template<typename T> void single_mutable(const char *name)
  {
    this->single_mutable(name, CPPType::get<T>());
  }
  void single_mutable(const char *name, const CPPType &type)
  {
    this->mutable_(name, MFDataType::ForSingle(type));
  }
  template<typename T> void vector_mutable(const char *name)
  {
    this->vector_mutable(name, CPPType::get<T>());
  }
  void vector_mutable(const char *name, const CPPType &base_type)
  {
    this->mutable_(name, MFDataType::ForVector(base_type));
  }
  void mutable_(const char *name, MFDataType data_type)
  {
    signature_.params.append({MFParamType(MFParamType::Mutable, data_type), name});
  }

  void add(const char *name, const MFParamType &param_type)
  {
    switch (param_type.interface_type()) {
      case MFParamType::Input:
        this->input(name, param_type.data_type());
        break;
      case MFParamType::Mutable:
        this->mutable_(name, param_type.data_type());
        break;
      case MFParamType::Output:
        this->output(name, param_type.data_type());
        break;
    }
  }

  template<MFParamCategory Category, typename T>
  void add(MFParamTag<Category, T> /* tag */, const char *name)
  {
    switch (Category) {
      case MFParamCategory::SingleInput:
        this->single_input<T>(name);
        return;
      case MFParamCategory::VectorInput:
        this->vector_input<T>(name);
        return;
      case MFParamCategory::SingleOutput:
        this->single_output<T>(name);
        return;
      case MFParamCategory::VectorOutput:
        this->vector_output<T>(name);
        return;
      case MFParamCategory::SingleMutable:
        this->single_mutable<T>(name);
        return;
      case MFParamCategory::VectorMutable:
        this->vector_mutable<T>(name);
        return;
    }
    BLI_assert_unreachable();
  }
};

}  // namespace blender::fn
