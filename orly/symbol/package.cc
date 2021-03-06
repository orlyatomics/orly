/* <orly/symbol/package.cc>

   Implements <orly/symbol/package.h>

   Copyright 2010-2014 OrlyAtomics, Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <orly/symbol/package.h>

#include <base/path.h>

using namespace Orly;
using namespace Orly::Package;
using namespace Orly::Symbol;

TPackage::TPtr TPackage::New(const Package::TName &name, const std::string &index_name, unsigned int version) {
  return TPackage::TPtr(new TPackage(name, index_name, version));
}

TPackage::TPackage(const TName &name, const std::string &index_name, unsigned int version)
    : Name(name), IndexName(index_name), Version(version) {
    assert(Base::IsValidNamespace(name.Name));
}

const Package::TName &TPackage::GetName() const {
  assert(this);
  return Name;
}


const std::string &TPackage::GetIndexName() const {
  assert(this);
  return IndexName;
}

unsigned int TPackage::GetVersion() const {
  assert(this);
  return Version;
}
