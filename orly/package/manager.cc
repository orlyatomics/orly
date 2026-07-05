/* <orly/package/manager.cc>

   Implements <orly/package/manager.h>

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

#include <orly/package/manager.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <base/split.h>

#include <base/assert_true.h>
#include <base/thrower.h>

//Force anything that includes the package manager to link against the orly runtime
// Which forces everything in the orly runtime to be part of the linking environment.
#include <orly/rt.h>

using namespace Base;
using namespace std;
using namespace Orly::Package;

TManager::TManager(const Jhm::TTree &package_dir)
    : PackageDir(package_dir), Installed(std::make_shared<const TInstalled>()) {}

//When the installed map destructs, the shared pointers will naturally go away, unloading the packages.
TManager::~TManager() {}

TLoaded::TPtr TManager::Get(const TName &package) const {
  /* Lock-free: walk our own pinned snapshot (#356). */
  const auto installed = Installed.load();
  auto it = installed->find(package);
  if(it == installed->end()) {
    THROW_ERROR(TManager::TError) << "Cannot get non-installed package \"" << package << '"';
  }
  return it->second;
}

void TManager::Install(const TVersionedNames &packages, const std::function<void(TLoaded::TPtr, bool)> &pre_install_step) {
  Load(packages, pre_install_step);
}

void TManager::Load(const TVersionedNames &packages, const std::function<void(TLoaded::TPtr, bool)> &pre_install_step) {

  lock_guard<mutex> lock(WriteLock);

  TInstalled installed(*Installed.load());

  list<std::tuple<TLoaded::TPtr, bool>> about_to_install;

  //Ensure all package upgrades are actually upgrades, all files exist, build up map to swap in.
  //Errors are collected across the whole batch so a caller sees every failing
  //package, not just the first; nothing is installed unless every package loads.
  std::vector<std::string> errors;
  for(const TVersionedName &package: packages) {
    try {
      auto installed_it = installed.find(package.Name);
      if(installed_it != installed.end()) {
        if(installed_it->second->GetName().Version > package.Version) {
          THROW_ERROR(TManager::TError) << "Cannot downgrade already installed package '" << package <<'\'';
        }
        if(installed_it->second->GetName().Version == package.Version) {
          // If package has been previously installed at the same version, do nothing / noop.
          std::ostringstream oss;
          oss << package;
          syslog(LOG_INFO, "Package already installed at requested version [%s]. No-op.", oss.str().c_str());
          continue;
        }
        installed_it->second = TLoaded::Load(PackageDir, package);
        about_to_install.push_back(
            make_tuple(installed_it->second, true /* is_new_version */));
      } else {
        auto ret = installed.insert(
            make_pair(package.Name, TLoaded::Load(PackageDir, package)));
        about_to_install.push_back(
            make_tuple(ret.first->second, true /* is_new_version */));
      }
    } catch (const std::exception &ex) {
      errors.emplace_back(ex.what());
    }
  }
  if (!errors.empty()) {
    THROW_ERROR(TManager::TError)
        << "failed to load " << errors.size() << " package(s): "
        << Join(errors, "; ");
  }

  // Call back with each installed package so outside systems can do what they need to
  // NOTE: This doesn't get rolled back if the callback throws partway through the list...
  for(auto &package: about_to_install) {
    pre_install_step(std::get<0>(package), std::get<1>(package));
  }

  // Guaranteed no-throw / the transaction will complete
  Installed.store(std::make_shared<const TInstalled>(std::move(installed)));
}

const Jhm::TTree &TManager::GetPackageDir() const {
  return PackageDir;
}

void TManager::SetPackageDir(const Jhm::TTree &package_dir) {
  PackageDir = package_dir;
}


void TManager::Uninstall(const TVersionedNames &packages) {

  lock_guard<mutex> lock(WriteLock);
  TInstalled installed(*Installed.load());

  for(const TVersionedName &package: packages) {
    auto installed_it = installed.find(package.Name);
    if(installed_it == installed.end()) {
      THROW_ERROR(TManager::TError) << "Cannot uninstall package '" << package << "' because it is not installed";
    }
    installed.erase(installed_it);
  }

  Installed.store(std::make_shared<const TInstalled>(std::move(installed)));
}

void TManager::YieldInstalled(std::function<bool (const TVersionedName &name)> cb) const {
  /* Lock-free, and the callback runs against our own pinned snapshot, so it
     can take as long as it likes without holding up an install (#356). */
  const auto installed = Installed.load();
  for(const auto &it: *installed) {
    if(!cb(it.second->GetName())) {
      break;
    }
  }
}
