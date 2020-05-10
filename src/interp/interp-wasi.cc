/*
 * Copyright 2020 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/interp/interp-wasi.h"
#include "src/interp/interp-util.h"

#ifdef WITH_WASI

#include "uvwasi.h"

using namespace wabt;
using namespace wabt::interp;

namespace wabt {
namespace interp {

Result WasiBindImports(uvwasi_s* uvwasi,
                       const Module::Ptr& module,
                       RefVec& imports,
                       Stream* stream,
                       Stream* trace_stream) {
  Store* store = module.store();
  for (auto&& import : module->desc().imports) {
    if (import.type.type->kind != ExternKind::Func) {
      stream->Writef("wasi error: invalid import type: %s\n",
                     import.type.name.c_str());
      return Result::Error;
    }

    if (import.type.module != "wasi_snapshot_preview1") {
      stream->Writef("wasi error: unknown module import: %s\n",
                     import.type.module.c_str());
      return Result::Error;
    }

    auto func_type = *cast<FuncType>(import.type.type.get());
    auto import_name = StringPrintf("%s.%s", import.type.module.c_str(),
                                    import.type.name.c_str());
    HostFunc::Ptr host_func;
    if (import.type.name == "proc_exit") {
      host_func = HostFunc::New(*store, func_type,
                                [=](const Values& params, Values& results,
                                    Trap::Ptr* trap) -> Result {
                                  if (trace_stream) {
                                    trace_stream->Writef(
                                        ">>> running wasi function \"%s\":\n",
                                        import.type.name.c_str());
                                  }
                                  const Value return_code = params[0];
                                  uvwasi_proc_exit(uvwasi, return_code.i32_);
                                  return Result::Ok;
                                });
    } else {
      stream->Writef("unknown wasi_snapshot_preview1 import: %s\n",
                     import.type.name.c_str());
      return Result::Error;
    }
    imports.push_back(host_func.ref());
  }

  return Result::Ok;
}

Result WasiRunStart(const Instance::Ptr& instance,
                    Stream* stream,
                    Stream* trace_stream) {
  Store* store = instance.store();
  auto module = store->UnsafeGet<Module>(instance->module());
  auto&& module_desc = module->desc();
  for (auto&& export_ : module_desc.exports) {
    if (export_.type.name != "_start") {
      continue;
    }
    if (export_.type.type->kind != ExternalKind::Func) {
      stream->Writef("wasi error: _start export is not a function\n");
      return Result::Error;
    }
    auto* func_type = cast<FuncType>(export_.type.type.get());
    if (func_type->params.size() || func_type->results.size()) {
      stream->Writef("wasi error: invalid _start signature\n");
      return Result::Error;
    }
    Values params;
    Values results;
    Trap::Ptr trap;
    auto func = store->UnsafeGet<Func>(instance->funcs()[export_.index]);
    Result res = func->Call(*store, params, results, &trap, trace_stream);
    if (trap) {
      WriteTrap(stream, " error", trap);
    }
    return res;
  }

  stream->Writef("wasi error: _start function not found\n");
  return Result::Error;
}

}  // namespace interp
}  // namespace wabt

#endif
