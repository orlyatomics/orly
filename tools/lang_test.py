#!/usr/bin/env python3
# Copyright 2010-2026 Atomic Kismet Company
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import itertools
import multiprocessing
import json
import os
import os.path
import re
import subprocess

# Process object. wraps subprocess.Popen object to augment it with a filename.
class TProcess(object):
  def __init__(self, filepath, process):
    self.filepath = filepath
    self.process = process
    self.pid = process.pid
    self.stdout = process.stdout

  def poll(self):
    return self.process.poll()

  def returncode(self):
    return self.process.returncode

# Bounded worker pool for orlyc invocations. Replaces the original
# littleworkers.Pool dependency, which is unmaintained and has a
# Python 3 typing bug in its poll loop.
class TPool(object):
    def __init__(self, out_dir, workers):
        self.__out_dir = out_dir
        self.__workers = max(1, int(workers))

    def create_process(self, filepath):
        # ORLYC overrides the compiler binary; ORLYC_EXTRA_ARGS appends flags
        # (e.g. ORLYC_EXTRA_ARGS=--test-engine=indy to run tests on indy, #262).
        orlyc = os.environ.get('ORLYC', '../out_orly/debug/orly/orlyc')
        extra = os.environ.get('ORLYC_EXTRA_ARGS', '')
        return TProcess(
                 filepath,
                 subprocess.Popen(
                     orlyc + ' -m -d ' + filepath + ' -o ' + self.__out_dir + ' ' + extra,
                     shell=True,
                     stderr=subprocess.STDOUT,
                     stdout=subprocess.PIPE))

    def run(self, filepaths, callback):
        import time
        pending = list(filepaths)
        in_flight = []  # list of TProcess
        while pending or in_flight:
            while pending and len(in_flight) < self.__workers:
                in_flight.append(self.create_process(pending.pop(0)))
            # Reap any that finished. Polling rather than os.wait so we
            # don't race subprocess.Popen's own bookkeeping; it would
            # otherwise see ECHILD and set returncode to 0 incorrectly.
            still_running = []
            made_progress = False
            for proc in in_flight:
                if proc.poll() is None:
                    still_running.append(proc)
                else:
                    callback(proc)
                    made_progress = True
            in_flight = still_running
            if not made_progress and in_flight:
                time.sleep(0.05)

def GetArgParser():
  default_worker_count = multiprocessing.cpu_count()

  # Parse command line arguments
  parser = argparse.ArgumentParser(description='Report changes in tests')
  parser.add_argument('filepaths', type=str, nargs='*',
      help='The list of orlyscript files to use, defaults to all *.orly files in the current directory')
  parser.add_argument('--update', '-u', action='store_true', help='Update the state files for the given orlyscript files')
  parser.add_argument('--changes', '-c', action='store_true', help='Print out the changes')
  parser.add_argument('--directories', '-d', action='store_true', help='Treat filepaths as directories containing files rather than files');
  parser.add_argument(
      '--worker-count', '-w',
      default=default_worker_count,
      type=int,
      help='Specify the number of workers for the script. (default: ' + str(default_worker_count) + ')')
  return parser

# Compiler diagnostics embed the THROW site's source location via the HERE
# macro -- e.g. `@["orly/code_gen/builder.cc":259]` (errors) or
# `[orly/compiler.cc, 202]` (compile failures). Baselines captured that line
# number verbatim, so any edit shifting lines in a file named in a diagnostic
# silently invalidated the checked-in `.state` (it bit #73; #75's baseline had
# drifted 259 -> 282). Strip just the line number, keeping the filename (which
# still says which check fired). Test-file positions like `19:4-19:5` have no
# bracketed path and are left intact.
_SRC_LOC_QUOTED = re.compile(r'(\["[^"]+"):\d+\]')                       # ["path":NNN] -> ["path"]
_SRC_LOC_BARE = re.compile(r'(\[[^][,]+\.(?:cc|h|hh|hpp|cpp|cxx)), \d+\]')  # [path.cc, NNN] -> [path.cc]

def NormalizeSourceLocations(text):
  text = _SRC_LOC_QUOTED.sub(r'\1]', text)
  text = _SRC_LOC_BARE.sub(r'\1]', text)
  return text

def GetResult(proc):
  output = proc.stdout.read()
  if isinstance(output, bytes):
    output = output.decode('utf-8', errors='replace')
  output = NormalizeSourceLocations(output)
  sections = [x.splitlines() for x in output.split('MM_NOTICE: ')]
  return proc.returncode(), sections

def GetStateFilename(filepath):
  dirpath, filename = os.path.split(filepath)
  return os.path.join(dirpath, '.' + filename + '.test.state')

def LoadXFail(directory):
  """Load expected-failure paths from <directory>/.xfail.

  Returns a dict mapping the normalised filepath (as it will appear in
  the harness's collected list) -> list of issue refs (may be empty).

  File format: one entry per line. Blank lines and lines whose first
  non-whitespace token starts with '# ' (hash + space) are comments.
  Each entry is a path relative to <directory>, optionally followed by
  whitespace-separated '#NNN' issue refs."""
  xfail_path = os.path.join(directory, '.xfail')
  entries = {}
  try:
    f = open(xfail_path)
  except (IOError, OSError) as ex:
    if ex.errno != 2:
      raise
    return entries
  with f:
    for raw_line in f:
      line = raw_line.strip()
      # Any line starting with '#' is a comment (a bare '#' spacer line used
      # to parse as a phantom entry); real entries are paths, which never
      # start with '#'.
      if not line or line.startswith('#'):
        continue
      parts = line.split()
      rel_path = parts[0]
      issues = [p for p in parts[1:] if p.startswith('#')]
      # Absolute keys: in file mode the .xfail walk-up runs on abspaths while
      # the harness's collected filepaths stay as given, so relative keys
      # would never match and every xfail would report as unexpected.
      key = os.path.abspath(os.path.join(directory, rel_path))
      entries[key] = issues
  return entries

def BuildXFailMap(input_paths, treat_as_directories):
  """Build the merged xfail map from every .xfail file reachable from
  the input paths. In --directories mode each input is itself a
  directory to scan; otherwise we walk up from each file's containing
  directory until we find an .xfail (or run out of parents)."""
  merged = {}
  seen_dirs = set()

  def _absorb(directory):
    directory = os.path.normpath(directory)
    if directory in seen_dirs:
      return
    seen_dirs.add(directory)
    merged.update(LoadXFail(directory))

  if treat_as_directories:
    for path in input_paths:
      _absorb(path)
  else:
    for path in input_paths:
      directory = os.path.dirname(os.path.abspath(path))
      while True:
        if os.path.exists(os.path.join(directory, '.xfail')):
          _absorb(directory)
          break
        parent = os.path.dirname(directory)
        if parent == directory:
          break
        directory = parent
  return merged

def Main():
  args = GetArgParser().parse_args()

  # We should use mkdtemp, but that creates a new directory name for every build, giving us false positives for output changing.
  # out_dir = tempfile.mkdtemp(prefix = "orly") # Temp Directory
  out_dir = '/tmp/orly_compiler/'
  try:
    os.mkdir(out_dir)
  except OSError:
    pass

  # Singleton instance of our process pool
  lil = TPool(out_dir, workers=args.worker_count)



  # List of filepaths.
  filepaths = list();

  if args.directories:
    for path in args.filepaths:
      for dirname, _subdirs, filenames in os.walk(path):
        for filename in filenames:
          if os.path.splitext(filename)[-1] == '.orly':
            filepaths.append(os.path.join(dirname, filename))
  else:
    filepaths = args.filepaths

  xfail_map = BuildXFailMap(args.filepaths, args.directories)

  def IsXFail(filepath):
    return os.path.abspath(filepath) in xfail_map

  def XFailIssues(filepath):
    return xfail_map.get(os.path.abspath(filepath), [])

  changed_files = []
  passed_files = []
  failed_files = []

  def Callback(proc):
    # Get the result
    result = GetResult(proc)
    filepath = proc.filepath
    if args.update:
      # Baselines are stored as readable JSON (not pickle) so that a changed
      # baseline shows up in `git diff` / PR review and an uncommitted
      # regeneration shows as a dirty working tree.
      with open(GetStateFilename(filepath), 'w') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
      return
    else:
      try:
        # json.load yields lists; wrap in tuple so the `result == expected`
        # comparison below matches GetResult's (returncode, sections) tuple.
        with open(GetStateFilename(filepath), 'r') as f:
          expected = tuple(json.load(f))
      except (IOError, OSError) as ex:
        if ex.errno != 2:
          raise
        if result[0] != 0:
          print('New failure:', filepath)
          print('Exited with code', result[0])
          print('OUTPUT: ')
          print('\n'.join('\n'.join(x) for x in result[1]))
          changed_files.append(filepath)
          failed_files.append(filepath)
        else:
          passed_files.append(filepath)
        return

      # Did we pass?
      passed = result[0] == 0

      if result == expected:
        if passed:
          passed_files.append(filepath)
        else:
          failed_files.append(filepath)
        return

      returncode, section = result
      expected_returncode, expected_section = expected

      print('====================================================================')
      print('Changes:', filepath)

      acceptable_change = True

      def _label(section_val):
        return section_val[0] if section_val else '(empty)'

      for (expected_val, val) in itertools.zip_longest(expected_section, section):
        if expected_val == val:
          continue
        elif expected_val is None:
          acceptable_change = False
          print('Further:', _label(val))
          if args.changes:
            print('\n'.join(val))
        elif val is None:
          acceptable_change = False
          print('Worse:', _label(expected_val))
        else:
          print('Differs:', _label(expected_val))
          if args.changes:
            print('Old:')
            if len(expected_val) > 0:
              print('\n'.join(expected_val[1:]))
            print('New:')
            if len(val) > 0:
              print('\n'.join(val[1:]))

      if returncode != expected_returncode:
        acceptable_change = False
        print('Return code from', expected_returncode, 'to', returncode)

      if not acceptable_change:
        changed_files.append(filepath)

      if passed:
        passed_files.append(filepath)
      else:
        failed_files.append(filepath)
      return

  if len(filepaths) != 0:
    lil.run(filepaths, callback=Callback)

  if args.update:
    return 0

  # Split failures into tracked (xfail) vs unexpected, and detect any
  # tests that were marked xfail but actually passed (xpass). xpass is
  # treated as a failure to match pytest's xfail_strict=true semantics:
  # either the underlying limitation is fixed and the entry should be
  # removed, or the test is no longer exercising what it claims to.
  unexpected_failures = [f for f in failed_files if not IsXFail(f)]
  tracked_failures = [f for f in failed_files if IsXFail(f)]
  unexpected_passes = [f for f in passed_files if IsXFail(f)]

  print('Change:', ','.join(changed_files))
  print('Pass:', ','.join(passed_files))
  print('Fail:', ','.join(failed_files))
  if unexpected_passes:
    print('XPass:', ','.join(unexpected_passes))

  # 'passed' counts tests that genuinely passed and were not marked
  # xfail; xpasses are reported in their own bucket so each test sits
  # in exactly one column.
  pure_passed = len(passed_files) - len(unexpected_passes)
  summary = 'Overall: %d unexpected, %d passed, %d tracked failures (xfail)' % (
      len(unexpected_failures), pure_passed, len(tracked_failures))
  if unexpected_passes:
    noun = 'pass' if len(unexpected_passes) == 1 else 'passes'
    summary += ', %d unexpected %s (xpass)' % (len(unexpected_passes), noun)
  print(summary)

  for filepath in tracked_failures:
    issues = XFailIssues(filepath)
    suffix = ' (%s)' % ' '.join(issues) if issues else ''
    print('  xfail:', filepath + suffix)
  for filepath in unexpected_passes:
    issues = XFailIssues(filepath)
    suffix = ' (%s)' % ' '.join(issues) if issues else ''
    print('  xpass:', filepath + suffix,
          '-- remove from .xfail or update the test')

  ok = (not changed_files) and (not unexpected_failures) and (not unexpected_passes)
  return 0 if ok else -1

if __name__ == '__main__':
  exit(Main())
