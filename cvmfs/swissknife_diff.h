/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_SWISSKNIFE_DIFF_H_
#define CVMFS_SWISSKNIFE_DIFF_H_

#include <stdint.h>

#include <string>

#include "history.h"
#include "shortstring.h"
#include "swissknife.h"

namespace catalog {
class SimpleCatalogManager;
}

namespace swissknife {

class CommandDiff : public Command {
 public:
  CommandDiff() {}
  ~CommandDiff();
  virtual std::string GetName() const { return "diff"; }
  virtual std::string GetDescription() const {
    return "Show changes between two revisions";
  }
  ParameterList GetParams() const;
  int Main(const ArgumentList &args);
};  // class CommandDiff

}  // namespace swissknife

#endif  // CVMFS_SWISSKNIFE_DIFF_H_
