/* <orly/package/loaded.cc>

   Implementes <orly/package/loaded.h>

   Copyright 2010-2026 Atomic Kismet Company

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <orly/package/loaded.h>

#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <syslog.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include <base/as_str.h>
#include <base/thrower.h>
#include <orly/package/api.h>

using namespace Base;
using namespace Jhm;
using namespace std;
using namespace Orly::Package;

static void *IfNull(const Base::TCodeLocation &code_location, void *handle) {
  if(!handle) {
    throw TDlError(code_location, dlerror());
  }
  return handle;
}

namespace {

  /* Mangled-name prefix of every member of Orly::Native::Record<...>. */
  constexpr string_view RecordPrefix = "_ZN4Orly6Native6RecordI";

  /* The unique Record<...> symbols of every package loaded into this process
     (#797). A library with a unique symbol is never unloaded, so names only
     ever join this set. */
  mutex UniqueRecordMutex;
  unordered_set<string> UniqueRecordSymbols;

  /* T must be trivially copyable; false if [offset, offset + sizeof(T)) is
     outside `image`. */
  template <typename T>
  bool ReadAt(const string &image, uint64_t offset, T &out) {
    if (offset > image.size() || image.size() - offset < sizeof(T)) {
      return false;
    }
    memcpy(&out, image.data() + offset, sizeof(T));
    return true;
  }

  /* The record type T of a mangled Orly::Native::Record<T> member, for an
     error message; the mangled name itself if it won't demangle. */
  string GetRecordTypeName(const string &mangled) {
    int status = 0;
    char *demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    string result = (status == 0 && demangled) ? demangled : mangled;
    free(demangled);
    const string open = "Orly::Native::Record<";
    const auto start = result.find(open), limit = result.rfind(">::");
    if (start != string::npos && limit != string::npos && limit > start + open.size()) {
      result = result.substr(start + open.size(), limit - start - open.size());
    }
    return result;
  }

}  // anonymous namespace

vector<string> Orly::Package::GetUniqueRecordSymbols(const string &path) {
  vector<string> result;
  ifstream in(path, ios::binary);
  const string image((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
  Elf64_Ehdr ehdr;
  if (!ReadAt(image, 0, ehdr) || memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
      ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_shentsize != sizeof(Elf64_Shdr)) {
    return result;
  }
  for (uint16_t sec_idx = 0; sec_idx < ehdr.e_shnum; ++sec_idx) {
    Elf64_Shdr symtab, strtab;
    if (!ReadAt(image, ehdr.e_shoff + uint64_t(sec_idx) * sizeof(Elf64_Shdr), symtab) ||
        symtab.sh_type != SHT_DYNSYM || symtab.sh_entsize != sizeof(Elf64_Sym) ||
        !ReadAt(image, ehdr.e_shoff + uint64_t(symtab.sh_link) * sizeof(Elf64_Shdr), strtab) ||
        strtab.sh_offset > image.size() || image.size() - strtab.sh_offset < strtab.sh_size) {
      continue;
    }
    const string_view names(image.data() + strtab.sh_offset, strtab.sh_size);
    for (uint64_t sym_off = 0; sym_off + sizeof(Elf64_Sym) <= symtab.sh_size; sym_off += sizeof(Elf64_Sym)) {
      Elf64_Sym sym;
      if (!ReadAt(image, symtab.sh_offset + sym_off, sym)) {
        break;
      }
      if (ELF64_ST_BIND(sym.st_info) != STB_GNU_UNIQUE || sym.st_shndx == SHN_UNDEF || sym.st_name >= names.size()) {
        continue;
      }
      const string_view name = names.substr(sym.st_name, names.find('\0', sym.st_name) - sym.st_name);
      if (name.starts_with(RecordPrefix)) {
        result.emplace_back(name);
      }
    }
  }
  return result;
}

TLoaded::TPtr TLoaded::Load(const Jhm::TTree &package_dir, const TVersionedName &name) {
  return TPtr(new TLoaded(package_dir, name));
}

TLoaded::~TLoaded() {
  /* dlclose can fail (it returns nonzero and sets dlerror); from a dtor the
     only honest response is to say so loudly (#356). */
  if (dlclose(Handle) != 0) {
    const char *err = dlerror();
    syslog(LOG_ERR, "package [%s]: dlclose failed: %s", AsStr(Name).c_str(), err ? err : "(no dlerror)");
  }
}

bool TLoaded:: ForEachTest(const function<bool (const TTest *)> &cb) const {

  for(auto &it: LinkInfo->PrimaryInfo->Tests) {
    if(!cb(it)) {
      return false;
    }
  }

  return true;
}

TFuncHolder::TPtr TLoaded::GetFunctionInfo(const Base::TPiece<const char> &func) const {
  string func_name(func.GetStart(), func.GetLimit());
  auto iter = LinkInfo->PrimaryInfo->Functions.find(func_name);
  if (iter == LinkInfo->PrimaryInfo->Functions.end()) {
    DEFINE_ERROR(error_t, runtime_error, "unknown function name");
    THROW_ERROR(error_t) << '"' << func_name << '"';
  }
  return TFuncHolder::TPtr(new TFuncHolder(shared_from_this(), iter->second));
}

bool TLoaded::ForEachFunction(
    const std::function<bool(const std::string &name, const std::shared_ptr<const TFuncHolder> &func)> &cb) const {
  for (const auto &func : LinkInfo->PrimaryInfo->Functions) {
    if (!cb(func.first, GetFunctionInfo(AsPiece(func.first)))) {
      return false;
    }
  }
  return true;
}

const TVersionedName &TLoaded::GetName() const {
  return Name;
}

const TIndexByIndexId &TLoaded::GetIndexByIndexId() const {
  return LinkInfo->IndexByIndexId;
}

const TIndexIdSet &TLoaded::GetIndexIdSet() const {
  return LinkInfo->PrimaryInfo->IndexIdSet;
}

const std::string &TLoaded::GetIndexPrefix() const {
  return LinkInfo->IndexName;
}

bool TLoaded::ForEachIndexId(const std::function <bool (Base::TUuid *)> &cb) const {
  for (auto id : LinkInfo->PrimaryInfo->IndexIdSet) {
    if (!cb(id)) {
      return false;
    }
  }
  return true;
}

TLoaded::TLoaded(const Jhm::TTree &package_dir, const TVersionedName &name) : Name(name) {
  string filename = AsStr(package_dir.GetAbsPath(name.GetSoRelPath()));

  /* Refuse a package that would share a record registry with one already
     loaded (#797): see GetUniqueRecordSymbols(). Packages compiled since then
     keep their registries hidden, and only ever report none. */
  const vector<string> unique_record_symbols = GetUniqueRecordSymbols(filename);
  lock_guard<mutex> unique_record_lock(UniqueRecordMutex);
  for (const auto &symbol : unique_record_symbols) {
    if (UniqueRecordSymbols.contains(symbol)) {
      ostringstream oss;
      oss << "Package '" << AsStr(name) << "' was compiled by an orlyc older than the fix for #797, and an"
          << " already loaded package compiled the same way declares the same record type ("
          << GetRecordTypeName(symbol) << "). Loading both would corrupt that record type and crash the server on"
          << " its next read. Recompile the package with this release's orlyc.";
      throw TLoaderError(HERE, oss.str().c_str());
    }
  }

  Handle = IfNull(HERE, dlopen(filename.c_str(), RTLD_NOW));
  /* Loaded now, and never unloaded while it has a unique symbol, whatever
     happens below. */
  UniqueRecordSymbols.insert(unique_record_symbols.begin(), unique_record_symbols.end());

  try {

    int32_t api_verno = reinterpret_cast<int32_t (*)()>(IfNull(HERE, dlsym(Handle, "GetApiVersion")))();
    if(ORLY_API_VERSION != api_verno) {
      throw TLoaderError(HERE, "Package API version doesn't match server API version");
    }

    LinkInfo = reinterpret_cast<TLinkInfo*>(reinterpret_cast<TLinkInfo *(*)()>(IfNull(HERE, dlsym(Handle, "GetLinkInfo")))());

    assert(LinkInfo);
    if(LinkInfo->PrimaryName != AsStr(name.Name)) {
      std::ostringstream oss;
      oss << "Package name inside the package doesn't match the filename. It is illegal to rename Orly package '.so' files. Named '" << LinkInfo->PrimaryName << "' in package, expected '" << AsStr(name.Name) << "'.";
      throw TLoaderError(HERE, oss.str().c_str());
    }

    if(LinkInfo->PrimaryVersion != name.Version) {
      throw TLoaderError(HERE, "Package version inside the package doesn't match the filename. It is illegal to rename Orly package '.so' files.");
    }

  } catch (...) {
    if (dlclose(Handle) != 0) {
      const char *err = dlerror();
      syslog(LOG_ERR, "package [%s]: dlclose failed during failed load: %s", AsStr(name).c_str(), err ? err : "(no dlerror)");
    }
    throw;
  }
}

const TParamMap &TFuncHolder::GetParameters() const {

  return Func->Parameters;
}

const Orly::Type::TType &TFuncHolder::GetReturnType() const {

  return Func->ReturnType;
}

Orly::Atom::TCore TFuncHolder::Call(TContext &ctx, const TArgMap &args) const {

  return (Func->Runner)(ctx, args);
}

TFuncHolder::TFuncHolder(const TLoaded::TPtr &package, const TFuncInfo *func) : Package(package), Func(func) {
  assert(package);
  assert(func);
}
