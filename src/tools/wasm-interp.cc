/*
 * Copyright 2016 WebAssembly Community Group participants
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

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#ifdef WITH_WASI
#include "uvwasi.h"
#endif

#include "src/binary-reader.h"
#include "src/error-formatter.h"
#include "src/feature.h"
#include "src/interp/binary-reader-interp.h"
#include "src/interp/interp-util.h"
#include "src/interp/interp.h"
#include "src/option-parser.h"
#include "src/stream.h"

using namespace wabt;
using namespace wabt::interp;

static int s_verbose;
static const char* s_infile;
static Thread::Options s_thread_options;
static Stream* s_trace_stream;
static bool s_run_all_exports;
static bool s_host_print;
static bool s_dummy_import_func;
static Features s_features;
#ifdef WITH_WASI
static bool s_wasi;
#endif

static std::unique_ptr<FileStream> s_log_stream;
static std::unique_ptr<FileStream> s_stdout_stream;

static Store s_store;

static const char s_description[] =
    R"(  read a file in the wasm binary format, and run in it a stack-based
  interpreter.

examples:
  # parse binary file test.wasm, and type-check it
  $ wasm-interp test.wasm

  # parse test.wasm and run all its exported functions
  $ wasm-interp test.wasm --run-all-exports

  # parse test.wasm, run the exported functions and trace the output
  $ wasm-interp test.wasm --run-all-exports --trace

  # parse test.wasm and run all its exported functions, setting the
  # value stack size to 100 elements
  $ wasm-interp test.wasm -V 100 --run-all-exports
)";

static void ParseOptions(int argc, char** argv) {
  OptionParser parser("wasm-interp", s_description);

  parser.AddOption('v', "verbose", "Use multiple times for more info", []() {
    s_verbose++;
    s_log_stream = FileStream::CreateStdout();
  });
  s_features.AddOptions(&parser);
  parser.AddOption('V', "value-stack-size", "SIZE",
                   "Size in elements of the value stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.value_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('C', "call-stack-size", "SIZE",
                   "Size in elements of the call stack",
                   [](const std::string& argument) {
                     // TODO(binji): validate.
                     s_thread_options.call_stack_size = atoi(argument.c_str());
                   });
  parser.AddOption('t', "trace", "Trace execution",
                   []() { s_trace_stream = s_stdout_stream.get(); });
#ifdef WITH_WASI
  parser.AddOption("wasi",
                   "Assume input module is WASI compliant (Export "
                   " WASI API the the module and invoke _start function)",
                   []() { s_wasi = true; });
#endif
  parser.AddOption(
      "run-all-exports",
      "Run all the exported functions, in order. Useful for testing",
      []() { s_run_all_exports = true; });
  parser.AddOption("host-print",
                   "Include an importable function named \"host.print\" for "
                   "printing to stdout",
                   []() { s_host_print = true; });
  parser.AddOption(
      "dummy-import-func",
      "Provide a dummy implementation of all imported functions. The function "
      "will log the call and return an appropriate zero value.",
      []() { s_dummy_import_func = true; });

  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) { s_infile = argument; });
  parser.Parse(argc, argv);
}

Result RunAllExports(const Instance::Ptr& instance, Errors* errors) {
  Result result = Result::Ok;

  auto module = s_store.UnsafeGet<Module>(instance->module());
  auto&& module_desc = module->desc();

  for (auto&& export_ : module_desc.exports) {
    if (export_.type.type->kind != ExternalKind::Func) {
      continue;
    }
    auto* func_type = cast<FuncType>(export_.type.type.get());
    if (func_type->params.empty()) {
      if (s_trace_stream) {
        s_trace_stream->Writef(">>> running export \"%s\":\n",
                               export_.type.name.c_str());
      }
      auto func = s_store.UnsafeGet<Func>(instance->funcs()[export_.index]);
      Values params;
      Values results;
      Trap::Ptr trap;
      result |= func->Call(s_store, params, results, &trap, s_trace_stream);
      WriteCall(s_stdout_stream.get(), export_.type.name, *func_type, params,
                results, trap);
    }
  }

  return result;
}

#ifdef WITH_WASI
Result RunWasiStart(const Instance::Ptr& instance, Errors* errors) {
  auto* stream = s_stdout_stream.get();

  auto module = s_store.UnsafeGet<Module>(instance->module());
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
    auto func = s_store.UnsafeGet<Func>(instance->funcs()[export_.index]);
    Result res = func->Call(s_store, params, results, &trap, s_trace_stream);
    if (trap) {
      WriteTrap(stream, " error", trap);
    }
    return res;
  }

  stream->Writef("wasi error: _start function not found\n");
  return Result::Error;
}
#endif

Result ReadAndInstantiateModule(const char* module_filename,
                                Errors* errors,
                                Instance::Ptr* out_instance) {
  auto* stream = s_stdout_stream.get();
  std::vector<uint8_t> file_data;
  CHECK_RESULT(ReadFile(module_filename, &file_data));

  ModuleDesc module_desc;
  const bool kReadDebugNames = true;
  const bool kStopOnFirstError = true;
  const bool kFailOnCustomSectionError = true;
  ReadBinaryOptions options(s_features, s_log_stream.get(), kReadDebugNames,
                            kStopOnFirstError, kFailOnCustomSectionError);
  CHECK_RESULT(ReadBinaryInterp(file_data.data(), file_data.size(), options,
                                errors, &module_desc));

  if (s_verbose) {
    module_desc.istream.Disassemble(stream);
  }

  auto module = Module::New(s_store, module_desc);

#ifdef WITH_WASI
  uvwasi_t uvwasi;
  uvwasi_t* uvwasi_ptr = &uvwasi;

  if (s_wasi) {
    uvwasi_errno_t err;
    uvwasi_options_t init_options;
    /* Setup the initialization options. */
    init_options.in = 0;
    init_options.out = 1;
    init_options.err = 2;
    init_options.fd_table_size = 3;
    init_options.argc = 0;
    init_options.argv = NULL;
    init_options.envp = NULL;
    init_options.preopenc = 0;
    init_options.preopens = NULL;
    init_options.allocator = NULL;

    err = uvwasi_init(uvwasi_ptr, &init_options);
    if (err != UVWASI_ESUCCESS) {
      stream->Writef("error initialiazing uvwasi: %d\n", err);
      return Result::Error;
    }
  }
#endif

  RefVec imports;
  for (auto&& import : module_desc.imports) {
    if (import.type.type->kind == ExternKind::Func) {
#ifdef WITH_WASI
      if (s_wasi && import.type.module == "wasi_snapshot_preview1") {
        auto func_type = *cast<FuncType>(import.type.type.get());
        auto import_name = StringPrintf("%s.%s", import.type.module.c_str(),
                                        import.type.name.c_str());
        HostFunc::Ptr host_func;
        if (import.type.name == "proc_exit") {
          host_func = HostFunc::New(
              s_store, func_type,
              [=](const Values& params, Values& results,
                  Trap::Ptr* trap) -> Result {
                if (s_trace_stream) {
                  s_trace_stream->Writef(">>> running wasi function \"%s\":\n",
                                         import.type.name.c_str());
                }
                const Value return_code = params[0];
                uvwasi_proc_exit(uvwasi_ptr, return_code.i32_);
                return Result::Ok;
              });
        } else {
          stream->Writef("unknown wasi_snapshot_preview1 import: %s\n",
                         import.type.name.c_str());
          return Result::Error;
        }
        imports.push_back(host_func.ref());
        continue;
      }
#endif
      if (((s_host_print && import.type.module == "host" &&
            import.type.name == "print") ||
           s_dummy_import_func)) {
        auto func_type = *cast<FuncType>(import.type.type.get());
        auto import_name = StringPrintf("%s.%s", import.type.module.c_str(),
                                        import.type.name.c_str());

        auto host_func = HostFunc::New(
            s_store, func_type,
            [=](const Values& params, Values& results,
                Trap::Ptr* trap) -> Result {
              printf("called host ");
              WriteCall(stream, import_name, func_type, params, results, *trap);
              return Result::Ok;
            });
        imports.push_back(host_func.ref());
        continue;
      }
    }

    // By default, just push an null reference. This won't resolve, and
    // instantiation will fail.
    imports.push_back(Ref::Null);
  }

  RefPtr<Trap> trap;
  *out_instance = Instance::Instantiate(s_store, module.ref(), imports, &trap);
  if (!*out_instance) {
    // TODO: change to "initializing"
    WriteTrap(stream, "error initialiazing module", trap);
    return Result::Error;
  }

  return Result::Ok;
}

static Result ReadAndRunModule(const char* module_filename) {
  Errors errors;
  Instance::Ptr instance;
  Result result = ReadAndInstantiateModule(module_filename, &errors, &instance);
  if (Succeeded(result)) {
    if (s_run_all_exports) {
      RunAllExports(instance, &errors);
    }
#ifdef WITH_WASI
    if (s_wasi) {
      RunWasiStart(instance, &errors);
    }
#endif
  }
  FormatErrorsToFile(errors, Location::Type::Binary);
  return result;
}

int ProgramMain(int argc, char** argv) {
  InitStdio();
  s_stdout_stream = FileStream::CreateStdout();

  ParseOptions(argc, argv);

  wabt::Result result = ReadAndRunModule(s_infile);
  return result != wabt::Result::Ok;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
